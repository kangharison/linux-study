/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_INTERNAL_H
#define BLK_INTERNAL_H

#include <linux/bio-integrity.h>  /* [한국어] bio_integrity_payload 등 T10-PI/DIF/DIX 자료구조 — NVMe PI(Protection Information) Guard/APP/REF 태그가 CQE와 함께 검증되는 경로에 사용 */
#include <linux/blk-crypto.h>     /* [한국어] blk_crypto_key/keyslot API — NVMe SED/TCG Opal 또는 inline encryption에서 write 시 key 선택, read 시 key unwrap에 사용 */
#include <linux/lockdep.h>        /* [한국어] rwsem_acquire/release 등 lockdep 매크로 — request_queue freeze/PM lockdep map으로 NVMe reset 중 교착(deadlock)을 정적으로 검출 */
#include <linux/memblock.h>	/* [한국어] max_pfn/max_low_pfn 참조용 — NVMe DMA/PRP(Physical Region Page) 물리 주소 상한을 결정할 때 사용(추정) */
#include <linux/sched/sysctl.h>   /* [한국어] sysctl_hung_task_timeout_secs — NVMe command가 오래 멈췄을 때 hung_task watchdog 오탐을 피하기 위한 대기 주기 계산 기준 */
#include <linux/timekeeping.h>    /* [한국어] ktime_get_ns() — NVMe I/O latency 측정 및 doorbell 발행 타이밍 기록에 사용되는 단조 시각 소스 */
#include <xen/xen.h>              /* [한국어] xen_domain()/xen_biovec_phys_mergeable() — Xen 게스트 환경에서 grant table 경계로 인해 NVMe PRP/SGL 병합이 불가능한 경우를 추가로 판단 */
#include "blk-crypto-internal.h"  /* [한국어] blk-crypto 내부 API(bio_crypt_rq_ctx_compatible 등) — 두 bio/request의 암호화 컨텍스트가 같아 병합 가능한지 판단할 때 필요 */

/*
 * [한국어 설명] block/blk.h — 블록 계층 내부(private) 공용 헤더 (block/blk.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 include/linux/blkdev.h로 외부(디바이스 드라이버)에 노출되지 않는,
 * block/ 디렉터리 아래의 .c 파일들끼리만 공유하는 "블록 계층 내부 API"를
 * 모아 둔다. request_queue의 생명주기 제어(진입/동결/배수), bio 병합·분할
 * 내부 함수, PREFLUSH/FUA 시퀀싱을 위한 blk_flush_queue 상태머신, elevator
 * (IO 스케줄러) 내부 인터페이스, zone(ZNS) 내부 헬퍼, block integrity(T10
 * PI/DIF/DIX) 내부 훅, request 참조 카운트(req_ref_*), timeout, debugfs
 * lock, partition/gendisk 내부 API 등을 선언한다. NVMe/SCSI 등 어떤
 * 디바이스 드라이버도 이 헤더를 직접 include하지 않으며, 오직 block/*.c
 * 번역 단위만 "blk.h"로 include해 사용한다. 즉 이 파일은 블록 계층의
 * "구현 세부사항"이고, 드라이버가 보는 공개 API(include/linux/blkdev.h)와는
 * 명확히 분리된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 I/O 파이프라인에서, 이 헤더가 선언하는 함수들은 파일시스템이 제출한
 * bio가 NVMe SQ(Submission Queue) 엔트리로 변환되기 전의 "블록 계층 내부
 * 단계"에 위치한다. 대표 호출 흐름:
 *   submit_bio -> submit_bio_noacct -> __bio_split_to_limits(본 파일,
 *   queue_limits 초과 시 분할) -> blk_mq_submit_bio ->
 *   blk_attempt_plug_merge/blk_try_merge(본 파일, 병합 시도) ->
 *   blk_insert_flush(본 파일, PREFLUSH/FUA 시퀀싱 진입) ->
 *   blk_mq_get_request -> mq_ops->queue_rq (간접 호출; NVMe PCIe 면 nvme_queue_rq -> nvme_sq_copy_cmd -> nvme_write_sq_db).
 * 완료 경로는 역순으로 진행된다: NVMe CQ 완료 인터럽트 -> nvme_complete_rq
 * -> blk_mq_end_request -> req_ref_put_and_test(본 파일, rq 참조 해제) ->
 * bio_endio. 실행 컨텍스트는 함수별로 다르다 — 제출 경로 대부분은 프로세스
 * 컨텍스트(태스크 또는 kblockd workqueue)이고, 완료 경로는 NVMe 인터럽트/
 * softirq 컨텍스트이며, freeze/drain 계열은 컨트롤러 reset/제거를 수행하는
 * 별도 커널 스레드에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: linux/bio-integrity.h(T10 PI), linux/blk-crypto.h와
 * blk-crypto-internal.h(인라인 암호화), linux/lockdep.h(freeze/PM lockdep),
 * linux/memblock.h(DMA 물리 주소 상한), linux/sched/sysctl.h(hung task),
 * linux/timekeeping.h(타임스탬프), xen/xen.h(Xen 게스트 DMA gap).
 * 피의존 모듈: block/blk-core.c, blk-mq.c, blk-merge.c, blk-flush.c,
 * blk-mq-sched.c, elevator.c, blk-zoned.c, blk-integrity.c, bio.c, genhd.c,
 * blk-ioc.c, blk-timeout.c, bdev.c, ioctl.c, blk-settings.c 등 block/ 아래
 * 거의 모든 번역 단위가 "blk.h"를 include해 이 파일의 선언을 사용한다.
 * 데이터 흐름 관점에서 bio의 bi_iter(SLBA/길이)/bi_io_vec(페이지 배열)은
 * 이 헤더의 병합/분할 함수(bio_split_rw, blk_try_merge,
 * biovec_phys_mergeable 등)를 거쳐 request의 nr_phys_segments와
 * NVMe PRP/SGL 후보 개수로 정리된 뒤 드라이버로 전달된다. 공유하는 핵심
 * 자료구조는 struct request_queue(q_usage_counter, limits, elevator),
 * struct request(rq_flags, ref, bio 리스트), struct bio(bi_iter,
 * bi_io_vec), struct blk_flush_queue(PREFLUSH/FUA 상태머신)이며, 이들의
 * "내부" 필드에 접근하는 함수는 오직 이 헤더를 통해서만 다른 block/*.c로
 * 노출된다.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_try_enter_queue() / bio_queue_enter() : q_usage_counter 기반 큐 진입
 *   참조 획득 — freeze/dying 상태에서는 -ENODEV로 진입 차단.
 * blk_queue_start_drain() / __blk_freeze_queue_start() : NVMe reset/remove
 *   시 큐를 dying/freeze 상태로 전환해 신규 IO 진입을 막고 배수(drain).
 * bio_attempt_back_merge() / blk_try_merge() / blk_attempt_plug_merge() :
 *   bio-request 병합 가능 여부 판정 및 실제 병합 수행.
 * __bio_split_to_limits() / bio_split_rw() / bio_split_discard() 등 :
 *   queue_limits(MDTS, max_segments 등) 초과 시 bio를 분할.
 * blk_insert_flush() : PREFLUSH/FUA 요청을 blk_flush_queue 상태머신에 진입.
 * req_ref_*() : request(NVMe CID에 대응)의 참조 카운트를 원자적으로 관리.
 * blk_time_get_ns() / blk_time_get() : plug 단위로 batching된 IO 타임스탬프
 *   획득 — 동일 plug 내 여러 NVMe rq가 같은 타임스탬프를 공유.
 * struct blk_flush_queue : PREFLUSH/FUA를 NVMe FLUSH 명령으로 시퀀싱하는
 *   더블 버퍼 상태머신 (필드별 상세 설명은 구조체 정의부 참고).
 */

struct elv_change_ctx;
/* [한국어] elevator_switch() 등 elevator.c 내부에서 스케줄러 교체 시 사용하는
 * 컨텍스트 구조체의 전방 선언. 실제 정의는 elevator.c에 있으며, blk.h는
 * elv_update_nr_hw_queues()의 매개변수 타입으로만 이 이름을 필요로 한다. */

/*
 * Default upper limit for the software max_sectors limit used for regular I/Os.
 * This can be increased through sysfs.
 *
 * This should not be confused with the max_hw_sector limit that is entirely
 * controlled by the block device driver, usually based on hardware limits.
 */
/* [한국어] BLK_DEF_MAX_SECTORS_CAP — 소프트웨어 계층에서 강제하는 max_sectors 기본 상한.
 * 왜 필요한가: 드라이버가 하드웨어적으로 훨씬 큰 max_hw_sectors(예: NVMe MDTS가 매우 큰
 * 경우)를 보고하더라도, 소프트웨어 기본값은 지연시간·페이지 캐시 사용량 균형을 위해
 * 4MB로 제한한다. sysfs(/sys/block/<disk>/queue/max_sectors_kb)를 통해 max_hw_sectors
 * 한도 내에서 사용자가 늘릴 수 있다(추정: nvme_update_ns_info_block 등에서 조정). */
#define BLK_DEF_MAX_SECTORS_CAP	(SZ_4M >> SECTOR_SHIFT)	/* [한국어] 4MB를 섹터(512B) 단위로 환산 = 8192 sectors */

/* [한국어] BLK_DEV_MAX_SECTORS — request_queue가 표현 가능한 섹터 수의 이론적 최댓값.
 * 왜 필요한가: sector_t 연산이 signed long long 범위를 벗어나 오버플로되지 않도록
 * 컴파일 타임에 안전한 상한을 못박아 둔다. NVMe의 64비트 LBA namespace라도 실질적
 * 한도는 이 값이 아니라 MDTS/CAP 및 BLK_DEF_MAX_SECTORS_CAP로 결정된다. */
#define	BLK_DEV_MAX_SECTORS	(LLONG_MAX >> 9)
/* [한국어] BLK_MIN_SEGMENT_SIZE — queue_limits.max_segment_size의 하한.
 * 왜 필요한가: 이보다 작은 값을 드라이버가 설정하면 병합/분할 로직의 세그먼트 계산이
 * 무의미해지므로, NVMe PRP entry가 표현하는 최소 단위(4KB 페이지)와 일치시켜 안전
 * 마진을 둔다. */
#define	BLK_MIN_SEGMENT_SIZE	4096

/* Max future timer expiry for timeouts */
/* [한국어] BLK_MAX_TIMEOUT — blk_add_timer()가 등록하는 future timer의 상한(5초).
 * 왜 필요한가: timeout 값을 jiffies로 변환해 더할 때, 너무 큰 값을 그대로 쓰면
 * unsigned long 타이머 표현이 감싸돌아(wrap-around) 실제보다 훨씬 이른 시각으로
 * 오해될 수 있다. NVMe admin/abort 명령처럼 매우 긴 timeout이 요청되어도 실제 타이머는
 * 이 값 단위로 쪼개 재등록함으로써 그런 오버플로를 방지한다(추정: blk_rq_timeout() 참고). */
#define BLK_MAX_TIMEOUT		(5 * HZ)

/* [한국어] blk_queue_ktype — request_queue의 kobject가 sysfs에 노출될 때 사용하는
 * kobj_type(release/sysfs_ops/default_groups). 실제 정의는 block/blk-sysfs.c에
 * 있으며, request_queue 임베디드 kobject 초기화 시(blk_register_queue 등) 참조된다.
 * NVMe namespace가 /sys/block/nvmeXnY/queue/ 아래 속성 파일들을 노출하는 근거가
 * 이 kobj_type이다. */
extern const struct kobj_type blk_queue_ktype;
/* [한국어] blk_debugfs_root — 블록 계층 전역 debugfs 최상위 디렉터리
 * (/sys/kernel/debug/block)의 dentry. blk_dev_init()에서 부팅 시 한 번 생성되며,
 * 이후 각 request_queue/hctx가 이 아래에 자신의 디버그 디렉터리를 만든다. NVMe
 * 드라이버의 hctx별 tags, sched_tags, requeue_list 등을 확인하는 debugfs 인터페이스의
 * 진입점이다. */
extern struct dentry *blk_debugfs_root;

/*
 * [한국어]
 * struct blk_flush_queue - PREFLUSH/FUA 시퀀싱을 위한 per-hctx 상태머신
 *
 * NVMe volatile write cache(VWC)가 켜진 컨트롤러에서는 데이터를 비휘발성
 * 매체에 확정(commit)하려면 별도의 FLUSH 명령(Opcode 0x00)을 SQ에 넣어야
 * 한다. 이 구조체는 상위에서 내려온 REQ_PREFLUSH/REQ_FUA 요청들을 PREFLUSH
 * -> DATA -> POSTFLUSH 순서로 시퀀싱하고, 여러 요청을 하나의 NVMe FLUSH
 * 명령으로 묶어 발행하는 더블 버퍼링(flush_pending_idx/flush_running_idx)을
 * 구현한다. block/blk-flush.c가 이 구조체의 유일한 소유자이며, blk.h는
 * blk-mq.c 등 다른 번역 단위가 포인터만 주고받을 수 있도록 타입을 노출한다.
 * 실제 정의·조작 로직에 대한 상세 설명은 block/blk-flush.c 파일 상단
 * 주석과 각 함수 주석을 참고한다(용어와 세부 규칙이 이 구조체와 동일하게
 * 적용됨).
 */
struct blk_flush_queue {
	spinlock_t		mq_flush_lock;
	/* [한국어] 이 blk_flush_queue 전체를 보호하는 IRQ-safe 스핀락.
	 * 역할: flush_pending_idx/running_idx, rq_status, flush_data_in_flight,
	 *   flush_queue[]에 대한 모든 읽기/수정을 직렬화하는 유일한 락.
	 * 설정자: blk_alloc_flush_queue()에서 spin_lock_init()으로 초기화.
	 * 사용자: blk_insert_flush(), blk_kick_flush(), flush_end_io(),
	 *   mq_flush_data_end_io()가 모두 spin_lock_irqsave/irqrestore로 획득 —
	 *   NVMe CQ 완료 인터럽트 경로와 제출(submit) 경로 간의 경쟁을 막는다.
	 * 값 범위: 잠금/해제 상태만 가짐(스핀락 자체에 값 의미 없음).
	 * 동기화: 반드시 IRQ 비활성 상태로 획득해야 한다 — NVMe CQ 완료가 하드
	 *   인터럽트 컨텍스트에서 동일 락을 재귀 없이 기다릴 수 있기 때문이다. */

	unsigned int		flush_pending_idx:1;
	/* [한국어] 현재 "대기 중(pending)" 상태인 flush_queue[] 버퍼의 인덱스(0 또는 1).
	 * 역할: 새로 도착하는 PREFLUSH/POSTFLUSH 단계 요청이 어느 버퍼에 쌓이는지
	 *   가리킨다. blk_kick_flush()가 이 버퍼를 하나의 NVMe FLUSH 명령으로
	 *   묶어 발행할 때 flush_pending_idx를 반대편으로 토글하여, 이후 도착하는
	 *   요청이 다음 FLUSH 사이클의 버퍼로 쌓이게 한다(더블 버퍼링/ping-pong).
	 * 설정자: blk_kick_flush()가 fq->mq_flush_lock 보유 중 ^= 1로 토글.
	 * 읽는 자: blk_insert_flush()(현재 pending 버퍼에 삽입),
	 *   blk_kick_flush()(발행 조건 검사).
	 * 값 범위: 0 또는 1 (비트필드 1비트).
	 * 동기화: mq_flush_lock 보유 하에서만 읽기/쓰기 — 그렇지 않으면 CQ 완료
	 *   경로와 경쟁해 잘못된 버퍼에 요청이 섞일 수 있다. */

	unsigned int		flush_running_idx:1;
	/* [한국어] 현재 NVMe 컨트롤러로 "in-flight" 상태인 FLUSH 명령이 대응하는
	 *   flush_queue[] 버퍼 인덱스.
	 * 역할: flush_end_io()가 NVMe CQ에서 FLUSH 완료를 받았을 때, 이 인덱스가
	 *   가리키는 버퍼에 쌓여 있던 PRE/POSTFLUSH 요청들을 모두 다음 시퀀스
	 *   단계로 전이시킨다. pending_idx == running_idx이면 "현재 in-flight인
	 *   FLUSH가 없다"는 불변식(C1)을 의미하므로, 이 조건이 깨지면 BUG_ON.
	 * 설정자: flush_end_io()가 완료 처리 후 ^= 1로 토글(다음 슬롯을 비움).
	 * 읽는 자: flush_end_io()(자신이 처리할 리스트 선택),
	 *   blk_kick_flush()(pending_idx와 비교해 이미 in-flight인지 판단).
	 * 값 범위: 0 또는 1.
	 * 동기화: mq_flush_lock 보유 하에서만 접근. */

	blk_status_t 		rq_status;
	/* [한국어] 아직 상위로 보고되지 않은 flush 관련 request의 보류(pending)
	 *   에러 상태.
	 * 역할: DATA(FUA Write) 단계 요청이 에러로 완료되면, 그 에러를 즉시
	 *   버리지 않고 이 필드에 잠시 저장해 두었다가, 같은 시퀀스에 속한
	 *   POSTFLUSH/FLUSH 완료 시점에 최종 error로 채택해 전체 체인에
	 *   전파한다. NVMe CQE의 Status Field가 blk_status_t(BLK_STS_*)로
	 *   변환된 값이 여기 저장된다.
	 * 설정자: mq_flush_data_end_io()/blk_flush_complete_seq() 등이
	 *   에러 발생 시 대입.
	 * 읽는 자: flush_end_io()가 최종 error 채택 시 읽고 나서 BLK_STS_OK로
	 *   리셋 — 다음 FLUSH 사이클에 이전 에러가 새어 들어가지 않도록 한다.
	 * 값 범위: BLK_STS_OK(에러 없음) 또는 임의의 blk_status_t 에러 코드.
	 * 동기화: mq_flush_lock 보유 하에서만 읽기/쓰기. */

	unsigned long		flush_pending_since;
	/* [한국어] 현재 pending 버퍼가 "비어 있다가 처음 채워진" jiffies 시각.
	 * 역할: FLUSH_PENDING_TIMEOUT(기아 방지 타임아웃, C3 조건)을 계산하는
	 *   기준점. flush_data_in_flight가 0이 아니더라도, 이 시각으로부터
	 *   FLUSH_PENDING_TIMEOUT이 지나면 blk_kick_flush()가 강제로 FLUSH를
	 *   발행해 무한 대기를 막는다.
	 * 설정자: blk_insert_flush()가 pending 리스트가 비어 있던 시점에만
	 *   jiffies로 갱신 — 이미 대기 중인 요청이 있으면 갱신하지 않는다.
	 * 읽는 자: blk_kick_flush()가 time_before(jiffies, ...) 비교에 사용.
	 * 값 범위: jiffies 단위의 임의 시각(래핑 가능 — time_before() 매크로로
	 *   비교해야 함).
	 * 동기화: mq_flush_lock 보유 하에서만 접근. */

	struct list_head	flush_queue[2];
	/* [한국어] PREFLUSH/POSTFLUSH 단계에 있는 request들을 담는 더블 버퍼
	 *   대기열 두 칸.
	 * 역할: flush_pending_idx가 가리키는 칸은 "다음 NVMe FLUSH 명령에 함께
	 *   묶일 후보"들의 리스트이고, flush_running_idx가 가리키는 칸은 "현재
	 *   in-flight FLUSH가 완료되면 다음 단계로 전이될" 리스트다. 하나의
	 *   NVMe FLUSH 명령으로 여러 request의 PREFLUSH 요구를 한 번에
	 *   충족시켜, 컨트롤러에 발행되는 FLUSH 명령 수를 줄이는 것이 이
	 *   더블 버퍼링의 목적이다.
	 * 설정자: blk_insert_flush()가 list_add_tail()로 pending 칸에 삽입.
	 * 읽는 자: blk_kick_flush()(pending 칸에서 running 칸으로 넘길 때),
	 *   flush_end_io()(running 칸을 순회하며 다음 단계로 전이).
	 * 값 범위: request->queuelist로 연결된 연결 리스트(비어 있을 수 있음).
	 * 동기화: mq_flush_lock 보유 하에서만 list 연산 수행. */

	unsigned long		flush_data_in_flight;
	/* [한국어] 아직 NVMe 컨트롤러로부터 완료 응답을 받지 못한 DATA(FUA
	 *   Write) 단계 request의 개수.
	 * 역할: POSTFLUSH를 필요로 하는 정책(policy)에서, 이 값이 0이 아니면
	 *   아직 진행 중인 데이터 쓰기가 있다는 뜻이므로 POSTFLUSH FLUSH
	 *   명령 발행을 지연시켜야 한다(C2 조건). 데이터가 매체에 반영되기
	 *   전에 FLUSH를 먼저 보내면 durability 보장이 깨지기 때문이다.
	 * 설정자: blk_insert_flush()가 DATA 단계 진입 직전 증가시키고,
	 *   mq_flush_data_end_io()가 NVMe CQE로 완료를 확인한 뒤 감소시킨다.
	 * 읽는 자: blk_kick_flush()가 POSTFLUSH 발행 가능 여부(C2) 판단 시 확인.
	 * 값 범위: 0 이상의 정수. 0이어야 POSTFLUSH FLUSH를 안전하게 보낼 수 있음.
	 * 동기화: mq_flush_lock 보유 하에서만 증감. */

	struct request		*flush_rq;
	/* [한국어] 이 hctx 전용으로 미리 할당되어 NVMe FLUSH 명령 발행에
	 *   반복 재사용되는 request 포인터.
	 * 역할: 매 FLUSH 사이클마다 request를 새로 할당하지 않고 이 rq를
	 *   재활용함으로써, hot path에서의 할당 오버헤드를 없앤다. NVMe
	 *   드라이버 관점에서는 이 rq가 매번 새로운 CID(Command ID)를
	 *   빌려(non-scheduler 모드) 또는 driver/internal tag를 새로 받아
	 *   (scheduler 모드) SQ에 삽입된다.
	 * 설정자: blk_alloc_flush_queue()가 cmd_size만큼의 드라이버 사설
	 *   공간을 포함해 kzalloc_node()로 할당.
	 * 읽는 자/갱신자: blk_kick_flush()(다음 FLUSH 발행 시 필드 재설정),
	 *   is_flush_rq()(rq->end_io == flush_end_io 비교로 식별),
	 *   flush_end_io()(완료 처리 후에도 free하지 않고 그대로 재사용).
	 * 값 범위: 유효한 request 포인터(NULL 아님) — blk_alloc_flush_queue
	 *   실패 시에만 fq 자체가 생성되지 않으므로 정상 경로에서는 항상 유효.
	 * 동기화: mq_flush_lock 보유 하에서 상태 필드(seq, end_io 등)를 갱신. */

	struct rcu_head		rcu_head;
	/* [한국어] blk_flush_queue 자체를 RCU로 지연 해제하기 위한 콜백 헤더.
	 * 역할: hctx가 제거되어 blk_free_flush_queue()가 호출된 뒤에도, 이미
	 *   RCU read-side critical section 안에서 이 fq를 참조 중인 다른
	 *   CPU가 있을 수 있으므로, 실제 kfree()는 RCU grace period가 지난
	 *   뒤로 미룬다. NVMe hctx 제거/재매핑(CPU hotplug, nr_hw_queues
	 *   변경) 시 use-after-free를 방지한다.
	 * 설정자: blk_free_flush_queue()가 kfree_rcu(fq, rcu_head) 형태로 사용.
	 * 읽는 자: RCU 서브시스템 내부(call_rcu 콜백 큐)만 접근 — blk 계층
	 *   코드가 직접 이 필드를 읽지 않는다.
	 * 값 범위: RCU 콜백 리스트 연결 상태 — 커널 RCU 구현 내부 값.
	 * 동기화: RCU 자체의 lock-free 메커니즘으로 보호되며 별도 락 불필요. */
};

/*
 * [한국어]
 * is_flush_rq - 주어진 request가 blk_flush_queue의 flush_rq인지 판별
 *
 * @req: 판별 대상 request (NVMe 완료/timeout 경로에서 전달되는 임의의 rq)
 * @return: true이면 이 hctx의 flush_rq(NVMe FLUSH 명령 재사용 rq), false이면 일반 데이터 request
 *
 * rq->end_io가 blk-flush.c의 정적 함수 flush_end_io를 가리키는지로 판별한다.
 * NVMe 드라이버(nvme_queue_rq)는 flush_rq를 일반 READ/WRITE와 동일한 경로로
 * SQ에 삽입하지만, 완료 시에는 flush_end_io가 호출되어 PREFLUSH/POSTFLUSH
 * 상태머신으로 재진입한다. 실제 정의는 block/blk-flush.c에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 또는 timeout 워크 컨텍스트.
 * 호출자: blk_mq_rq_ctx_init(), timeout 처리 경로 등.
 * 피호출자: 없음(단순 포인터 비교).
 *
 * 호출 체인:
 *   blk_mq_rq_ctx_init / timeout 경로 -> [is_flush_rq]
 */
bool is_flush_rq(struct request *req);

/*
 * [한국어]
 * blk_alloc_flush_queue - per-hctx PREFLUSH/FUA 상태머신 자료구조 할당·초기화
 *
 * @node: NUMA 노드 번호 (연결될 hctx와 동일 노드에 할당해 접근 지역성 확보)
 * @cmd_size: NVMe(또는 다른 블록) 드라이버의 driver-private 명령 공간 크기 —
 *            flush_rq 뒤에 이어붙는 바이트 수(struct request 뒤에 패딩)
 * @flags: kzalloc_node()에 전달할 GFP 플래그
 * @return: 초기화된 blk_flush_queue 포인터, 메모리 부족 시 NULL
 *
 * NVMe SQ/CQ 쌍(hctx)마다 독립적인 PREFLUSH/FUA 상태머신이 필요하므로
 * hctx 생성 시 함께 할당된다. 내부에서 flush_rq(FLUSH 명령 재사용 request)와
 * flush_queue[0]/[1](더블 버퍼 대기열)을 초기화한다. 실제 정의는
 * block/blk-flush.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (hctx 할당 경로, sleep 가능한 GFP 사용 시).
 * 호출자: blk_mq_alloc_hctx().
 * 피호출자: kzalloc_node(), spin_lock_init(), INIT_LIST_HEAD().
 * 에러 경로: fq 또는 flush_rq 할당 실패 시 이미 확보한 자원을 해제하고 NULL 반환.
 *
 * 호출 체인:
 *   blk_mq_alloc_hctx -> [blk_alloc_flush_queue]
 */
struct blk_flush_queue *blk_alloc_flush_queue(int node, int cmd_size,
					      gfp_t flags);
/*
 * [한국어]
 * blk_free_flush_queue - blk_alloc_flush_queue()로 할당한 자료구조 해제
 *
 * @q: 해제할 blk_flush_queue (NULL이면 아무 동작도 하지 않음 — bio-based
 *     queue에는 flush queue 자체가 없을 수 있으므로 방어적으로 검사)
 * @return: 없음
 *
 * flush_rq를 먼저 kfree()한 뒤 fq 자신을 해제한다. hctx가 소멸(CPU
 * hotplug, nr_hw_queues 변경, NVMe 컨트롤러 제거 등)할 때 짝을 맞춰
 * 호출되어야 한다. 실제 정의는 block/blk-flush.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (hctx 해제 경로).
 * 호출자: blk_mq_free_hctx().
 * 피호출자: kfree().
 *
 * 호출 체인:
 *   blk_mq_free_hctx -> [blk_free_flush_queue]
 */
void blk_free_flush_queue(struct blk_flush_queue *q);

/*
 * [한국어]
 * __blk_mq_unfreeze_queue - request_queue 동결(freeze) 해제 내부 구현
 *
 * @q: 동결 해제할 request_queue
 * @force_atomic: true이면 percpu_ref를 atomic 모드로 강제 전환한 뒤 부활
 * @return: true이면 이 호출이 마지막 소유자의 unfreeze(락 해제 등 후속 처리 필요)
 *
 * NVMe 컨트롤러 reset/shutdown 과정에서 큐를 동결(freeze)해 두었다가,
 * 복구가 끝나면 q->mq_freeze_depth를 감소시키고 0에 도달하면
 * percpu_ref_resurrect()로 q_usage_counter를 되살려 outstanding 및 신규
 * SQ 진입이 다시 doorbell을 받을 수 있게 한다. force_atomic은 메모리 절약
 * 경로(memcg percpu)에서 atomic fallback이 필요할 때 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, q->mq_freeze_lock 보유 상태.
 * 호출자: blk_mq_unfreeze_queue_nomemrestore() (blk-mq.c).
 * 피호출자: percpu_ref_resurrect(), wake_up_all(&q->mq_freeze_wq).
 *
 * 호출 체인:
 *   nvme_reset_work -> blk_mq_unfreeze_queue -> [__blk_mq_unfreeze_queue]
 */
bool __blk_mq_unfreeze_queue(struct request_queue *q, bool force_atomic);

/*
 * [한국어]
 * blk_queue_start_drain - queue를 DYING 상태로 만들고 신규 IO 진입을 차단
 *
 * @q: drain을 시작할 request_queue
 * @return: 이 호출이 실제로 freeze를 시작했으면 true, 이미 진행 중이었으면 false
 *
 * NVMe 컨트롤러 reset·remove·surprise removal 시, 기존 in-flight I/O는
 * 계속 완료되도록 두면서 새로운 submit_bio 호출은 -ENODEV를 받도록 큐를
 * DYING 상태로 전환한다. 내부적으로 __blk_freeze_queue_start()로
 * q_usage_counter를 kill해 이후 blk_try_enter_queue()가 실패하게 만들고,
 * 대기 중인 태스크를 깨워 DYING 상태를 재검사하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NVMe reset kthread, nvme_remove 경로).
 * 호출자: blk_mq_destroy_queue(), del_gendisk() 경로 등.
 * 피호출자: __blk_freeze_queue_start(), blk_mq_wake_waiters(), wake_up_all().
 *
 * 호출 체인:
 *   nvme_remove / nvme_reset_ctrl -> ... -> [blk_queue_start_drain]
 *   -> __blk_freeze_queue_start
 */
bool blk_queue_start_drain(struct request_queue *q);
/*
 * [한국어]
 * __blk_freeze_queue_start - 큐 동결(freeze) 시작: 새 IO 진입 차단
 *
 * @q: 동결할 request_queue
 * @owner: freeze 소유자 태스크(중첩 freeze 추적용); NULL이면 non-owner freeze
 * @return: true이면 이 호출이 "소유자" freeze를 새로 시작함(짝이 되는 unfreeze에서 락 해제 필요)
 *
 * percpu_ref_kill(&q->q_usage_counter)로 사용 카운터를 종료시켜, 이후
 * percpu_ref_tryget 계열 호출(blk_try_enter_queue 등)이 모두 실패하도록
 * 만든다. 이미 진행 중인 NVMe request는 계속 완료될 수 있으며, 마지막
 * 참조가 반납되어 percpu_ref_is_zero가 true가 되면 freeze_wq의 대기자를
 * 깨운다. 같은 태스크의 중첩 freeze는 mq_freeze_depth로 추적되어 idempotent
 * 하게 처리된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (반드시 sleep 가능해야 함).
 * 호출자: blk_queue_start_drain(), blk_mq_freeze_queue() 계열.
 * 피호출자: percpu_ref_kill(), blk_mq_run_hw_queues() (pending flush 처리).
 *
 * 호출 체인:
 *   blk_freeze_queue_start -> [__blk_freeze_queue_start]
 *     -> percpu_ref_kill -> blk_mq_run_hw_queues
 */
bool __blk_freeze_queue_start(struct request_queue *q,
			      struct task_struct *owner);

/*
 * [한국어]
 * __bio_queue_enter - bio 제출 경로 전용 request_queue 사용 카운터 획득
 *
 * @q: 진입할 request_queue
 * @bio: 제출 중인 bio (GD_DEAD 확인 및 실패 시 bio_io_error() 호출에 사용)
 * @return: 0=성공, -EAGAIN=REQ_NOWAIT인데 즉시 획득 실패, -ENODEV=디스크가
 *          이미 GD_DEAD 상태(이 경우 함수 내부에서 bio_io_error()도 호출됨)
 *
 * blk_try_enter_queue()의 빠른 경로가 실패했을 때 호출되는 느린 경로다.
 * NVMe 컨트롤러 제거/리셋 중에는 percpu_ref가 dead 상태이므로, 이 함수는
 * mq_freeze_wq에서 unfreeze 또는 dying 확정까지 대기한다. GD_DEAD(gendisk
 * 소멸) 비트가 확인되면 즉시 에러를 반환해 무한 대기를 피한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NOWAIT가 아니면 sleep 가능).
 * 호출자: bio_queue_enter() (본 파일, 인라인 빠른 경로 실패 시).
 * 피호출자: blk_try_enter_queue(), wait_event(), bio_io_error().
 *
 * 호출 체인:
 *   bio_queue_enter(bio) -> [__bio_queue_enter] -> blk_try_enter_queue / wait_event
 */
int __bio_queue_enter(struct request_queue *q, struct bio *bio);

/*
 * [한국어]
 * submit_bio_noacct_nocheck - 검증을 이미 마친 bio를 실제 제출 경로로 전달
 *
 * @bio: 제출할 bio (submit_bio_noacct()의 유효성 검사를 이미 통과한 상태여야 함)
 * @split: true이면 분할되어 나온 앞부분 bio(리스트 맨 앞에 추가해 우선 처리),
 *         false이면 새로 제출되는 일반 bio(리스트 뒤에 추가)
 * @return: 없음
 *
 * "nocheck"라는 이름처럼 GD_DEAD, 파티션 경계, read-only 등의 검사를
 * 생략하고 곧바로 cgroup I/O 계정(blk_cgroup_bio_start), tracepoint 발행,
 * 그리고 current->bio_list 유무에 따른 재귀 방지 처리 후 실제
 * __submit_bio_noacct_mq()(NVMe 등 blk-mq 표준 장치) 또는
 * __submit_bio_noacct()(레벨 정렬이 필요한 stacking 장치)로 분기한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio_noacct 및 blk_throtl_bio 콜백 경로).
 * 호출자: submit_bio_noacct(), bio_split() 이후 분할된 잔여 bio 재제출 경로.
 * 피호출자: blk_cgroup_bio_start(), __submit_bio_noacct_mq()/__submit_bio_noacct().
 *
 * 호출 체인:
 *   submit_bio_noacct -> [submit_bio_noacct_nocheck] -> __submit_bio_noacct_mq
 *   -> blk_mq_submit_bio -> mq_ops->queue_rq (간접 호출; NVMe PCIe 면 nvme_queue_rq -> nvme_sq_copy_cmd -> nvme_write_sq_db)
 */
void submit_bio_noacct_nocheck(struct bio *bio, bool split);
/*
 * [한국어]
 * bio_submit_or_kill - bio를 제출하되, 큐 진입 실패 시 bio를 즉시 종료(kill)
 *
 * @bio: 제출할 bio
 * @flags: bio_queue_enter 계열에 전달할 진입 플래그(NOWAIT 등)
 * @return: 0=정상 제출됨, 음수=진입 실패로 bio가 이미 에러 완료 처리됨
 *
 * bio_queue_enter()로 큐 진입을 시도하고, 실패(-ENODEV 등, 컨트롤러가
 * 제거/리셋 중)하면 bio_io_error()로 bio를 즉시 -ENODEV 완료시켜 호출자가
 * 별도 에러 처리를 하지 않아도 되게 한다. 성공하면 submit_bio_noacct_nocheck()
 * 계열로 실제 제출을 이어간다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bio 제출 계열 헬퍼(예: __bio_split_to_limits 이후 재제출 경로).
 * 피호출자: bio_queue_enter(), bio_io_error(), submit_bio_noacct_nocheck().
 *
 * 호출 체인:
 *   (bio 재제출 경로) -> [bio_submit_or_kill] -> bio_queue_enter -> submit_bio_noacct_nocheck
 */
int bio_submit_or_kill(struct bio *bio, unsigned int flags);

/*
 * [한국어]
 * blk_try_enter_queue - RCU 보호 하에 큐의 live 참조 획득을 "시도"(sleep 없음)
 *
 * @q: 진입할 request_queue (NVMe namespace마다 독립적으로 존재)
 * @pm: power-management 경로로 진입하는지 여부 (true면 PM resume 경로 허용)
 * @return: true=참조 획득 성공(호출자는 이후 blk_queue_exit()로 반납해야 함),
 *          false=큐가 dead/suspended여서 즉시 실패
 *
 * bio_queue_enter()의 빠른 경로로 사용되며, percpu_ref_tryget_live_rcu()가
 * 실패(큐가 freeze/dying 상태)하거나 pm_only 상태에서 PM 경로가 아니면
 * 즉시 false를 반환한다. sleep하지 않으므로 인터럽트/atomic 컨텍스트에서도
 * 호출 가능하다. 느린 경로(대기 필요)는 __bio_queue_enter()가 담당한다.
 * 실행 컨텍스트: 임의 컨텍스트 (RCU read-side critical section 내부에서만
 * percpu_ref를 조작하므로 atomic 컨텍스트에서도 안전).
 * 호출자: bio_queue_enter() (본 파일).
 * 피호출자: percpu_ref_tryget_live_rcu(), blk_queue_pm_only(), blk_queue_exit().
 * 에러 경로: 실패 시 fail/fail_put 레이블로 점프해 획득한 자원(있다면)을 반납.
 *
 * 호출 체인:
 *   bio_queue_enter -> [blk_try_enter_queue]
 */
static inline bool blk_try_enter_queue(struct request_queue *q, bool pm)
{
	rcu_read_lock();						/* NVMe queue live 상태는 RCU로 보호; 제거 중에는 percpu_ref dead 후 grace period 경과 */
	if (!percpu_ref_tryget_live_rcu(&q->q_usage_counter))		/* NVMe 컨트롤러 제거/reset 시 q_usage_counter dead -> -ENODEV로 진입 차단 */
		goto fail;

	/*
	 * The code that increments the pm_only counter must ensure that the
	 * counter is globally visible before the queue is unfrozen.
	 */
	/* pm_only가 설정되면 NVMe 컨트롤러가 suspend 상태이므로 I/O 거부; resume 완료 전 doorbell 무의미 */
	if (blk_queue_pm_only(q) &&
	    (!pm || queue_rpm_status(q) == RPM_SUSPENDED))		/* pm 경로가 아니거나 SUSPENDED 이면 진입 실패 -> NVMe queue 진입 차단 */
		goto fail_put;

	rcu_read_unlock();
	return true;							/* NVMe namespace queue 진입 성공; 이후 blk_mq_get_request -> CID/tag 할당 가능 */

fail_put:
	blk_queue_exit(q);						/* 획득한 q_usage_counter 반납; NVMe queue ref count 감소 */
fail:
	rcu_read_unlock();
	return false;							/* NVMe controller dead/suspended; bio_submit_or_kill에서 -ENODEV 처리 */
}

/*
 * [한국어]
 * bio_queue_enter - bio가 소속된 request_queue로의 진입 참조를 획득
 *
 * @bio: 진입할 대상 큐를 결정하는 bio (bio->bi_bdev로 큐를 역참조)
 * @return: 0=성공(이후 blk_queue_exit()로 반납 필요), 음수=진입 실패
 *          (-EAGAIN/-ENODEV 등, __bio_queue_enter()가 이미 에러 처리를 마쳤을 수 있음)
 *
 * 먼저 blk_try_enter_queue()로 sleep 없는 빠른 경로를 시도하고, 큐가
 * freeze/suspended 상태라 실패하면 __bio_queue_enter()의 느린 경로로
 * 넘어가 대기하거나 즉시 에러로 종료한다. lockdep 표시(rwsem_acquire/
 * release)는 실제 락 없이 "이 지점에서 큐 진입이 일어났다"는 사실만
 * lockdep에 알리기 위한 것이다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio 경로).
 * 호출자: submit_bio_noacct() 계열, __bio_queue_enter() 호출 이전 단계.
 * 피호출자: bdev_get_queue(), blk_try_enter_queue(), __bio_queue_enter().
 *
 * 호출 체인:
 *   submit_bio_noacct -> [bio_queue_enter] -> blk_try_enter_queue (성공)
 *   또는 -> __bio_queue_enter (실패 시 느린 경로)
 */
static inline int bio_queue_enter(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);	/* bio -> block_device -> request_queue; NVMe namespace 마다 독립 queue */

	if (blk_try_enter_queue(q, false)) {				/* NVMe I/O 일반 경로; PM 경로 아님 */
		rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_);
		rwsem_release(&q->io_lockdep_map, _RET_IP_);
		return 0;						/* queue 진입 성공; blk_mq_submit_bio 계속 진행 -> NVMe SQ 할당 */
	}
	return __bio_queue_enter(q, bio);				/* slow path: queue dead/suspended 상태에서 대기 또는 kill; NVMe reset 대기(추정) */
}

/*
 * [한국어]
 * blk_wait_io - completion을 hung_task 오탐 없이 동기적으로 대기
 *
 * @done: I/O 완료 시 complete()이 호출될 completion 객체
 * @return: 없음 (completion이 완료될 때까지 반환하지 않음)
 *
 * NVMe sync 명령(예: admin identify, submit_bio_wait를 통한 flush 등)처럼
 * 매우 길게 걸릴 수 있는 I/O 대기에서, 커널의 hung_task watchdog이
 * "프로세스가 멈췄다"고 오탐하지 않도록 hung_task 타임아웃의 절반 간격으로
 * wait_for_completion_io_timeout()을 반복 호출해 워치독을 주기적으로 리셋한다.
 * sysctl_hung_task_timeout_secs가 0(감시 비활성)이면 무기한
 * wait_for_completion_io()로 단순 대기한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sleep 가능한 태스크).
 * 호출자: submit_bio_wait() 등 동기 I/O 완료 대기가 필요한 경로.
 * 피호출자: wait_for_completion_io_timeout(), wait_for_completion_io().
 *
 * 호출 체인:
 *   submit_bio_wait -> [blk_wait_io] -> wait_for_completion_io_timeout(반복)
 */
static inline void blk_wait_io(struct completion *done)
{
	/* Prevent hang_check timer from firing at us during very long I/O */
	unsigned long timeout = sysctl_hung_task_timeout_secs * HZ / 2;		/* NVMe admin command timeout보다 긴 sync wait를 분할; hung task false positive 방지 */

	if (timeout)
		while (!wait_for_completion_io_timeout(done, timeout))		/* NVMe CQ 완료가 도착할 때까지 timeout/2 단위로 polling; ISR -> complete() */
			;
	else
		wait_for_completion_io(done);					/* timeout=0이면 무기한 대기; NVMe recovery 없이는 hang 가능 */
}

/*
 * [한국어]
 * blkdev_get_no_open - device number(dev_t)만으로 block_device를 참조 획득(open 없이)
 *
 * @dev: 대상 장치의 dev_t (major/minor 번호)
 * @autoload: true이면 모듈이 아직 로드되지 않은 경우 자동 로드(request_module) 시도
 * @return: 참조 획득된 block_device 포인터, 실패 시 NULL
 *
 * 일반적인 bdev_open()과 달리 실제 open(파일 디스크립터 연결, 배타적 락 등)
 * 절차 없이 block_device의 존재 여부만 확인하고 참조 카운트를 올린다.
 * partition scan, sysfs 조회 등 "장치가 있는지만 알면 되는" 경로에서 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bdev_open()의 내부 준비 단계, 파티션 관리 코드 등.
 * 피호출자: class_find_device(), bdget() 계열 내부 헬퍼(bdev.c 정의).
 *
 * 호출 체인:
 *   bdev_open 등 -> [blkdev_get_no_open]
 */
struct block_device *blkdev_get_no_open(dev_t dev, bool autoload);
/*
 * [한국어]
 * blkdev_put_no_open - blkdev_get_no_open()으로 얻은 참조를 반납
 *
 * @bdev: 반납할 block_device
 * @return: 없음
 *
 * blkdev_get_no_open()과 반드시 짝을 이루어 호출해야 하는 반납 함수다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_get_no_open() 사용처의 정리(cleanup) 경로.
 * 피호출자: put_device() 계열 내부 헬퍼.
 *
 * 호출 체인:
 *   (blkdev_get_no_open 사용처) -> [blkdev_put_no_open]
 */
void blkdev_put_no_open(struct block_device *bdev);

/*
 * [한국어]
 * bvec_try_merge_hw_page - 하드웨어(queue_limits) 관점에서 페이지 병합 시도
 *
 * @q: 대상 request_queue (seg_boundary_mask, max_segment_size 등 limits 보유)
 * @bv: 병합 대상이 될 기존 마지막 bio_vec (성공 시 길이가 늘어남)
 * @page: 새로 추가하려는 물리 페이지
 * @len: 추가하려는 길이(바이트)
 * @offset: 페이지 내 시작 오프셋
 * @return: true=@bv에 병합되어 별도 entry 불필요, false=새 bio_vec 필요
 *
 * bio_add_page() 계열이 bio_vec 배열에 페이지를 추가할 때, 바로 이전 항목과
 * 물리적으로 인접하고 seg_boundary_mask/max_segment_size를 넘지 않으면
 * 병합해 bio_vec 배열의 항목 수(=NVMe PRP/SGL entry 후보 수)를 줄인다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (bio 구성 경로).
 * 호출자: bio_add_page(), bio_add_folio() 등 bio.c의 페이지 추가 헬퍼.
 * 피호출자: get_max_segment_size(), bvec_phys() 등(정의: bio.c).
 *
 * 호출 체인:
 *   bio_add_page -> [bvec_try_merge_hw_page]
 */
bool bvec_try_merge_hw_page(struct request_queue *q, struct bio_vec *bv,
		struct page *page, unsigned len, unsigned offset);

/*
 * [한국어]
 * biovec_phys_mergeable - 두 bio_vec이 물리적으로 인접해 하나의 NVMe
 *                         PRP/SGL segment로 묶일 수 있는지 검사
 *
 * @q: 두 bio_vec이 속하게 될 request_queue (seg_boundary_mask 제공)
 * @vec1: 앞쪽 bio_vec (낮은 주소)
 * @vec2: 뒤쪽 bio_vec (vec1 바로 다음에 이어붙일 후보)
 * @return: true=물리적으로 연속해 하나의 segment로 병합 가능, false=별도 segment 필요
 *
 * NVMe PRP entry는 인접한 물리 페이지를 하나의 entry로 묶어 표현할 수
 * 있으나, seg_boundary_mask 경계를 넘어서는 인접 페이지는 컨트롤러가
 * 요구하는 정렬 규칙 때문에 별도 PRP/SGL entry로 나눠야 한다. KMSAN
 * 빌드에서는 metadata 페이지 인접성이 보장되지 않아 항상 병합을 거부한다.
 * Xen 게스트에서는 grant table 매핑 경계도 추가로 검사한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (세그먼트 계산 경로), 인터럽트 불필요.
 * 호출자: blk_recalc_rq_segments() 등 세그먼트 재계산 함수(blk-merge.c).
 * 피호출자: queue_segment_boundary(), bvec_phys(), xen_biovec_phys_mergeable().
 *
 * 호출 체인:
 *   blk_recalc_rq_segments -> ... -> [biovec_phys_mergeable]
 *   -> blk_rq_map_sg -> nvme_pci_setup_data_prp/nvme_pci_setup_data_sgl
 */
static inline bool biovec_phys_mergeable(struct request_queue *q,
		struct bio_vec *vec1, struct bio_vec *vec2)
{
	unsigned long mask = queue_segment_boundary(q);			/* NVMe controller의 seg_boundary_mask; PRP/SGL entry가 넘지 말아야 할 경계 */
	phys_addr_t addr1 = bvec_phys(vec1);				/* vec1의 물리 주소; NVMe PRP0/PRP1 계산 시 동일한 기준 */
	phys_addr_t addr2 = bvec_phys(vec2);				/* vec2의 물리 주소; 인접하면 PRP list entry 하나로 병합 가능 */

	/*
	 * Merging adjacent physical pages may not work correctly under KMSAN
	 * if their metadata pages aren't adjacent. Just disable merging.
	 */
	if (IS_ENABLED(CONFIG_KMSAN))
		return false;							/* KMSAN에서는 metadata 인접 보장 안 됨; 병합 off -> NVMe PRP/SGL entry 수 증가 */

	/* vec1 뒤에 vec2가 물리적으로 붙어 있어야 PRP/SGL 병합 가능 */
	if (addr1 + vec1->bv_len != addr2)
		return false;							/* 물리적으로 떨어진 페이지 -> 별도 NVMe PRP/SGL entry 필요 */
	if (xen_domain() && !xen_biovec_phys_mergeable(vec1, vec2->bv_page))
		return false;							/* Xen grant mapping 경계를 넘으면 NVMe DMA 주소가 불연속; 별도 entry */
	/* seg_boundary_mask 범위를 벗어나면 별도 PRP/SGL entry 필요 */
	if ((addr1 | mask) != ((addr2 + vec2->bv_len - 1) | mask))
		return false;							/* seg_boundary crossing; NVMe controller가 하나의 PRP/SGL entry로 표현 불가 */
	return true;								/* 인접 + 경계 내 -> 하나의 NVMe PRP/SGL segment로 묶음; queue depth 절약 */
}

/*
 * [한국어]
 * __bvec_gap_to_prev - virt_boundary_mask 기준으로 실제 gap 존재 여부를 계산
 *
 * @lim: 대상 queue_limits (virt_boundary_mask 보유)
 * @bprv: 이전 bio_vec (경계 검사의 기준이 되는 앞쪽 항목)
 * @offset: 새로 추가하려는 bio_vec의 시작 오프셋
 * @return: true=virt_boundary를 넘는 gap이 존재해 별도 segment 필요, false=문제 없음
 *
 * bvec_gap_to_prev()가 virt_boundary_mask가 0이 아님을 미리 확인한 뒤에만
 * 호출하는 실제 계산 함수다. 새 bio_vec의 시작 offset이나 이전 bio_vec의
 * 끝 위치가 virt_boundary_mask 정렬 경계를 걸치면 IOMMU/SMMU가 두 영역을
 * 하나의 DMA 주소 범위로 취급하지 못할 수 있으므로 gap으로 간주한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (세그먼트/병합 계산 경로).
 * 호출자: bvec_gap_to_prev() (본 파일).
 * 피호출자: 없음 (순수 비트 연산).
 *
 * 호출 체인:
 *   bvec_gap_to_prev -> [__bvec_gap_to_prev]
 */
static inline bool __bvec_gap_to_prev(const struct queue_limits *lim,
		struct bio_vec *bprv, unsigned int offset)
{
	return (offset & lim->virt_boundary_mask) ||			/* 새 bvec 시작 offset이 virt_boundary를 건치면 gap 발생 -> NVMe SGL 분리 */
		((bprv->bv_offset + bprv->bv_len) & lim->virt_boundary_mask);	/* 이전 bvec 끝이 virt_boundary 위치면 gap -> 별도 segment */
}

/*
 * Check if adding a bio_vec after bprv with offset would create a gap in
 * the SG list. Most drivers don't care about this, but some do.
 */
/*
 * [한국어]
 * bvec_gap_to_prev - virt_boundary_mask가 설정된 경우에만 gap 검사를 수행
 *
 * @lim: 대상 queue_limits
 * @bprv: 이전 bio_vec
 * @offset: 새로 추가할 bio_vec의 시작 오프셋
 * @return: true=gap 존재(병합/병합 불가), false=gap 없음(대부분의 드라이버는 이 경로)
 *
 * 대부분의 NVMe 드라이버는 virt_boundary_mask를 설정하지 않으므로(SGL이
 * 임의의 물리적 gap을 표현 가능) 이 함수는 빠르게 false를 반환한다.
 * virt_boundary_mask가 설정된 일부 IOMMU/드라이버 조합에서만
 * __bvec_gap_to_prev()로 실제 계산을 위임한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: integrity_req_gap_back_merge()/integrity_req_gap_front_merge() 등
 *   병합 가능 여부를 판단하는 여러 함수(본 파일, ll_back_merge_fn 등).
 * 피호출자: __bvec_gap_to_prev() (필요할 때만).
 *
 * 호출 체인:
 *   ll_back_merge_fn / integrity_req_gap_* -> [bvec_gap_to_prev] -> __bvec_gap_to_prev
 */
static inline bool bvec_gap_to_prev(const struct queue_limits *lim,
		struct bio_vec *bprv, unsigned int offset)
{
	if (!lim->virt_boundary_mask)
		return false;							/* virt_boundary 없음 -> NVMe SGL은 물리 gap만 segment로 분리 */
	return __bvec_gap_to_prev(lim, bprv, offset);				/* IOMMU/SMMU 설정에서 NVMe DMA 주소 연속성을 강제로 끊어야 하는지 판단 */
}

/*
 * [한국어]
 * rq_mergeable - request가 앞으로 추가 병합의 "대상"이 될 수 있는지 검사
 *
 * @rq: 검사 대상 request
 * @return: true=추가 bio 병합 허용, false=병합 금지(단독으로 SQ에 발행되어야 함)
 *
 * passthrough(벤더 특수 opcode), FLUSH(opcode 0h), WRITE ZEROES(Dataset
 * Management 계열), ZONE_APPEND(0x7D, 쓰기 포인터 자동 할당) 명령은 NVMe에서
 * 저마다 다른 opcode/의미를 가지므로 일반 READ/WRITE와 병합될 수 없다.
 * REQ_NOMERGE_FLAGS/RQF_NOMERGE_FLAGS가 설정된 경우(상위에서 명시적으로
 * 금지했거나 이미 SQ에 삽입되어 CID가 확정된 경우)도 병합을 거부한다.
 * 병합을 거부하면 NVMe queue depth(SQ 슬롯)를 더 많이 소모하지만, 요청 하나당
 * 크기가 작아져 개별 요청의 지연(latency)은 낮아질 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (병합 판단 경로, 락 불필요).
 * 호출자: blk_rq_merge_ok() (blk-merge.c).
 * 피호출자: blk_rq_is_passthrough(), req_op() (인라인 헬퍼).
 *
 * 호출 체인:
 *   blk_rq_merge_ok -> [rq_mergeable]
 */
static inline bool rq_mergeable(struct request *rq)
{
	if (blk_rq_is_passthrough(rq))
		return false;							/* NVMe admin/io passthrough는 vendor 특수 opcode -> READ/WRITE와 CID 공유 불가 */

	if (req_op(rq) == REQ_OP_FLUSH)
		return false;							/* NVMe FLUSH opcode(0h)는 data command와 병합 불가; 별도 flush_rq 사용 */

	if (req_op(rq) == REQ_OP_WRITE_ZEROES)
		return false;							/* NVMe Write Zeroes는 Dataset Management 계열; 일반 Write와 PRP 형식 다름 */

	if (req_op(rq) == REQ_OP_ZONE_APPEND)
		return false;							/* NVMe ZONE_APPEND(0x7D)는 쓰기 포인터 자동 할당; 연속 LBA 가정 무효 */

	if (rq->cmd_flags & REQ_NOMERGE_FLAGS)
		return false;							/* REQ_NOMERGE 등 상위 명시적 금지; NVMe low-latency 경로에서 자주 설정 */
	if (rq->rq_flags & RQF_NOMERGE_FLAGS)
		return false;							/* RQF_STARTED 등 이미 NVMe SQ에 삽입된 rq는 병합 불가; CID 할당 완료 */

	return true;								/* 일반 NVMe READ/WRITE로 병합 가능; PRP/SGL entry 추가로 확장 */
}

/*
 * There are two different ways to handle DISCARD merges:
 *  1) If max_discard_segments > 1, the driver treats every bio as a range and
 *     send the bios to controller together. The ranges don't need to be
 *     contiguous.
 *  2) Otherwise, the request will be normal read/write requests.  The ranges
 *     need to be contiguous.
 */
/*
 * [한국어]
 * blk_discard_mergable - Discard request가 range-list 방식으로 병합 가능한지 검사
 *
 * @req: 검사 대상 request (REQ_OP_DISCARD일 때만 의미 있음)
 * @return: true=여러 range를 하나의 Dataset Management 명령으로 묶어 발행 가능,
 *          false=일반 READ/WRITE처럼 LBA 연속성이 필요(단일 range)
 *
 * max_discard_segments가 1보다 크면 드라이버(NVMe)가 하나의 Dataset
 * Management(DSM, opcode 0x9) 명령 안에 여러 개의 불연속 range를 담을 수
 * 있다는 뜻이므로, 이 경우 discard bio들은 LBA가 연속하지 않아도 같은
 * request로 병합될 수 있다. 그렇지 않은 장치는 일반 request처럼 연속된
 * range만 병합 가능하다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_try_merge() (병합 종류 판별 시 가장 먼저 검사).
 * 피호출자: queue_max_discard_segments() (인라인 헬퍼).
 *
 * 호출 체인:
 *   blk_try_merge -> [blk_discard_mergable]
 */
static inline bool blk_discard_mergable(struct request *req)
{
	if (req_op(req) == REQ_OP_DISCARD &&
	    queue_max_discard_segments(req->q) > 1)			/* NVMe Namespace Dataset Management(0x9)가 multi-range 지원; SGLD entry 수 = max_discard_segments */
		return true;
	return false;								/* 단일 range 또는 non-discard -> 병합 불가; 별도 NVMe command 소모 */
}

/*
 * [한국어]
 * blk_rq_get_max_segments - request 종류별 최대 segment(PRP/SGL entry) 수 조회
 *
 * @rq: 대상 request
 * @return: 이 request가 가질 수 있는 최대 segment 수
 *
 * Discard는 데이터 버퍼가 아닌 range-list를 사용하므로 별도의
 * max_discard_segments 한도를 따르고, 그 외 READ/WRITE 등은 일반
 * max_segments(=NVMe PRP list/SGL segment 최대 개수, 컨트롤러의 MDTS 및
 * SGL 지원 여부에서 유래)를 따른다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (세그먼트 한도 검사 경로).
 * 호출자: ll_new_hw_segment() 등 병합 시 세그먼트 수 초과 검사(blk-merge.c).
 * 피호출자: queue_max_discard_segments(), queue_max_segments().
 *
 * 호출 체인:
 *   ll_back_merge_fn -> ll_new_hw_segment -> [blk_rq_get_max_segments]
 */
static inline unsigned int blk_rq_get_max_segments(struct request *rq)
{
	if (req_op(rq) == REQ_OP_DISCARD)
		return queue_max_discard_segments(rq->q);		/* NVMe Dataset Management는 PRP가 아닌 range list; max_discard_segments가 한도 */
	return queue_max_segments(rq->q);				/* NVMe READ/WRITE의 PRP/SGL entry 최대 수; 컨트롤러 MAXSGEATS/MDTS에 의존 */
}

/*
 * [한국어]
 * blk_queue_get_max_sectors - request의 opcode 종류에 맞는 최대 섹터 수 조회
 *
 * @rq: 대상 request (req_op(rq)/cmd_flags로 종류 판별)
 * @return: 이 request가 가질 수 있는 최대 섹터 수(512B 단위)
 *
 * REQ_OP_DISCARD/SECURE_ERASE/WRITE_ZEROES/ATOMIC은 각각 별도의 한도
 * (max_discard_sectors, max_secure_erase_sectors, max_write_zeroes_sectors,
 * atomic_write_max_sectors)를 가지므로 opcode별로 분기해 반환하고, 그 외
 * 일반 READ/WRITE는 q->limits.max_sectors(NVMe MDTS와 max_hw_sectors 중
 * 작은 값)를 반환한다. Discard/Secure Erase는 UINT_MAX >> SECTOR_SHIFT와
 * min을 취해 32비트 오버플로를 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (분할/병합 한도 계산 경로).
 * 호출자: bio 분할·병합 관련 함수들(blk-merge.c의 get_max_io_size 등).
 * 피호출자: req_op() (인라인 헬퍼).
 *
 * 호출 체인:
 *   bio_split_rw / ll_back_merge_fn -> ... -> [blk_queue_get_max_sectors]
 */
static inline unsigned int blk_queue_get_max_sectors(struct request *rq)
{
	struct request_queue *q = rq->q;				/* NVMe namespace당 request_queue; limits에 MDTS, namespace boundary 반영 */
	enum req_op op = req_op(rq);					/* NVMe opcode 계열 판별; READ/WRITE(0x1/0x2), Dataset Management(0x9) 등 */

	if (unlikely(op == REQ_OP_DISCARD))
		return min(q->limits.max_discard_sectors,		/* NVMe Deallocate 한 번에 처리할 수 있는 최대 sector 수 */
			   UINT_MAX >> SECTOR_SHIFT);		/* sector 계산 시 오버플로 방지; NVMe LBA range entry 크기 제한 고려 */

	if (unlikely(op == REQ_OP_SECURE_ERASE))
		return min(q->limits.max_secure_erase_sectors,		/* NVMe Sanitize/Secure Erase 경로; 컨트롤러 sanitize capability에 따름 */
			   UINT_MAX >> SECTOR_SHIFT);

	if (unlikely(op == REQ_OP_WRITE_ZEROES))
		return q->limits.max_write_zeroes_sectors;		/* NVMe Write Zeroes 명령 최대 길이; namespace NAWUN/NAWUPN 반영(추정) */

	if (rq->cmd_flags & REQ_ATOMIC)
		return q->limits.atomic_write_max_sectors;		/* NVMe Atomic Write(NAWUN) 경계; atomic unit를 넘어서면 분할 필요 */

	return q->limits.max_sectors;					/* 일반 NVMe READ/WRITE; MDTS와 max_hw_sectors 중 작은 값 */
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
/*
 * [한국어]
 * blk_flush_integrity - integrity(PI) 관련 workqueue 작업을 모두 완료 대기
 *
 * @return: 없음
 *
 * 모듈 언로드나 시스템 종료 등에서 integrity 관련 비동기 작업(있다면)이
 * 남아있지 않도록 flush한다. 실제 정의는 block/blk-integrity.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 블록 계층 종료/언로드 경로.
 * 피호출자: (정의부 참고, 내부적으로 관련 workqueue를 flush).
 *
 * 호출 체인:
 *   (모듈/서브시스템 정리 경로) -> [blk_flush_integrity]
 */
void blk_flush_integrity(void);
/*
 * [한국어]
 * bio_integrity_free - bio에 붙은 integrity payload(bip) 해제
 *
 * @bio: integrity payload를 해제할 bio
 * @return: 없음
 *
 * bio_uninit() 경로에서 호출되어, bio_integrity_alloc()으로 할당된
 * bio_integrity_payload(NVMe PI Guard/APP/REF 태그를 담는 buffer)와 그
 * bvec 배열을 mempool/slab으로 반납한다. 실제 정의는 block/bio-integrity.c.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 bio 해제가 일어나는 임의 컨텍스트.
 * 호출자: bio_uninit(), bio_free().
 * 피호출자: mempool_free(), bvec_free() 등(정의부 참고).
 *
 * 호출 체인:
 *   bio_uninit -> [bio_integrity_free]
 */
void bio_integrity_free(struct bio *bio);

/*
 * Integrity payloads can either be owned by the submitter, in which case
 * bio_uninit will free them, or owned and generated by the block layer,
 * in which case we'll verify them here (for reads) and free them before
 * the bio is handed back to the submitted.
 */
/*
 * [한국어]
 * __bio_integrity_endio - 블록 계층이 생성한 PI(Protection Information) 태그를 검증
 *
 * @bio: 완료된 bio (읽기 완료 시 Guard/APP/REF 태그 검증 대상)
 * @return: true=검증 통과(또는 검증 불필요), false=검증 실패(데이터 손상 의심)
 *
 * NVMe end-to-end data protection(T10 DIF/DIX 계열)이 활성화된 namespace에서
 * 읽기 완료 시, CQE와 함께 반환된 데이터에 대해 Guard(CRC)/Application/
 * Reference 태그를 검증한다. 실제 정의는 block/bio-integrity.c 또는
 * block/t10-pi.c 계열에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 컨텍스트 (bio_endio 경로).
 * 호출자: bio_integrity_endio() (본 파일, 인라인 래퍼).
 * 피호출자: T10-PI 검증 루틴(t10-pi.c).
 *
 * 호출 체인:
 *   bio_endio -> bio_integrity_endio -> [__bio_integrity_endio]
 */
bool __bio_integrity_endio(struct bio *bio);
/*
 * [한국어]
 * bio_integrity_endio - bio 완료 시 PI 검증이 필요한지 판단 후 위임
 *
 * @bio: 완료된 bio
 * @return: true=검증 통과 또는 검증 대상 아님, false=검증 실패
 *
 * bip_flags에 BIP_BLOCK_INTEGRITY가 설정된 경우(블록 계층이 직접 생성/
 * 소유한 PI 태그)에만 실제 검증(__bio_integrity_endio)을 수행한다.
 * submitter가 자체적으로 PI를 관리하는 경우(BIP_BLOCK_INTEGRITY 미설정)
 * bio_uninit이 나중에 free할 뿐, 여기서는 검증을 건너뛴다.
 * 실행 컨텍스트: NVMe CQ 완료 경로 (bio_endio 내부에서 호출).
 * 호출자: bio_endio() (bio.c).
 * 피호출자: bio_integrity(), __bio_integrity_endio().
 *
 * 호출 체인:
 *   bio_endio -> [bio_integrity_endio] -> __bio_integrity_endio
 */
static inline bool bio_integrity_endio(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* NVMe PI metadata를 담은 bip; Guard/APP/REF 태그 포함 */

	if (bip && (bip->bip_flags & BIP_BLOCK_INTEGRITY))		/* 블록 계층 생성/소유 PI 태그일 때만 검증; NVMe CQE 수신 후 데이터와 비교 */
		return __bio_integrity_endio(bio);
	return true;								/* PI 없거나 submitter 소유면 검증 스킵; NVMe end-to-end protection off 상태 */
}

/*
 * [한국어]
 * blk_integrity_merge_rq - 두 request의 PI 설정이 병합 가능한 조합인지 검사
 *
 * @q: request_queue (첫 인자, 이름 없는 프로토타입 — integrity profile 보유)
 * @req: (두 번째) 첫 번째 request
 * @next: (세 번째) 병합하려는 두 번째 request
 * @return: true=PI 설정이 호환되어 병합 가능, false=병합 불가
 *
 * 두 request의 integrity(PI) 프로파일, 태그 크기, 인터리브 여부 등이
 * 일치하지 않으면 하나의 NVMe 명령으로 합칠 수 없으므로 병합을 거부한다.
 * 실제 정의는 block/blk-integrity.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (병합 판단 경로).
 * 호출자: blk_attempt_req_merge()/attempt_merge() (blk-merge.c).
 *
 * 호출 체인:
 *   attempt_merge -> [blk_integrity_merge_rq]
 */
bool blk_integrity_merge_rq(struct request_queue *, struct request *,
		struct request *);
/*
 * [한국어]
 * blk_integrity_merge_bio - request와 새 bio의 PI 설정이 병합 가능한지 검사
 *
 * @q: request_queue
 * @req: 기존 request
 * @bio: 병합하려는 새 bio
 * @return: true=병합 가능, false=병합 불가
 *
 * blk_integrity_merge_rq()와 동일한 취지이나 대상이 (request, request) 쌍이
 * 아니라 (request, bio) 쌍이다. ll_back_merge_fn()/blk_rq_merge_ok()에서
 * bio를 기존 request에 병합할 수 있는지 판단할 때 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: ll_new_hw_segment(), blk_rq_merge_ok() (blk-merge.c).
 *
 * 호출 체인:
 *   ll_back_merge_fn -> ll_new_hw_segment -> [blk_integrity_merge_bio]
 */
bool blk_integrity_merge_bio(struct request_queue *, struct request *,
		struct bio *);

/*
 * [한국어]
 * integrity_req_gap_back_merge - request 뒤에 bio를 병합할 때 integrity gap 검사
 *
 * @req: 기존 request (뒤에 next를 병합하려는 대상)
 * @next: 병합 후보 bio
 * @return: true=integrity buffer 사이에 gap 존재(병합 거부해야 함), false=gap 없음
 *
 * NVMe PI metadata는 데이터 PRP/SGL과 별도의 buffer(bip_vec)로 관리되므로,
 * 데이터 세그먼트가 물리적으로 인접해도 PI buffer는 별도로 gap 검사가
 * 필요하다. req의 마지막 integrity bvec과 next의 첫 integrity bvec 사이의
 * virt_boundary gap을 bvec_gap_to_prev()로 검사한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: ll_back_merge_fn() 등 back-merge 검사 경로(blk-merge.c).
 * 피호출자: bio_integrity(), bvec_gap_to_prev().
 *
 * 호출 체인:
 *   ll_back_merge_fn -> [integrity_req_gap_back_merge] -> bvec_gap_to_prev
 */
static inline bool integrity_req_gap_back_merge(struct request *req,
		struct bio *next)
{
	struct bio_integrity_payload *bip = bio_integrity(req->bio);		/* 현재 NVMe rq의 PI payload; PRP data와 분리된 integrity buffer */
	struct bio_integrity_payload *bip_next = bio_integrity(next);		/* 병합 후보 bio의 PI payload; 연속 integrity 영역 필요 */

	return bvec_gap_to_prev(&req->q->limits,
				&bip->bip_vec[bip->bip_vcnt - 1],		/* 마지막 integrity bvec; NVMe PI metadata도 segment boundary 검사 필요 */
				bip_next->bip_vec[0].bv_offset);		/* 다음 integrity buffer offset; virt_boundary 통과 시 병합 거부 */
}

/*
 * [한국어]
 * integrity_req_gap_front_merge - request 앞에 bio를 병합할 때 integrity gap 검사
 *
 * @req: 기존 request (앞에 bio를 병합하려는 대상)
 * @bio: 병합 후보 bio (req보다 앞쪽 LBA)
 * @return: true=gap 존재(병합 거부), false=gap 없음
 *
 * integrity_req_gap_back_merge()의 front-merge 버전. bio가 req 앞에
 * 붙는 경우이므로 bip/bip_next의 역할이 반대로 배정된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: front-merge 검사 경로(blk-merge.c).
 * 피호출자: bio_integrity(), bvec_gap_to_prev().
 *
 * 호출 체인:
 *   (front merge 경로) -> [integrity_req_gap_front_merge] -> bvec_gap_to_prev
 */
static inline bool integrity_req_gap_front_merge(struct request *req,
		struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);		/* 앞에 붙일 bio의 PI payload */
	struct bio_integrity_payload *bip_next = bio_integrity(req->bio);	/* 기존 NVMe rq의 PI payload */

	return bvec_gap_to_prev(&req->q->limits,
				&bip->bip_vec[bip->bip_vcnt - 1],		/* front merge에서도 integrity bvec 간 gap 검사; NVMe PI 연속성 유지 */
				bip_next->bip_vec[0].bv_offset);
}

/* [한국어] blk_integrity_attr_group — /sys/block/<disk>/integrity/ 아래 sysfs 속성
 * 그룹(format, tag_size, protection_interval_bytes 등). request_queue의
 * kobject에 등록되어 사용자공간이 NVMe namespace의 PI 설정을 조회할 수
 * 있게 한다. 실제 정의는 block/blk-integrity.c. */
extern const struct attribute_group blk_integrity_attr_group;
#else /* CONFIG_BLK_DEV_INTEGRITY */
/*
 * [한국어]
 * blk_integrity_merge_rq - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁
 *
 * @rq: 사용되지 않음 (request_queue)
 * @r1: 사용되지 않음
 * @r2: 사용되지 않음
 * @return: 항상 true
 *
 * PI 기능 자체가 빌드에서 빠진 경우, 병합 판단에서 integrity 검사를
 * 완전히 생략하기 위해 항상 통과시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_rq_merge_ok() 등 (CONFIG_BLK_DEV_INTEGRITY=n 빌드에서).
 */
static inline bool blk_integrity_merge_rq(struct request_queue *rq,
		struct request *r1, struct request *r2)
{
	return true;								/* PI off -> 병합 항상 허용; NVMe PI metadata 고려 없이 PRP/SGL만 계산 */
}
/*
 * [한국어]
 * blk_integrity_merge_bio - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁
 *
 * @rq: 사용되지 않음
 * @r: 사용되지 않음
 * @b: 사용되지 않음
 * @return: 항상 true
 *
 * 위 blk_integrity_merge_rq 스텁과 동일한 취지 — PI 미사용 커널에서는
 * bio 병합 시에도 검사를 생략한다.
 */
static inline bool blk_integrity_merge_bio(struct request_queue *rq,
		struct request *r, struct bio *b)
{
	return true;								/* NVMe DIF/DIX 비활성 namespace; segment 병합 제약 없음 */
}
/*
 * [한국어]
 * integrity_req_gap_back_merge - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁
 *
 * @req: 사용되지 않음
 * @next: 사용되지 않음
 * @return: 항상 false (gap 없음으로 간주)
 */
static inline bool integrity_req_gap_back_merge(struct request *req,
		struct bio *next)
{
	return false;								/* gap 없음; NVMe PI buffer 분리 불필요 */
}
/*
 * [한국어]
 * integrity_req_gap_front_merge - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁
 *
 * @req: 사용되지 않음
 * @bio: 사용되지 않음
 * @return: 항상 false
 */
static inline bool integrity_req_gap_front_merge(struct request *req,
		struct bio *bio)
{
	return false;								/* front merge gap 없음 */
}

/*
 * [한국어]
 * blk_flush_integrity - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁 (아무 동작 없음)
 *
 * @return: 없음
 */
static inline void blk_flush_integrity(void)
{
}
/*
 * [한국어]
 * bio_integrity_endio - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁
 *
 * @bio: 사용되지 않음
 * @return: 항상 true (검증 없이 통과)
 */
static inline bool bio_integrity_endio(struct bio *bio)
{
	return true;								/* PI off; NVMe CQE 완료 후 추가 검증 없이 상위로 전달 */
}
/*
 * [한국어]
 * bio_integrity_free - CONFIG_BLK_DEV_INTEGRITY 비활성 시 스텁 (아무 동작 없음)
 *
 * @bio: 사용되지 않음
 * @return: 없음
 */
static inline void bio_integrity_free(struct bio *bio)
{
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

/*
 * [한국어]
 * blk_rq_timeout - 사용자가 요청한 timeout 값을 안전한 범위로 재계산
 *
 * @timeout: 드라이버/상위 계층이 요청한 원하는 timeout(jiffies)
 * @return: BLK_MAX_TIMEOUT 이내로 clamp된 실제 사용할 timeout(jiffies)
 *
 * timeout이 너무 크면(0에 가깝거나 unsigned long 연산이 넘칠 수 있는
 * 값이면) BLK_MAX_TIMEOUT(5초) 단위로 잘라서 반환한다. 실제 요청이 그보다
 * 오래 걸려야 한다면 타이머가 여러 번 재등록되며 누적된다. 실제 정의는
 * block/blk-timeout.c에 있다.
 * 실행 컨텍스트: 임의 컨텍스트 (타이머 등록 경로).
 * 호출자: blk_add_timer() (본 파일 선언, blk-timeout.c 정의).
 *
 * 호출 체인:
 *   blk_add_timer -> [blk_rq_timeout]
 */
unsigned long blk_rq_timeout(unsigned long timeout);
/*
 * [한국어]
 * blk_add_timer - request(NVMe CID)에 대한 timeout 타이머를 등록/갱신
 *
 * @req: timeout을 등록할 request
 * @return: 없음
 *
 * NVMe 컨트롤러가 이 CID에 대해 CQE를 제때 반환하지 않을 경우를 대비해,
 * request가 SQ에 삽입되는 시점에 deadline을 기록한다. hctx 단위로 관리되는
 * timeout 워크가 주기적으로 deadline이 지난 request를 찾아 mq_ops->timeout
 * 콜백(NVMe라면 nvme_timeout())을 호출해 abort/reset을 유도한다. 실제
 * 정의는 block/blk-timeout.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 인터럽트 컨텍스트(request 할당/
 *   dispatch 경로에 따라 다름).
 * 호출자: blk_mq_start_request() 등 request가 진행 상태로 전이하는 지점.
 * 피호출자: blk_rq_timeout().
 *
 * 호출 체인:
 *   blk_mq_start_request -> [blk_add_timer] -> blk_rq_timeout
 */
void blk_add_timer(struct request *req);

enum bio_merge_status {
	BIO_MERGE_OK,
	/* [한국어] bio가 기존 request에 성공적으로 병합됨.
	 * 역할: 호출자(blk_attempt_plug_merge, blk_bio_list_merge 등)가 새
	 *   request를 만들지 않고 기존 request 재사용을 계속 진행하도록 신호.
	 * 발생 위치: bio_attempt_back_merge()/bio_attempt_front_merge()/
	 *   bio_attempt_discard_merge()가 실제 병합에 성공했을 때 반환.
	 * 동기화: 병합 자체는 request_queue 락 없이 진행되며(blk-mq는 lockless
	 *   병합을 지향), 병합 대상 request가 아직 dispatch되지 않았음을
	 *   보장하는 것은 plug list/scheduler 자료구조의 소유권 규칙이다. */
	BIO_MERGE_NONE,
	/* [한국어] 병합을 아예 시도하지 않음(LBA가 인접하지 않는 등 애초에
	 *   병합 후보가 아님).
	 * 역할: 호출자가 다음 후보 request로 넘어가거나, 새로운 request를
	 *   만들어야 함을 알림 — 에러는 아니다.
	 * 발생 위치: blk_try_merge()가 ELEVATOR_NO_MERGE를 반환했을 때 등. */
	BIO_MERGE_FAILED,
	/* [한국어] 병합 후보였으나(LBA는 인접) 실제로는 실패함(세그먼트 한도
	 *   초과, integrity/crypto 불일치, REQ_NOMERGE 등).
	 * 역할: 호출자가 이 request로는 병합할 수 없음을 확정하고 다음 후보를
	 *   보거나 새 request 생성으로 넘어가게 함.
	 * 발생 위치: ll_back_merge_fn()/ll_new_hw_segment() 등이 세그먼트/
	 *   integrity 검사에서 실패했을 때. */
};

/*
 * [한국어]
 * bio_attempt_back_merge - request 뒤에 bio를 실제로 병합
 *
 * @req: 병합 대상 request (LBA가 낮은 쪽, 이 request가 유지됨)
 * @bio: 병합할 새 bio (LBA가 req 뒤에 이어짐)
 * @nr_segs: bio의 세그먼트 수(사전 계산된 값, 재계산 비용 절감)
 * @return: BIO_MERGE_OK(성공) 또는 BIO_MERGE_FAILED(세그먼트/정책 위반)
 *
 * ll_back_merge_fn()으로 물리적/논리적 병합 가능성을 검증한 뒤, bio를
 * req->bio 리스트 끝에 연결하고 req->__data_len, req->nr_phys_segments를
 * 갱신한다. 병합이 성공하면 이 request가 NVMe SQ에 삽입될 때 하나의 명령
 * (SLBA+NLB)으로 더 많은 데이터를 담아 doorbell 횟수를 줄인다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (병합 판단/수행 경로).
 * 호출자: blk_attempt_plug_merge(), blk_bio_list_merge(), blk-mq-sched.c의
 *   스케줄러 병합 경로.
 * 피호출자: ll_back_merge_fn(), blk_account_io_merge_bio().
 *
 * 호출 체인:
 *   submit_bio -> blk_attempt_plug_merge -> [bio_attempt_back_merge]
 *   -> ll_back_merge_fn
 */
enum bio_merge_status bio_attempt_back_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs);
/*
 * [한국어]
 * blk_attempt_plug_merge - 현재 태스크의 plug list에서 병합 대상을 찾아 시도
 *
 * @q: bio가 큐잉되는 request_queue
 * @bio: 새로 들어온 bio
 * @nr_segs: bio의 세그먼트 수
 * @return: true=plug list의 어떤 request와 병합 성공, false=병합 대상 없음/실패
 *
 * Plugging(current->plug)은 동일 issuer가 짧은 시간에 제출하는 여러 bio를
 * 스케줄러에 보내기 전에 먼저 모아, NVMe SQ에 들어갈 request의 크기를
 * 키우는 메커니즘이다. plug->mq_list의 tail(가장 최근 request)을 우선
 * 검사하고, multiple_queues가 설정된 경우 다른 큐 소속 request까지 순회한다.
 * 호출자는 미리 !blk_queue_nomerges(q)를 확인해야 한다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_mq_submit_bio() (blk-mq.c).
 * 피호출자: blk_attempt_bio_merge() (내부 정적 함수).
 *
 * 호출 체인:
 *   submit_bio -> blk_mq_submit_bio -> [blk_attempt_plug_merge]
 */
bool blk_attempt_plug_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs);
/*
 * [한국어]
 * blk_bio_list_merge - request 리스트를 역순 순회하며 병합 가능한 항목 탐색
 *
 * @q: request_queue
 * @list: 순회할 request 리스트 (예: 스케줄러의 dispatch 대기열)
 * @bio: 병합하려는 새 bio
 * @nr_segs: bio의 세그먼트 수
 * @return: true=리스트 내 어떤 request와 병합 성공, false=실패
 *
 * NVMe multi-queue 스케줄러가 I/O를 dispatch하기 전에 호출되어, 리스트
 * 내에서 연속된 LBA를 가진 request를 찾아 병합한다. 리스트는 최근
 * 추가된 순서(역순)로 순회해 병합 성공 가능성이 높은 항목을 먼저 검사한다.
 * 실제 정의는 block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 스케줄러 병합 경로(예: kyber, bfq, mq-deadline의 bio 병합 훅).
 * 피호출자: blk_try_merge(), bio_attempt_back_merge()/bio_attempt_front_merge().
 *
 * 호출 체인:
 *   (IO 스케줄러 bio 병합 훅) -> [blk_bio_list_merge] -> blk_try_merge
 */
bool blk_bio_list_merge(struct request_queue *q, struct list_head *list,
			struct bio *bio, unsigned int nr_segs);

/*
 * Plug flush limits
 */
/* [한국어] BLK_MAX_REQUEST_COUNT — 하나의 plug list가 담을 수 있는 request 개수 상한.
 * 왜 필요한가: plug list가 무한정 쌓이면 blk_finish_plug() 시점에 한꺼번에 dispatch되는
 * request가 너무 많아져 지연시간 스파이크가 발생한다. 32개로 제한해 NVMe I/O
 * 스케줄러가 처리하는 배치(batch) 크기의 상한선을 둔다. */
#define BLK_MAX_REQUEST_COUNT	32
/* [한국어] BLK_PLUG_FLUSH_SIZE — plug list의 누적 바이트 수가 이 값을 넘으면 즉시 flush.
 * 왜 필요한가: 큰 순차 I/O(예: 128KB 이상)가 plug에 쌓이면 이미 병합 이득이 충분하므로,
 * 더 오래 들고 있어 지연시간만 늘리지 않도록 조기에 NVMe multi-queue로 흘려보낸다. */
#define BLK_PLUG_FLUSH_SIZE	(128 * 1024)

/*
 * Internal elevator interface
 */
/* [한국어] ELV_ON_HASH(rq) — request가 스케줄러의 병합용 해시 테이블에 등록되어 있는지 검사.
 * 왜 필요한가: elevator(mq-deadline, bfq, kyber)는 LBA 기반 빠른 병합 탐색을 위해 rq를
 * 해시 테이블에 넣어 두는데, dispatch되어 해시에서 빠진 rq를 재병합 시도하면 안 되므로
 * RQF_HASHED 플래그로 "현재 병합 후보 풀에 있는지"를 구분한다(추정). */
#define ELV_ON_HASH(rq) ((rq)->rq_flags & RQF_HASHED)

/*
 * [한국어]
 * blk_insert_flush - PREFLUSH/FUA 요청을 flush 상태머신에 진입시켜 시퀀싱
 *
 * @rq: REQ_PREFLUSH 또는 REQ_FUA 플래그가 설정된 request
 * @return: true=flush 상태머신이 이 request를 소비함(호출자는 추가 처리 불필요),
 *          false=NVMe VWC/FUA 지원 상황상 일반 경로로 처리해야 함
 *
 * REQ_PREFLUSH/REQ_FUA 플래그와 NVMe 컨트롤러의 VWC(Volatile Write Cache)/
 * FUA 지원 여부를 조합해 실행할 시퀀스(policy)를 결정한다: policy가 0이면
 * 즉시 완료, DATA만이면 일반 경로, DATA+POSTFLUSH면 FUA 미지원이라 데이터
 * 쓰기 후 별도 FLUSH, PREFLUSH+DATA(+POSTFLUSH)면 FLUSH를 먼저 보내고
 * 데이터를 쓰는 전체 시퀀스를 구성한다. REQ_PREFLUSH 플래그는 시퀀스가
 * 결정된 뒤 드라이버에 전달되기 전에 제거된다. 실제 정의는
 * block/blk-flush.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (request 할당/제출 경로).
 * 호출자: blk_mq_submit_bio() -> blk_mq_get_request() 경로 (blk-mq.c).
 * 피호출자: blk_flush_complete_seq(), blk_mq_end_request(), blk_kick_flush().
 *
 * 호출 체인:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *     -> [blk_insert_flush] -> blk_flush_complete_seq / blk_mq_end_request
 */
bool blk_insert_flush(struct request *rq);

/*
 * [한국어]
 * elv_update_nr_hw_queues - 하드웨어 큐 개수 변경 시 elevator를 강제 재부착
 *
 * @q: 대상 request_queue
 * @ctx: 스케줄러 전환 컨텍스트(이름, 타입, 새 리소스 등을 담는 elv_change_ctx)
 * @return: 없음
 *
 * NVMe 멀티큐 컨트롤러가 nr_io_queues를 바꾸면(예: CPU hotplug, 인터럽트
 * 벡터 재배치) 각 blk_mq_hw_ctx와 tag_set이 새 큐 개수에 맞춰 갱신되므로,
 * elevator(스케줄러)도 동일한 hctx 배치에 맞춰 다시 초기화해야 한다.
 * queue가 freeze된 상태에서 호출되어야 하며, 내부에서
 * elevator_switch()/elevator_change_done()을 호출한다. 실제 정의는
 * block/elevator.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트, q->mq_freeze_depth > 0 상태에서 호출.
 * 호출자: blk_mq_update_nr_hw_queues() (blk-mq.c).
 * 피호출자: elevator_switch(), elevator_change_done(), blk_mq_free_sched_res().
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues -> [elv_update_nr_hw_queues] -> elevator_switch
 */
void elv_update_nr_hw_queues(struct request_queue *q,
		struct elv_change_ctx *ctx);

/*
 * [한국어]
 * elevator_set_default - 장치 등록 시 기본 IO 스케줄러를 연결
 *
 * @q: 대상 request_queue
 * @return: 없음
 *
 * 단일 하드웨어 큐이거나 shared tags를 사용하는 경우 "mq-deadline"을
 * 시도하고, 실패하거나 멀티큐(NVMe의 일반적인 구성)이면 "none"(스케줄러
 * 없음) 상태로 남긴다. 고성능 NVMe SSD는 커널 자체 재정렬/병합 오버헤드
 * 없이 blk-mq가 바로 dispatch하는 "none"이 기본값이 되는 경우가 많다.
 * 실제 정의는 block/elevator.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk() -> blk_register_queue() 경로.
 * 피호출자: elevator_change(), elevator_find_get(), elevator_put().
 *
 * 호출 체인:
 *   add_disk -> blk_register_queue -> [elevator_set_default]
 */
void elevator_set_default(struct request_queue *q);
/*
 * [한국어]
 * elevator_set_none - 스케줄러를 "none"(스케줄러 없음)으로 강제 전환
 *
 * @q: 대상 request_queue
 * @return: 없음
 *
 * 사용자가 sysfs(/sys/block/<disk>/queue/scheduler)에 "none"을 쓰거나,
 * BLK_MQ_F_NO_SCHED_BY_DEFAULT 플래그를 가진 드라이버가 초기화될 때
 * 호출된다. NVMe 고성능 장치에서 소프트웨어 스케줄링 오버헤드를 제거하고
 * bio/request를 곧바로 blk-mq를 거쳐 nvme_queue_rq()로 전달하려는 목적이다.
 * 실제 정의는 block/elevator.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: elv_iosched_store() (sysfs write 핸들러), 드라이버 초기화 경로.
 * 피호출자: elevator_switch(), blk_queue_flag_clear().
 *
 * 호출 체인:
 *   elv_iosched_store -> [elevator_set_none] -> elevator_switch
 */
void elevator_set_none(struct request_queue *q);

/*
 * [한국어]
 * part_size_show - sysfs "size" 속성: 파티션/디스크 크기(섹터) 출력
 *
 * @dev: 대상 block_device를 감싼 struct device
 * @attr: sysfs device_attribute (사용되지 않고 dev만으로 충분)
 * @buf: 결과 문자열을 쓸 PAGE_SIZE 버퍼
 * @return: buf에 쓴 바이트 수
 *
 * /sys/block/nvmeXnY/size (또는 파티션의 size)를 읽을 때 호출되어
 * bdev_nr_sectors()를 10진 문자열로 변환해 반환한다. 실제 정의는
 * block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read 시스템 호출 경로).
 * 호출자: sysfs 코어(kernfs)가 device_attribute.show 콜백으로 호출.
 *
 * 호출 체인:
 *   sysfs read -> [part_size_show]
 */
ssize_t part_size_show(struct device *dev, struct device_attribute *attr,
		char *buf);
/*
 * [한국어]
 * part_stat_show - sysfs "stat" 속성: /proc/diskstats 형식의 I/O 통계 출력
 *
 * @dev: 대상 block_device를 감싼 struct device
 * @attr: sysfs device_attribute
 * @buf: 결과 문자열을 쓸 버퍼
 * @return: buf에 쓴 바이트 수
 *
 * 읽기/쓰기/discard 각각의 완료 수, 병합 수, 섹터 수, 누적 시간(ms) 등을
 * part_stat_read_all()로 per-CPU 통계를 집계해 출력한다. NVMe namespace의
 * iostat 도구가 이 파일을 파싱한다. 실제 정의는 block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: sysfs 코어.
 *
 * 호출 체인:
 *   sysfs read -> [part_stat_show] -> part_stat_read_all
 */
ssize_t part_stat_show(struct device *dev, struct device_attribute *attr,
		char *buf);
/*
 * [한국어]
 * part_inflight_show - sysfs "inflight" 속성: 현재 진행 중인 I/O 수 출력
 *
 * @dev: 대상 block_device를 감싼 struct device
 * @attr: sysfs device_attribute
 * @buf: 결과 문자열을 쓸 버퍼
 * @return: buf에 쓴 바이트 수 (읽기/쓰기 두 개의 숫자)
 *
 * 현재 이 파티션/디스크에서 완료되지 않은(inflight) 읽기·쓰기 요청 수를
 * 출력한다. NVMe 큐 깊이 모니터링에 사용될 수 있다. 실제 정의는
 * block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs read -> [part_inflight_show]
 */
ssize_t part_inflight_show(struct device *dev, struct device_attribute *attr,
		char *buf);
/*
 * [한국어]
 * part_fail_show - sysfs "make-it-fail" 속성 읽기: fault injection 활성 여부
 *
 * @dev: 대상 block_device를 감싼 struct device
 * @attr: sysfs device_attribute
 * @buf: 결과 문자열을 쓸 버퍼
 * @return: buf에 쓴 바이트 수 (0 또는 1)
 *
 * CONFIG_FAIL_MAKE_REQUEST가 활성화된 커널에서, 이 장치에 BD_MAKE_IT_FAIL
 * 플래그가 설정되어 있는지를 출력한다. 실제 정의는 block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs read -> [part_fail_show]
 */
ssize_t part_fail_show(struct device *dev, struct device_attribute *attr,
		char *buf);
/*
 * [한국어]
 * part_fail_store - sysfs "make-it-fail" 속성 쓰기: fault injection 토글
 *
 * @dev: 대상 block_device를 감싼 struct device
 * @attr: sysfs device_attribute
 * @buf: 사용자가 쓴 문자열("0" 또는 "1")
 * @count: buf 길이
 * @return: 성공 시 count, 실패 시 음수 errno
 *
 * BD_MAKE_IT_FAIL 플래그를 설정/해제해 should_fail_request()가 인위적으로
 * I/O를 실패시키도록 만든다. 드라이버(NVMe 포함)의 에러 처리 경로를
 * 테스트하는 용도다. 실제 정의는 block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs write -> [part_fail_store] -> should_fail_request(이후 영향)
 */
ssize_t part_fail_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);
/*
 * [한국어]
 * part_timeout_show - sysfs "io-timeout-fail" 속성 읽기: timeout fault injection 상태
 *
 * @dev: 대상 struct device
 * @attr: sysfs device_attribute (첫 번째 미사용 인자와 이름 없이 선언)
 * @buf: 결과 문자열을 쓸 버퍼
 * @return: buf에 쓴 바이트 수
 *
 * request timeout을 인위적으로 유발하는 fault injection 설정 상태를
 * 출력한다. 실제 정의는 block/blk-timeout.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs read -> [part_timeout_show]
 */
ssize_t part_timeout_show(struct device *, struct device_attribute *, char *);
/*
 * [한국어]
 * part_timeout_store - sysfs "io-timeout-fail" 속성 쓰기: timeout fault injection 설정
 *
 * @dev: 대상 struct device (첫 인자, 이름 생략된 프로토타입)
 * @attr: sysfs device_attribute
 * @buf: 사용자가 쓴 설정 문자열
 * @count: buf 길이 (마지막 인자, 이름 생략)
 * @return: 성공 시 소비한 바이트 수, 실패 시 음수 errno
 *
 * NVMe request가 마치 컨트롤러 무응답으로 timeout된 것처럼 인위적으로
 * 만들어, timeout/abort/reset 경로를 테스트할 수 있게 하는 디버그 인터페이스다.
 * 실제 정의는 block/blk-timeout.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs write -> [part_timeout_store]
 */
ssize_t part_timeout_store(struct device *, struct device_attribute *,
				const char *, size_t);

/*
 * [한국어]
 * bio_split_discard - Discard/Secure Erase bio를 컨트롤러 한도에 맞춰 분할
 *
 * @bio: 분할 대상 bio (REQ_OP_DISCARD 또는 REQ_OP_SECURE_ERASE)
 * @lim: 적용할 queue_limits (max_discard_sectors/max_secure_erase_sectors,
 *       discard_granularity, discard_alignment 등)
 * @nsegs: [out] 분할된 앞부분 bio의 세그먼트 수(discard는 보통 1)
 * @return: 분할되어 먼저 제출할 앞부분 bio, 분할 불필요 시 원본 bio 그대로
 *
 * NVMe Dataset Management(DSM, opcode 0x9) 명령이 한 번에 처리 가능한
 * range 크기(max_discard_sectors)를 넘는 discard bio를, granularity/
 * alignment 경계에 맞춰 자른다. 정렬이 어긋나면 이전 aligned 경계에서
 * 잘라 컨트롤러가 Deallocate를 거부하지 않게 한다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (제출 경로).
 * 호출자: __bio_split_to_limits() (본 파일).
 * 피호출자: bio_submit_split() (blk-merge.c 내부).
 *
 * 호출 체인:
 *   submit_bio -> __bio_split_to_limits -> [bio_split_discard]
 */
struct bio *bio_split_discard(struct bio *bio, const struct queue_limits *lim,
		unsigned *nsegs);
/*
 * [한국어]
 * bio_split_write_zeroes - Write Zeroes bio를 컨트롤러 한도에 맞춰 분할
 *
 * @bio: 분할 대상 bio (REQ_OP_WRITE_ZEROES)
 * @lim: 적용할 queue_limits (max_write_zeroes_sectors 등)
 * @nsegs: [out] 분할된 앞부분 bio의 세그먼트 수
 * @return: 분할된 앞부분 bio, 분할 불필요 시 원본 bio
 *
 * NVMe Write Zeroes 명령이 한 번에 처리 가능한 최대 섹터 수를 넘는 경우
 * 분할한다. max_sectors가 0이면(Write Zeroes 미지원 장치) 분할 없이
 * 원본을 그대로 반환한다. 실제 정의는 block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: __bio_split_to_limits() (본 파일).
 *
 * 호출 체인:
 *   submit_bio -> __bio_split_to_limits -> [bio_split_write_zeroes]
 */
struct bio *bio_split_write_zeroes(struct bio *bio,
		const struct queue_limits *lim, unsigned *nsegs);
/*
 * [한국어]
 * bio_split_rw - READ/WRITE bio를 NVMe PRP/SGL 한계에 맞춰 분할
 *
 * @bio: 분할 대상 bio (REQ_OP_READ 또는 REQ_OP_WRITE)
 * @lim: 적용할 queue_limits (max_sectors, max_segments, seg_boundary_mask 등)
 * @nr_segs: [out] 분할된 앞부분 bio의 물리 세그먼트 수
 * @return: 분할된 앞부분 bio, 분할 불필요 시 원본 bio (또는 REQ_ATOMIC/
 *          REQ_NOWAIT 상황에서 분할이 불가능하면 ERR_PTR)
 *
 * get_max_io_size()가 계산한 NVMe MDTS(Maximum Data Transfer Size)와
 * 물리 정렬을 함께 고려한 최대 바이트 수를 기준으로 분할 지점을 찾는다.
 * REQ_ATOMIC(원자적 쓰기) bio는 분할 시 원자성이 깨지므로 -EINVAL,
 * REQ_NOWAIT bio는 -EAGAIN으로 즉시 실패 처리된다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: __bio_split_to_limits() (본 파일), bio_split_to_limits() (blk-merge.c).
 * 피호출자: get_max_io_size(), bio_split_rw_at(), bio_submit_split().
 *
 * 호출 체인:
 *   submit_bio -> __bio_split_to_limits -> [bio_split_rw]
 *   -> bio_submit_split -> nvme_pci_setup_data_prp/nvme_pci_setup_data_sgl
 */
struct bio *bio_split_rw(struct bio *bio, const struct queue_limits *lim,
		unsigned *nr_segs);
/*
 * [한국어]
 * bio_split_zone_append - Zone Append bio의 세그먼트 수 계산(분할은 절대 하지 않음)
 *
 * @bio: 대상 bio (REQ_OP_ZONE_APPEND)
 * @lim: 적용할 queue_limits (max_zone_append_sectors)
 * @nr_segs: [out] bio의 물리 세그먼트 수
 * @return: 항상 원본 bio 그대로(정상 시), 한도 초과가 감지되면 WARN 후 처리
 *
 * NVMe ZNS의 Zone Append(0x7D) 명령은 실제 쓰기 위치(LBA)를 컨트롤러가
 * zone write pointer 기준으로 결정해 CQE에 담아 반환하므로, 블록 계층이
 * bio를 분할해 버리면 쓰기 순서와 위치 정보가 깨진다. 그래서 이 함수는
 * 절대로 분할하지 않으며, bio_split_rw_at()의 세그먼트 계산 로직만
 * 재사용해 "제출자가 올바른 크기의 bio를 만들었는지" 검증한다
 * (max_zone_append_sectors 초과 시 WARN_ON_ONCE). 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: __bio_split_to_limits() (본 파일).
 * 피호출자: bio_split_rw_at(), bio_submit_split().
 *
 * 호출 체인:
 *   submit_bio -> __bio_split_to_limits -> [bio_split_zone_append]
 */
struct bio *bio_split_zone_append(struct bio *bio,
		const struct queue_limits *lim, unsigned *nr_segs);

/*
 * All drivers must accept single-segments bios that are smaller than PAGE_SIZE.
 *
 * This is a quick and dirty check that relies on the fact that bi_io_vec[0] is
 * always valid if a bio has data.  The check might lead to occasional false
 * positives when bios are cloned, but compared to the performance impact of
 * cloned bios themselves the loop below doesn't matter anyway.
 */
/*
 * [한국어]
 * bio_may_need_split - 정밀 분할 계산 전에, 분할이 필요할 가능성을 빠르게 스크리닝
 *
 * @bio: 검사 대상 bio
 * @lim: 적용할 queue_limits
 * @return: true=분할이 필요할 수 있으므로 bio_split_rw() 등 정밀 검사로 진행,
 *          false=단일 segment로 충분히 처리 가능(분할 불필요 확정)
 *
 * chunk_sectors(RAID/stripe 경계)가 있거나, bio_vec이 없거나(비정상),
 * 첫 bvec만으로 bio 전체를 커버하지 못하거나, 단일 bvec이
 * max_fast_segment_size를 넘으면 true를 반환해 상세 분할 로직으로
 * 넘긴다. 대부분의 작은 NVMe I/O(4K~16K, 단일 페이지)는 이 빠른 검사에서
 * false로 판정되어 분할 계산 비용을 피한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (제출 hot path).
 * 호출자: __bio_split_to_limits() (본 파일).
 * 피호출자: __bvec_iter_bvec() (인라인 헬퍼).
 *
 * 호출 체인:
 *   __bio_split_to_limits -> [bio_may_need_split]
 */
static inline bool bio_may_need_split(struct bio *bio,
		const struct queue_limits *lim)
{
	const struct bio_vec *bv;

	if (lim->chunk_sectors)
		return true;							/* RAID/stripe 단위 존재; NVMe RAID 하에서 chunk 경계 넘으면 분할 필요 */

	if (!bio->bi_io_vec)
		return true;							/* bio_vec 없음(비정상); NVMe PRP/SGL 매핑 불가 -> 분할/오류 처리 */

	bv = __bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);		/* 첫 번째 유효 bvec; NVMe PRP0에 해당하는 page/offset/len */
	if (bio->bi_iter.bi_size > bv->bv_len - bio->bi_iter.bi_bvec_done)
		return true;							/* 첫 bvec만으로 bio 전체를 커버하지 못함; 여러 PRP/SGL entry 필요 -> 세부 분할 검사 */
	return bv->bv_len + bv->bv_offset > lim->max_fast_segment_size;	/* 단일 bvec이 max_fast_segment_size 초과; NVMe PRP segment 한도 위반 */
}

/**
 * __bio_split_to_limits - split a bio to fit the queue limits
 * @bio:     bio to be split
 * @lim:     queue limits to split based on
 * @nr_segs: returns the number of segments in the returned bio
 *
 * Check if @bio needs splitting based on the queue limits, and if so split off
 * a bio fitting the limits from the beginning of @bio and return it.  @bio is
 * shortened to the remainder and re-submitted.
 *
 * The split bio is allocated from @q->bio_split, which is provided by the
 * block layer.
 */
/*
 * [한국어]
 * __bio_split_to_limits - bio의 opcode에 따라 알맞은 분할 함수로 분기
 *
 * @bio: 분할 대상 bio
 * @lim: 적용할 queue_limits
 * @nr_segs: [out] 반환되는(앞부분) bio의 세그먼트 수
 * @return: 분할된 앞부분 bio(또는 분할 불필요 시 원본), 실패 시 ERR_PTR
 *
 * READ/WRITE는 bio_may_need_split()로 빠른 스크리닝 후 필요할 때만
 * bio_split_rw()를 호출하고, ZONE_APPEND/DISCARD/SECURE_ERASE/
 * WRITE_ZEROES는 각각 전용 분할 함수로 위임한다. 그 외 연산(passthrough
 * 등)은 블록 계층이 분할하지 않으므로 nr_segs=0으로 표시하고 원본을
 * 그대로 반환한다 — 이 경우 드라이버가 직접 세그먼트를 처리해야 한다.
 * 이 함수는 bio_split_to_limits()의 실제 구현이며, 두 곳(공개 API와
 * blk-mq 내부 경로)에서 재사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bio_split_to_limits() (blk-merge.c, 공개 API),
 *   blk_mq_submit_bio() 등 blk-mq 내부 제출 경로.
 * 피호출자: bio_may_need_split(), bio_split_rw(), bio_split_zone_append(),
 *   bio_split_discard(), bio_split_write_zeroes().
 *
 * 호출 체인:
 *   submit_bio -> blk_mq_submit_bio -> [__bio_split_to_limits]
 *   -> bio_split_rw / bio_split_discard / ...
 */
static inline struct bio *__bio_split_to_limits(struct bio *bio,
		const struct queue_limits *lim, unsigned int *nr_segs)
{
	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		if (bio_may_need_split(bio, lim))
			return bio_split_rw(bio, lim, nr_segs);	/* NVMe MDTS/max_segments/seg_boundary 초과 시 분할; 반환 bio는 limits 내 */
		*nr_segs = 1;						/* 분할 불필요; 단일 NVMe PRP/SGL segment로 처리 가능 */
		return bio;
	case REQ_OP_ZONE_APPEND:
		return bio_split_zone_append(bio, lim, nr_segs);	/* ZNS: zone write pointer 단위로 분할; NVMe ZONE_APPEND(0x7D) 경계 준수 */
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		return bio_split_discard(bio, lim, nr_segs);	/* NVMe Dataset Management range 개수 제한 적용 */
	case REQ_OP_WRITE_ZEROES:
		return bio_split_write_zeroes(bio, lim, nr_segs);	/* NVMe Write Zeroes 길이 제한; NAWUN/NAWUPN 단위 고려 */
	default:
		/* other operations can't be split */
		*nr_segs = 0;							/* NVMe passthrough/admin 등은 블록 계층에서 분할하지 않음; 드라이버가 처리 */
		return bio;
	}
}

/**
 * get_max_segment_size() - maximum number of bytes to add as a single segment
 * @lim: Request queue limits.
 * @paddr: address of the range to add
 * @len: maximum length available to add at @paddr
 *
 * Returns the maximum number of bytes of the range starting at @paddr that can
 * be added to a single segment.
 */
/*
 * [한국어]
 * get_max_segment_size - 한 물리 주소 범위에서 하나의 segment로 담을 수 있는 최대 길이
 *
 * @lim: 대상 queue_limits (seg_boundary_mask, max_segment_size)
 * @paddr: 이어붙이려는 범위의 시작 물리 주소
 * @len: @paddr부터 사용 가능한 최대 길이(바이트)
 * @return: 하나의 NVMe PRP/SGL segment로 표현 가능한 최대 바이트 수(<= len)
 *
 * seg_boundary_mask(정렬 경계, 예: 4GB 경계)를 넘지 않는 한도와
 * max_segment_size(컨트롤러가 허용하는 세그먼트 최대 길이) 두 가지를
 * 모두 만족하는 값 중 작은 쪽을 택한다. seg_boundary_mask가
 * ULONG_MAX이고 paddr이 0일 때 발생할 수 있는 오버플로를 피하기 위해,
 * -1 계산 뒤에 +1을 min 비교 이후로 미룬다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (세그먼트 계산 hot path).
 * 호출자: bvec_try_merge_hw_page(), 세그먼트 재계산 루틴(blk-merge.c).
 * 피호출자: min_t() (매크로).
 *
 * 호출 체인:
 *   bvec_try_merge_hw_page -> [get_max_segment_size]
 */
static inline unsigned get_max_segment_size(const struct queue_limits *lim,
		phys_addr_t paddr, unsigned int len)
{
	/*
	 * Prevent an overflow if mask = ULONG_MAX and offset = 0 by adding 1
	 * after having calculated the minimum.
	 */
	return min_t(unsigned long, len,
		min(lim->seg_boundary_mask - (lim->seg_boundary_mask & paddr),	/* paddr부터 seg_boundary까지 남은 바이트; NVMe PRP entry가 경계 넘지 않도록 */
		    (unsigned long)lim->max_segment_size - 1) + 1);		/* max_segment_size와 비교 후 +1; NVMe controller의 max segment size 적용 */
}

/*
 * [한국어]
 * ll_back_merge_fn - request 뒤에 bio를 병합할 수 있는지 물리/정책 조건 검사
 *
 * @req: 병합 대상 request
 * @bio: 병합 후보 bio
 * @nr_segs: bio의 세그먼트 수(사전 계산)
 * @return: 1=병합 가능(세그먼트 카운터가 이미 반영됨), 0=병합 불가
 *
 * req와 bio 사이의 물리적 gap(req_gap_back_merge), 세그먼트 수 초과
 * (blk_rq_get_max_segments 한도), cgroup/integrity 정책 불일치 등을
 * 검사한다. NVMe 관점에서 back-merge는 SQ 엔트리 하나에 더 많은 논리
 * 블록을 담아 doorbell 횟수와 명령 오버헤드를 줄이는 효과가 있다. 실패
 * 시 req_set_nomerge()로 REQ_NOMERGE를 설정해 이후 재시도를 막는다.
 * 실제 정의는 block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bio_attempt_back_merge() (blk-merge.c).
 * 피호출자: ll_new_hw_segment(), req_set_nomerge().
 *
 * 호출 체인:
 *   bio_attempt_back_merge -> [ll_back_merge_fn] -> ll_new_hw_segment
 */
int ll_back_merge_fn(struct request *req, struct bio *bio,
		unsigned int nr_segs);
/*
 * [한국어]
 * blk_attempt_req_merge - 두 request(rq, next)를 하나로 병합 시도
 *
 * @q: request_queue
 * @rq: 병합 후 유지될 request (LBA가 낮은 쪽)
 * @next: rq에 흡수되어 소멸될 request
 * @return: true=병합 성공(호출자가 next를 free해야 함), false=실패
 *
 * scheduler(elevator) 내부나 timeout/abort 경로에서, 이미 큐잉된 두
 * request가 병합 가능한 조건(LBA 연속, integrity/crypto 호환 등)이면
 * 하나로 합쳐 NVMe SQ에 들어가는 명령 수를 줄인다. 실제 정의는
 * block/blk-merge.c의 attempt_merge()를 감싼 wrapper다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk-mq-sched.c의 스케줄러 병합 경로.
 * 피호출자: attempt_merge() (blk-merge.c 내부 정적 함수).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge -> [blk_attempt_req_merge] -> attempt_merge
 */
bool blk_attempt_req_merge(struct request_queue *q, struct request *rq,
				struct request *next);
/*
 * [한국어]
 * blk_recalc_rq_segments - request에 포함된 모든 bio로부터 물리 세그먼트 수 재계산
 *
 * @rq: 대상 request
 * @return: 재계산된 물리 세그먼트 수(nr_phys_segments에 대입될 값)
 *
 * 결과값은 NVMe SGL/PRP 리스트를 구성할 때 필요한 entry 개수의 상한이
 * 된다. Discard/Secure Erase/Write Zeroes는 데이터 버퍼가 없으므로 별도
 * 규칙으로 계산한다. 병합이 일어나 request의 bio 리스트가 바뀔 때마다
 * 다시 호출되어야 한다. 실제 정의는 block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: ll_back_merge_fn() 등 병합 성공 후 갱신 경로, blk_rq_map_sg() 이전 단계.
 * 피호출자: biovec_phys_mergeable(), bio 순회 헬퍼.
 *
 * 호출 체인:
 *   ll_back_merge_fn -> [blk_recalc_rq_segments]
 */
unsigned int blk_recalc_rq_segments(struct request *rq);
/*
 * [한국어]
 * blk_rq_merge_ok - request와 bio가 병합 가능한 "기본 조건"을 만족하는지 검사
 *
 * @rq: 대상 request
 * @bio: 병합 후보 bio
 * @return: true=기본 조건 만족(추가로 blk_try_merge()의 위치 판정 필요),
 *          false=애초에 병합 불가
 *
 * op 종류, cgroup, integrity(PI), crypto 컨텍스트, write_hint/stream,
 * ioprio, atomic write 속성이 모두 일치해야 한다. NVMe 컨트롤러는 한
 * 명령 내에서 이러한 속성이 섞이는 것을 허용하지 않으므로(예: PI 활성화
 * 여부, FUA, atomic 영역 경계) 사전에 병합을 차단한다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_try_merge() 이전 단계(예: blk_attempt_bio_merge, blk-merge.c).
 * 피호출자: rq_mergeable(), bio_mergeable(), blk_cgroup_mergeable(),
 *   blk_integrity_merge_bio(), bio_crypt_rq_ctx_compatible().
 *
 * 호출 체인:
 *   blk_attempt_bio_merge -> [blk_rq_merge_ok] -> rq_mergeable / ...
 */
bool blk_rq_merge_ok(struct request *rq, struct bio *bio);
/*
 * [한국어]
 * blk_try_merge - request에 대해 bio가 어떤 종류의 병합에 해당하는지 판별
 *
 * @rq: 기준 request
 * @bio: 판별 대상 bio
 * @return: ELEVATOR_DISCARD_MERGE(range 병합), ELEVATOR_BACK_MERGE(뒤에 이어붙임),
 *          ELEVATOR_FRONT_MERGE(앞에 이어붙임), ELEVATOR_NO_MERGE(불가)
 *
 * ELEVATOR_BACK_MERGE/FRONT_MERGE 판단은 LBA 연속성을 기준으로 하며,
 * NVMe 명령의 Starting LBA(SLBA)와 Length(NLB)를 이어붙일 수 있는지를
 * 사전 검사하는 것과 같다. blk_discard_mergable()이 true이면 LBA 연속성
 * 검사 없이 곧바로 ELEVATOR_DISCARD_MERGE를 반환한다. 실제 정의는
 * block/blk-merge.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_bio_list_merge(), blk-mq-sched.c의 병합 판정 경로.
 * 피호출자: blk_discard_mergable(), blk_rq_pos(), blk_rq_sectors().
 *
 * 호출 체인:
 *   blk_bio_list_merge -> [blk_try_merge] -> blk_discard_mergable
 */
enum elv_merge blk_try_merge(struct request *rq, struct bio *bio);

/*
 * [한국어]
 * blk_set_default_limits - queue_limits 구조체를 커널 기본값으로 초기화
 *
 * @lim: 초기화할 queue_limits (드라이버가 blk_alloc_queue() 전에 준비)
 * @return: 0=성공, 음수=잘못된 limits 조합(errno)
 *
 * max_segments, seg_boundary_mask, max_sectors 등 모든 필드를 커널이
 * 안전하다고 간주하는 기본값으로 채운다. NVMe 드라이버는 이후 컨트롤러
 * Identify 결과(MDTS 등)에 맞게 이 값들을 덮어쓴다. 실제 정의는
 * block/blk-settings.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (드라이버 probe 경로).
 * 호출자: 각 드라이버의 queue_limits 준비 코드, blk_alloc_queue() 내부 경로.
 *
 * 호출 체인:
 *   nvme_alloc_ns / blk_mq_alloc_disk -> [blk_set_default_limits]
 */
int blk_set_default_limits(struct queue_limits *lim);
/*
 * [한국어]
 * blk_apply_bdi_limits - backing_dev_info(BDI)의 특성을 queue_limits에 반영
 *
 * @bdi: 대상 backing_dev_info (읽기/쓰기 ahead 등 VM 힌트 보유)
 * @lim: 반영할 queue_limits
 * @return: 없음
 *
 * io_pages, ra_pages 등 VM/페이지 캐시 readahead 관련 힌트를 queue_limits
 * 기반으로 계산해 bdi에 설정한다. NVMe처럼 빠른 장치는 더 큰 readahead
 * 창을 가질 수 있다. 실제 정의는 block/blk-settings.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 큐 등록/limits 갱신 경로 (blk_register_queue 등).
 *
 * 호출 체인:
 *   blk_register_queue -> [blk_apply_bdi_limits]
 */
void blk_apply_bdi_limits(struct backing_dev_info *bdi,
		struct queue_limits *lim);
/*
 * [한국어]
 * blk_dev_init - 블록 계층 부트 타임 전역 초기화
 *
 * @return: 0 (항상 성공; 내부에서 실패하면 panic)
 *
 * 커널 부트 시 한 번 호출되어, REQ_OP_BITS/REQ_FLAG_BITS 컴파일 타임
 * 검사, kblockd_workqueue 생성, request_queue용 슬랩 캐시 생성, block
 * debugfs 최상위 디렉터리 생성 등을 수행한다. 이 초기화가 끝나야 이후
 * NVMe를 포함한 모든 블록 드라이버의 큐 할당이 가능해진다. 실제 정의는
 * block/blk-core.c에 있다.
 * 실행 컨텍스트: 부트 시 __init 함수 (단일 CPU, 초기화 전용 컨텍스트).
 * 호출자: start_kernel() (subsys_initcall 매커니즘을 통해).
 * 피호출자: alloc_workqueue(), KMEM_CACHE(), debugfs_create_dir().
 *
 * 호출 체인:
 *   start_kernel -> [blk_dev_init]
 */
int blk_dev_init(void);

/*
 * [한국어]
 * update_io_ticks - block_device의 io_ticks(활동 시간) 통계 갱신
 *
 * @part: 통계를 갱신할 block_device (파티션 또는 whole disk)
 * @now: 현재 jiffies 값
 * @end: true=I/O 완료 시점, false=I/O 시작 시점
 *
 * bd_stamp에 CAS(Compare-And-Swap)를 사용해 io_ticks를 원자적으로
 * 갱신한다. I/O가 진행 중(inflight > 0)이거나 완료 시점(end=true)에만
 * ticks를 증가시킨다. 파티션이면 bdev_whole()로 whole disk로 전환해
 * 재귀 없이 goto 루프로 반복 처리한다. /proc/diskstats와
 * /sys/block/<disk>/stat의 io_ticks 필드에 반영되어, NVMe namespace의 "장치가
 * 얼마나 바빴는지" 지표(utilization)로 쓰인다. 실제 정의는
 * block/blk-core.c에 있다.
 * 실행 컨텍스트: part_stat_lock() 보호 하에 호출되는 프로세스/인터럽트 컨텍스트.
 * 호출자: bio 시작/완료 계정 경로(blk_account_io_start/done 등).
 *
 * 호출 체인:
 *   blk_account_io_start/done -> [update_io_ticks]
 */
void update_io_ticks(struct block_device *part, unsigned long now, bool end);

/*
 * [한국어]
 * req_set_nomerge - request를 앞으로 병합 대상이 될 수 없도록 표시
 *
 * @q: request가 속한 request_queue (last_merge 캐시 무효화에 사용)
 * @req: 병합 불가로 표시할 request
 * @return: 없음
 *
 * ll_back_merge_fn() 등이 세그먼트 초과·정책 불일치로 병합에 실패했을 때
 * 호출되어 REQ_NOMERGE 플래그를 설정한다. 이 request가 NVMe 드라이버에
 * 의해 이미 PRP/SGL을 확정했거나(RQF_STARTED) 특수 명령(FLUSH 등)인
 * 경우에도 동일한 효과를 낸다. q->last_merge가 이 request를 가리키고
 * 있었다면 함께 초기화해, 다음 bio가 이 실패한 request를 다시 시도하지
 * 않고 새로운 후보를 탐색하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (병합 실패 처리 경로).
 * 호출자: ll_new_hw_segment() (blk-merge.c) 등 병합 실패 경로.
 * 피호출자: 없음 (플래그 대입만 수행).
 *
 * 호출 체인:
 *   ll_back_merge_fn -> ll_new_hw_segment -> [req_set_nomerge]
 */
static inline void req_set_nomerge(struct request_queue *q, struct request *req)
{
	req->cmd_flags |= REQ_NOMERGE;					/* NVMe driver가 PRP/SGL을 확정했거나 low-latency 모드; 추가 병합 금지 */
	if (req == q->last_merge)						/* scheduler의 병합 캐시가 이 rq를 가리키면 무효화; 다음 bio는 새로운 NVMe rq 탐색 */
		q->last_merge = NULL;
}

/*
 * Internal io_context interface
 */
/*
 * [한국어]
 * ioc_find_get_icq - 현재 태스크의 io_context에서 이 큐 전용 icq를 찾거나 새로 생성
 *
 * @q: 대상 request_queue
 * @return: 이 (io_context, request_queue) 쌍에 대응하는 io_cq(icq) 포인터, 실패 시 NULL
 *
 * io_cq(I/O Context Queue)는 CFQ/BFQ 등 cgroup-aware 스케줄러가 프로세스별
 * I/O 우선순위·상태를 큐 단위로 캐싱하는 자료구조다. 없으면 새로 할당해
 * 태스크의 io_context와 request_queue 양쪽 리스트에 연결한다. NVMe
 * 자체는 icq를 사용하지 않지만, BFQ 스케줄러를 얹은 NVMe 큐에서는 사용될
 * 수 있다. 실제 정의는 block/blk-ioc.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 스케줄러(BFQ 등)의 request 할당 경로.
 *
 * 호출 체인:
 *   bfq_get_rq_private -> [ioc_find_get_icq]
 */
struct io_cq *ioc_find_get_icq(struct request_queue *q);
/*
 * [한국어]
 * ioc_lookup_icq - 현재 태스크의 io_context에서 이 큐 전용 icq를 조회(생성하지 않음)
 *
 * @q: 대상 request_queue
 * @return: 이미 존재하는 io_cq 포인터, 없으면 NULL
 *
 * ioc_find_get_icq()와 달리 없으면 새로 만들지 않고 조회만 한다. 이미
 * 할당되어 있어야 하는 경로(예: 완료 처리)에서 사용된다. 실제 정의는
 * block/blk-ioc.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 스케줄러 콜백 컨텍스트.
 *
 * 호출 체인:
 *   (스케줄러 완료/병합 경로) -> [ioc_lookup_icq]
 */
struct io_cq *ioc_lookup_icq(struct request_queue *q);
#ifdef CONFIG_BLK_ICQ
/*
 * [한국어]
 * ioc_clear_queue - request_queue가 소멸할 때 연결된 모든 icq를 정리
 *
 * @q: 소멸 중인 request_queue
 * @return: 없음
 *
 * 이 큐에 연결된 모든 io_context의 icq를 찾아 연결을 끊고 필요하면
 * 해제한다. 큐가 사라진 뒤에도 다른 태스크의 io_context가 죽은 큐를
 * 가리키는 댕글링 포인터를 갖지 않도록 보장한다. 실제 정의는
 * block/blk-ioc.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (큐 해제 경로).
 * 호출자: blk_release_queue() 또는 큐 소멸 경로.
 *
 * 호출 체인:
 *   blk_release_queue -> [ioc_clear_queue]
 */
void ioc_clear_queue(struct request_queue *q);
#else
/*
 * [한국어]
 * ioc_clear_queue - CONFIG_BLK_ICQ 비활성 시 스텁 (icq 인프라 자체가 없음)
 *
 * @q: 사용되지 않음
 * @return: 없음
 */
static inline void ioc_clear_queue(struct request_queue *q)
{
}
#endif /* CONFIG_BLK_ICQ */

#ifdef CONFIG_BLK_DEV_ZONED
/*
 * Zoned NVMe(ZNS) 관련 함수들 - zone append/reset/open/close/finish
 * 관리. NVMe ZNS SSD에서는 write pointer 순서와 zone 상태가 중요하며,
 * ZONE_APPEND 명령은 컨트롤러가 실제 LBA를 CQ entry에 기록해 반환한다.
 */
/*
 * [한국어]
 * disk_init_zone_resources - gendisk의 zone 관련 자료구조(zone bitmap 등) 초기화
 *
 * @disk: 대상 gendisk (NVMe ZNS namespace)
 * @return: 없음
 *
 * conv_zones_bitmap(conventional zone 여부), seq_zones_wlock(zone write
 * lock) 등 zone 상태 추적용 자료구조를 disk_scan_partitions() 등에서 얻은
 * zone report를 기반으로 할당·초기화한다. 실제 정의는 block/blk-zoned.c.
 * 실행 컨텍스트: 프로세스 컨텍스트 (디스크 등록/재검사 경로).
 * 호출자: add_disk(), disk_update_zone_resources() 계열.
 *
 * 호출 체인:
 *   add_disk / revalidate -> [disk_init_zone_resources]
 */
void disk_init_zone_resources(struct gendisk *disk);
/*
 * [한국어]
 * disk_free_zone_resources - disk_init_zone_resources()로 할당한 자료구조 해제
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * gendisk가 소멸하거나 zone 구성이 바뀌어 재초기화가 필요할 때 호출된다.
 * 실제 정의는 block/blk-zoned.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 디스크 해제 경로, revalidate 재초기화 경로.
 *
 * 호출 체인:
 *   del_gendisk / revalidate -> [disk_free_zone_resources]
 */
void disk_free_zone_resources(struct gendisk *disk);
/*
 * [한국어]
 * bio_zone_write_plugging - bio가 zone write plug에 의해 순서 제어되고 있는지 검사
 *
 * @bio: 검사 대상 bio
 * @return: true=이 bio가 zone write plug를 통해 순차화되는 중, false=아님
 *
 * NVMe ZNS는 zone(구역) 내에서 반드시 순차 쓰기만 허용하므로, 블록
 * 계층은 동일 zone에 대한 여러 write bio를 zone write plug라는 내부
 * 큐로 직렬화한다. BIO_ZONE_WRITE_PLUGGING 플래그로 이 상태를 표시한다.
 * 실행 컨텍스트: 임의 컨텍스트 (bio 플래그 읽기만 수행).
 * 호출자: blk_zone_bio_endio() (본 파일).
 * 피호출자: bio_flagged() (인라인 헬퍼).
 *
 * 호출 체인:
 *   blk_zone_bio_endio -> [bio_zone_write_plugging]
 */
static inline bool bio_zone_write_plugging(struct bio *bio)
{
	return bio_flagged(bio, BIO_ZONE_WRITE_PLUGGING);		/* ZNS zone write plug 활성; NVMe ZONE_APPEND 순차화를 위해 bio 대기 */
}
/*
 * [한국어]
 * blk_req_bio_is_zone_append - request/bio가 (실제 또는 에뮬레이션된) Zone Append인지 검사
 *
 * @rq: 대상 request
 * @bio: 대상 bio (에뮬레이션 플래그 확인용)
 * @return: true=Zone Append 의미론을 가짐, false=일반 요청
 *
 * req_op(rq)가 REQ_OP_ZONE_APPEND(NVMe ZONE_APPEND opcode 0x7D)이거나,
 * BIO_EMULATES_ZONE_APPEND 플래그가 설정된 경우(컨트롤러가 네이티브
 * Zone Append를 지원하지 않아 소프트웨어로 흉내내는 zoned 장치) true를
 * 반환한다.
 * 실행 컨텍스트: 임의 컨텍스트.
 * 호출자: zone 관련 완료/병합 처리 경로(blk-zoned.c).
 * 피호출자: req_op(), bio_flagged().
 *
 * 호출 체인:
 *   (zone 완료 처리 경로) -> [blk_req_bio_is_zone_append]
 */
static inline bool blk_req_bio_is_zone_append(struct request *rq,
					      struct bio *bio)
{
	return req_op(rq) == REQ_OP_ZONE_APPEND ||			/* NVMe ZONE_APPEND opcode(0x7D) */
	       bio_flagged(bio, BIO_EMULATES_ZONE_APPEND);		/* 소프트웨어로 ZONE_APPEND를 흉낸 경우; NVMe ZNS가 아닌 zoned 장치 */
}
/*
 * [한국어]
 * blk_zone_write_plug_bio_merged - bio가 병합될 때 zone write plug 상태를 갱신
 *
 * @bio: 병합된 bio
 * @return: 없음
 *
 * 병합으로 인해 zone write plug가 추적하는 "다음 예상 쓰기 위치" 등의
 * 상태가 영향을 받으므로, 병합 시점에 이를 갱신해 순서 보장이 깨지지
 * 않게 한다. 실제 정의는 block/blk-zoned.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (병합 경로).
 * 호출자: bio 병합 성공 경로(blk-merge.c)에서 zone write bio인 경우.
 *
 * 호출 체인:
 *   bio_attempt_back_merge (zone write plugging bio) -> [blk_zone_write_plug_bio_merged]
 */
void blk_zone_write_plug_bio_merged(struct bio *bio);
/*
 * [한국어]
 * blk_zone_write_plug_init_request - Zone Append/Write request 초기화 시 plug 연결
 *
 * @rq: 초기화 중인 request
 * @return: 없음
 *
 * request가 zone write plug를 통해 순차 처리되어야 함을 표시하고
 * (RQF_ZONE_WRITE_PLUGGING) 연관 자료구조를 연결한다. 실제 정의는
 * block/blk-zoned.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (request 할당 경로).
 * 호출자: blk_mq_get_request() 계열, zone write bio 처리 경로.
 *
 * 호출 체인:
 *   blk_mq_get_request -> [blk_zone_write_plug_init_request]
 */
void blk_zone_write_plug_init_request(struct request *rq);
/*
 * [한국어]
 * blk_zone_append_update_request_bio - Zone Append 완료 후 실제 쓰기 위치를 bio에 반영
 *
 * @rq: 완료된 Zone Append request
 * @bio: 결과를 반영할 bio
 * @return: 없음
 *
 * NVMe ZONE_APPEND 명령은 실제로 기록된 LBA를 CQE(완료 큐 엔트리)에
 * 담아 반환하므로, 이 함수가 그 값을 bio->bi_iter.bi_sector 등에 다시
 * 써서 상위 호출자(파일시스템 등)가 실제 기록 위치를 알 수 있게 한다.
 * 실제 정의는 block/blk-zoned.c에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 컨텍스트.
 * 호출자: blk_zone_bio_endio() 관련 완료 경로(간접) 또는 blk_mq_end_request().
 *
 * 호출 체인:
 *   nvme_complete_rq -> ... -> [blk_zone_append_update_request_bio]
 */
void blk_zone_append_update_request_bio(struct request *rq, struct bio *bio);
/*
 * [한국어]
 * blk_zone_mgmt_bio_endio - Zone 관리(reset/open/close/finish) bio 완료 처리
 *
 * @bio: 완료된 zone 관리 bio (REQ_OP_ZONE_RESET 등)
 * @return: 없음
 *
 * zone reset은 해당 zone의 write pointer를 0으로 되돌리므로, 그 zone에
 * 연결된 zone write plug의 오프셋 상태도 함께 갱신해야 한다. 이 함수가
 * 그 갱신을 수행한다. 실제 정의는 block/blk-zoned.c에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 경로.
 * 호출자: blk_zone_bio_endio() (본 파일, op_is_zone_mgmt인 경우).
 *
 * 호출 체인:
 *   bio_endio -> blk_zone_bio_endio -> [blk_zone_mgmt_bio_endio]
 */
void blk_zone_mgmt_bio_endio(struct bio *bio);
/*
 * [한국어]
 * blk_zone_write_plug_bio_endio - zone write plug가 관리하던 write bio의 완료 처리
 *
 * @bio: 완료된 write bio
 * @return: 없음
 *
 * 이 bio의 완료를 신호로 삼아, 같은 zone에 대기 중이던 다음 write bio를
 * zone write plug가 순차적으로 제출할 수 있게 한다. NVMe ZNS의 순차
 * 쓰기 제약을 만족시키는 핵심 지점이다. 실제 정의는 block/blk-zoned.c.
 * 실행 컨텍스트: NVMe CQ 완료 경로.
 * 호출자: blk_zone_bio_endio() (본 파일, bio_zone_write_plugging()인 경우).
 *
 * 호출 체인:
 *   bio_endio -> blk_zone_bio_endio -> [blk_zone_write_plug_bio_endio]
 *   -> (다음 대기 bio 제출)
 */
void blk_zone_write_plug_bio_endio(struct bio *bio);
/*
 * [한국어]
 * blk_zone_bio_endio - bio 완료 시 zone 관련 후처리가 필요한지 판별해 위임
 *
 * @bio: 완료된 bio
 * @return: 없음
 *
 * zone 관리 명령(reset/open/close/finish)이면 blk_zone_mgmt_bio_endio(),
 * zone write plugging 대상 쓰기 bio이면 blk_zone_write_plug_bio_endio()로
 * 위임한다. 두 경로 모두 zone write plug가 관리하지 않는 zone에서도
 * 발생할 수 있는 zone 상태 변화(예: reset으로 인한 write pointer 리셋)를
 * 반영하기 위함이다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 컨텍스트 (bio_endio 내부).
 * 호출자: bio_endio() (bio.c).
 * 피호출자: op_is_zone_mgmt(), blk_zone_mgmt_bio_endio(),
 *   bio_zone_write_plugging(), blk_zone_write_plug_bio_endio().
 *
 * 호출 체인:
 *   bio_endio -> [blk_zone_bio_endio] -> blk_zone_mgmt_bio_endio /
 *   blk_zone_write_plug_bio_endio
 */
static inline void blk_zone_bio_endio(struct bio *bio)
{
	/*
	 * Zone management BIOs may impact zone write plugs (e.g. a zone reset
	 * changes a zone write plug zone write pointer offset), but these
	 * operation do not go through zone write plugging as they may operate
	 * on zones that do not have a zone write
	 * plug. blk_zone_mgmt_bio_endio() handles the potential changes to zone
	 * write plugs that are present.
	 */
	if (op_is_zone_mgmt(bio_op(bio))) {
		blk_zone_mgmt_bio_endio(bio);				/* NVMe ZNS zone reset/open/close/finish 완료 후 plug 상태 갱신 */
		return;
	}

	/*
	 * For write BIOs to zoned devices, signal the completion of the BIO so
	 * that the next write BIO can be submitted by zone write plugging.
	 */
	if (bio_zone_write_plugging(bio))
		blk_zone_write_plug_bio_endio(bio);			/* NVMe ZONE_APPEND 완료 후 다음 bio가 순차적으로 진행되도록 signal */
}

/*
 * [한국어]
 * blk_zone_write_plug_finish_request - request 종료 시 zone write plug 자원 해제
 *
 * @rq: 종료되는 request
 * @return: 없음
 *
 * zone write plug와의 연결을 끊고, write pointer 진행 상태를 반영한 뒤
 * 다음 대기 중인 bio가 있으면 진행할 수 있게 한다. 실제 정의는
 * block/blk-zoned.c에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 경로 (request 종료 처리 중).
 * 호출자: blk_zone_finish_request() (본 파일, 인라인 래퍼).
 *
 * 호출 체인:
 *   blk_mq_end_request -> blk_zone_finish_request -> [blk_zone_write_plug_finish_request]
 */
void blk_zone_write_plug_finish_request(struct request *rq);
/*
 * [한국어]
 * blk_zone_finish_request - request 종료 시 zone write plug 처리가 필요한지 판별
 *
 * @rq: 종료되는 request
 * @return: 없음
 *
 * RQF_ZONE_WRITE_PLUGGING 플래그가 설정된 request(zone write plug를 통해
 * 순차화되던 요청)에 대해서만 blk_zone_write_plug_finish_request()를
 * 호출한다. 그렇지 않은 일반 request는 아무 동작도 하지 않는다.
 * 실행 컨텍스트: NVMe CQ 완료 경로 (request 종료 처리 중).
 * 호출자: blk_mq_end_request() 등 request 완료 처리 경로(blk-mq.c).
 * 피호출자: blk_zone_write_plug_finish_request().
 *
 * 호출 체인:
 *   blk_mq_end_request -> [blk_zone_finish_request] -> blk_zone_write_plug_finish_request
 */
static inline void blk_zone_finish_request(struct request *rq)
{
	if (rq->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		blk_zone_write_plug_finish_request(rq);			/* NVMe ZONE_APPEND rq 종료 시 zone plug 해제; write pointer advance 반영 */
}
/*
 * [한국어]
 * blkdev_report_zones_ioctl - BLKREPORTZONE ioctl 구현: zone 상태 보고
 *
 * @bdev: 대상 block_device (NVMe ZNS namespace)
 * @cmd: ioctl 명령 번호(BLKREPORTZONE)
 * @arg: 사용자공간 struct blk_zone_report * (in/out)
 * @return: 0=성공, 음수=errno
 *
 * NVMe Zone Management Receive(opcode) 등을 이용해 zone들의 현재 상태
 * (write pointer, 상태: EMPTY/OPEN/FULL 등)를 사용자공간에 보고한다.
 * 실제 정의는 block/blk-zoned.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl 시스템 호출 경로).
 * 호출자: blkdev_ioctl() (ioctl.c).
 *
 * 호출 체인:
 *   blkdev_ioctl -> [blkdev_report_zones_ioctl]
 */
int blkdev_report_zones_ioctl(struct block_device *bdev, unsigned int cmd,
		unsigned long arg);
/*
 * [한국어]
 * blkdev_zone_mgmt_ioctl - BLKRESETZONE/BLKOPENZONE 등 zone 관리 ioctl 구현
 *
 * @bdev: 대상 block_device
 * @mode: 파일 open mode (쓰기 권한 검사 등에 사용)
 * @cmd: ioctl 명령 번호(reset/open/close/finish 등)
 * @arg: 사용자공간 인자(zone 범위 등)
 * @return: 0=성공, 음수=errno
 *
 * REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH bio를 생성해 NVMe ZNS 컨트롤러에
 * Zone Management Send 명령으로 변환·제출한다. 실제 정의는
 * block/blk-zoned.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl() (ioctl.c).
 *
 * 호출 체인:
 *   blkdev_ioctl -> [blkdev_zone_mgmt_ioctl]
 */
int blkdev_zone_mgmt_ioctl(struct block_device *bdev, blk_mode_t mode,
		unsigned int cmd, unsigned long arg);
#else /* CONFIG_BLK_DEV_ZONED */
/*
 * [한국어]
 * disk_init_zone_resources - CONFIG_BLK_DEV_ZONED 비활성 시 스텁 (아무 동작 없음)
 * @disk: 사용되지 않음 / @return: 없음
 */
static inline void disk_init_zone_resources(struct gendisk *disk)
{
}
/*
 * [한국어]
 * disk_free_zone_resources - CONFIG_BLK_DEV_ZONED 비활성 시 스텁 (아무 동작 없음)
 * @disk: 사용되지 않음 / @return: 없음
 */
static inline void disk_free_zone_resources(struct gendisk *disk)
{
}
/*
 * [한국어]
 * bio_zone_write_plugging - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @bio: 사용되지 않음 / @return: 항상 false
 */
static inline bool bio_zone_write_plugging(struct bio *bio)
{
	return false;								/* ZNS 비활성; NVMe ZONE_APPEND 경로 미사용 */
}
/*
 * [한국어]
 * blk_req_bio_is_zone_append - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @req: 사용되지 않음 / @bio: 사용되지 않음 / @return: 항상 false
 */
static inline bool blk_req_bio_is_zone_append(struct request *req,
					      struct bio *bio)
{
	return false;								/* 일반 NVMe namespace; ZONE_APPEND 처리 없음 */
}
/*
 * [한국어]
 * blk_zone_write_plug_bio_merged - CONFIG_BLK_DEV_ZONED 비활성 시 스텁 (아무 동작 없음)
 * @bio: 사용되지 않음 / @return: 없음
 */
static inline void blk_zone_write_plug_bio_merged(struct bio *bio)
{
}
/*
 * [한국어]
 * blk_zone_write_plug_init_request - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @rq: 사용되지 않음 / @return: 없음
 */
static inline void blk_zone_write_plug_init_request(struct request *rq)
{
}
/*
 * [한국어]
 * blk_zone_append_update_request_bio - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @rq: 사용되지 않음 / @bio: 사용되지 않음 / @return: 없음
 */
static inline void blk_zone_append_update_request_bio(struct request *rq,
						      struct bio *bio)
{
}
/*
 * [한국어]
 * blk_zone_bio_endio - CONFIG_BLK_DEV_ZONED 비활성 시 스텁 (아무 동작 없음)
 * @bio: 사용되지 않음 / @return: 없음
 */
static inline void blk_zone_bio_endio(struct bio *bio)
{
}
/*
 * [한국어]
 * blk_zone_finish_request - CONFIG_BLK_DEV_ZONED 비활성 시 스텁 (아무 동작 없음)
 * @rq: 사용되지 않음 / @return: 없음
 */
static inline void blk_zone_finish_request(struct request *rq)
{
}
/*
 * [한국어]
 * blkdev_report_zones_ioctl - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @bdev: 사용되지 않음 / @cmd: 사용되지 않음 / @arg: 사용되지 않음
 * @return: 항상 -ENOTTY (해당 ioctl을 지원하지 않는 장치임을 알림)
 */
static inline int blkdev_report_zones_ioctl(struct block_device *bdev,
		unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;								/* ZNS ioctl 미지원; NVMe namespace가 ZNS가 아님 */
}
/*
 * [한국어]
 * blkdev_zone_mgmt_ioctl - CONFIG_BLK_DEV_ZONED 비활성 시 스텁
 * @bdev: 사용되지 않음 / @mode: 사용되지 않음 / @cmd: 사용되지 않음 / @arg: 사용되지 않음
 * @return: 항상 -ENOTTY
 */
static inline int blkdev_zone_mgmt_ioctl(struct block_device *bdev,
		blk_mode_t mode, unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;								/* ZNS management ioctl 불가 */
}
#endif /* CONFIG_BLK_DEV_ZONED */

/*
 * [한국어]
 * bdev_alloc - gendisk에 속하는 새 block_device(whole disk 또는 파티션) 할당
 *
 * @disk: 상위 gendisk (NVMe namespace에 대응)
 * @partno: 파티션 번호 (0이면 whole disk 자신을 의미)
 * @return: 할당된 block_device 포인터, 실패 시 NULL
 *
 * bdev_inode를 포함한 struct block_device를 슬랩에서 할당하고 기본 필드를
 * 초기화한다. 실제 정의는 block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (디스크/파티션 등록 경로).
 * 호출자: __alloc_disk_node(), bdev_add_partition() (partitions/core.c).
 *
 * 호출 체인:
 *   add_disk / bdev_add_partition -> [bdev_alloc]
 */
struct block_device *bdev_alloc(struct gendisk *disk, u8 partno);
/*
 * [한국어]
 * bdev_add - 할당된 block_device를 시스템에 등록(해시 테이블/sysfs 연결)
 *
 * @bdev: 등록할 block_device (bdev_alloc()으로 미리 할당됨)
 * @dev: 부여할 dev_t (major/minor 번호)
 * @return: 없음
 *
 * bd_dev를 설정하고 bdev를 전역 해시(lookup용)에 넣어, 이후
 * blkdev_get_by_dev() 등으로 조회 가능하게 만든다. 실제 정의는 block/bdev.c.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk(), bdev_add_partition().
 *
 * 호출 체인:
 *   add_disk -> bdev_alloc -> [bdev_add]
 */
void bdev_add(struct block_device *bdev, dev_t dev);
/*
 * [한국어]
 * bdev_unhash - block_device를 전역 조회 해시에서 제거
 *
 * @bdev: 제거할 block_device
 * @return: 없음
 *
 * 디스크/파티션 제거 시 더 이상 새로운 open 요청이 이 bdev를 찾지
 * 못하도록 해시에서 뺀다(기존 참조자는 여전히 유효). 실제 정의는
 * block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: del_gendisk(), drop_partition() 경로.
 *
 * 호출 체인:
 *   del_gendisk -> [bdev_unhash]
 */
void bdev_unhash(struct block_device *bdev);
/*
 * [한국어]
 * bdev_drop - 마지막 참조가 끝난 block_device를 실제로 해제
 *
 * @bdev: 해제할 block_device
 * @return: 없음
 *
 * bdev_unhash()로 조회 불가 상태가 된 뒤, 남은 참조가 모두 반납되면
 * 호출되어 inode/구조체 메모리를 반환한다. 실제 정의는 block/bdev.c.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 마지막 bdev_put()/iput() 경로.
 *
 * 호출 체인:
 *   (마지막 참조 반납) -> [bdev_drop]
 */
void bdev_drop(struct block_device *bdev);

/*
 * [한국어]
 * blk_alloc_ext_minor - 확장(extended) 마이너 번호 공간에서 새 마이너 번호 할당
 *
 * @return: 할당된 마이너 번호(>= 0), 실패 시 음수 errno
 *
 * 전통적인 마이너 번호 공간(디스크당 고정 개수의 파티션 마이너)이
 * 부족한 NVMe처럼 파티션 수가 가변적인 장치를 위해, IDA(ID Allocator)
 * 기반의 확장 마이너 공간에서 번호를 발급한다. 실제 정의는 block/genhd.c.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: __alloc_disk_node() 등 gendisk 생성 경로.
 *
 * 호출 체인:
 *   __alloc_disk_node -> [blk_alloc_ext_minor]
 */
int blk_alloc_ext_minor(void);
/*
 * [한국어]
 * blk_free_ext_minor - blk_alloc_ext_minor()로 받은 마이너 번호 반납
 *
 * @minor: 반납할 마이너 번호
 * @return: 없음
 *
 * IDA에 번호를 반환해 재사용 가능하게 한다. 실제 정의는 block/genhd.c.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 디스크 해제 경로.
 *
 * 호출 체인:
 *   (디스크 해제 경로) -> [blk_free_ext_minor]
 */
void blk_free_ext_minor(unsigned int minor);
/* [한국어] ADDPART_FLAG_NONE — bdev_add_partition() 플래그 기본값(특별한 처리 없음). */
#define ADDPART_FLAG_NONE	0
/* [한국어] ADDPART_FLAG_RAID — 이 파티션이 md/RAID 멤버로 사용됨을 표시.
 * 왜 필요한가: RAID 멤버 파티션은 파일시스템이 직접 마운트하지 않도록 사용자공간
 * (mdadm 등)에 정보를 전달할 때 사용된다. */
#define ADDPART_FLAG_RAID	1
/* [한국어] ADDPART_FLAG_WHOLEDISK — 파티션 테이블 없이 whole-disk 자체를 등록할 때 사용.
 * 왜 필요한가: 일부 경로(loop, dm)에서 파티션 없는 디스크 전체를 "파티션 1개"로
 * 취급해 등록하는 특수 케이스를 구분하기 위함. */
#define ADDPART_FLAG_WHOLEDISK	2
/* [한국어] ADDPART_FLAG_READONLY — 파티션을 읽기 전용으로 등록.
 * 왜 필요한가: 일부 특수 파티션(예: 벤더 예약 영역)을 사용자공간의 실수로부터
 * 보호하기 위해 커널이 강제로 읽기 전용 플래그를 부여할 때 사용. */
#define ADDPART_FLAG_READONLY	4
/*
 * [한국어]
 * bdev_add_partition - 새 파티션을 gendisk에 추가(사용자공간 ioctl/스캔 경로)
 *
 * @disk: 상위 gendisk
 * @partno: 새 파티션 번호
 * @start: 시작 섹터
 * @length: 길이(섹터)
 * @return: 0=성공, 음수=errno
 *
 * bdev_alloc()으로 block_device를 만들고 bdev_add()로 등록한 뒤, 크기/
 * 정렬 등을 검증한다. 실제 정의는 block/partitions/core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (BLKPG ioctl 또는 자동 스캔 경로).
 * 호출자: blkpg_do_ioctl(), disk_scan_partitions().
 *
 * 호출 체인:
 *   blkpg_ioctl -> [bdev_add_partition]
 */
int bdev_add_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length);
/*
 * [한국어]
 * bdev_del_partition - 기존 파티션 제거
 *
 * @disk: 상위 gendisk
 * @partno: 제거할 파티션 번호
 * @return: 0=성공, 음수=errno(예: 파티션이 열려 있어 제거 불가)
 *
 * 실제 정의는 block/partitions/core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkpg_do_ioctl().
 *
 * 호출 체인:
 *   blkpg_ioctl -> [bdev_del_partition]
 */
int bdev_del_partition(struct gendisk *disk, int partno);
/*
 * [한국어]
 * bdev_resize_partition - 기존 파티션의 시작/길이를 변경
 *
 * @disk: 상위 gendisk
 * @partno: 대상 파티션 번호
 * @start: 새 시작 섹터
 * @length: 새 길이(섹터)
 * @return: 0=성공, 음수=errno
 *
 * 실제 정의는 block/partitions/core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkpg_do_ioctl().
 *
 * 호출 체인:
 *   blkpg_ioctl -> [bdev_resize_partition]
 */
int bdev_resize_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length);
/*
 * [한국어]
 * drop_partition - 파티션 block_device를 조회 불가 상태로 만들고 참조 반납
 *
 * @part: 제거할 파티션 block_device
 * @return: 없음
 *
 * bdev_unhash() + bdev_put() 조합으로, 디스크 제거나 파티션 재스캔 시
 * 사용된다. 실제 정의는 block/partitions/core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: del_gendisk(), disk_scan_partitions() (재스캔 시 기존 파티션 정리).
 *
 * 호출 체인:
 *   del_gendisk -> [drop_partition]
 */
void drop_partition(struct block_device *part);

/*
 * [한국어]
 * bdev_set_nr_sectors - block_device의 크기(섹터 수)를 설정
 *
 * @bdev: 대상 block_device
 * @sectors: 새 크기(섹터 단위)
 * @return: 없음
 *
 * NVMe namespace 크기 변경(namespace resize) 등에 대응해 bd_nr_sectors를
 * 갱신한다. 실제 정의는 block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: set_capacity() 계열, revalidate 경로.
 *
 * 호출 체인:
 *   set_capacity -> [bdev_set_nr_sectors]
 */
void bdev_set_nr_sectors(struct block_device *bdev, sector_t sectors);

/*
 * [한국어]
 * __alloc_disk_node - gendisk 구조체를 NUMA 노드 지정하여 할당
 *
 * @q: 이 디스크가 연결될 request_queue
 * @node_id: NUMA 노드 ID
 * @lkclass: lockdep용 lock_class_key (bd_size_lock 등에 사용)
 * @return: 할당된 gendisk 포인터, 실패 시 NULL
 *
 * gendisk와 그 안의 whole-disk block_device(bdev_alloc), part_tbl 등을
 * 함께 초기화한다. 실제 정의는 block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: __blk_mq_alloc_disk(), blk_alloc_disk() 계열(NVMe가 사용).
 * 피호출자: bdev_alloc(), blk_alloc_ext_minor().
 *
 * 호출 체인:
 *   nvme_alloc_ns -> blk_mq_alloc_disk -> [__alloc_disk_node]
 */
struct gendisk *__alloc_disk_node(struct request_queue *q, int node_id,
		struct lock_class_key *lkclass);
/*
 * [한국어]
 * blk_alloc_queue - 새로운 request_queue를 슬랩에서 할당해 완전히 초기화
 *
 * @lim: 드라이버가 준비한 queue limits (최대 전송 크기, segment 수 등)
 * @node_id: NUMA 노드 ID (컨트롤러가 속한 노드)
 * @return: 초기화된 request_queue 포인터, 실패 시 ERR_PTR(-errno)
 *
 * NVMe 드라이버가 blk_mq_init_queue() 경로로 호출하며, 이후 NVMe SQ/CQ/
 * tagset이 연결되는 블록 계층의 핸들이 된다. 슬랩 할당 -> IDA ID 발급 ->
 * 통계 구조체 -> limits 복사 -> 타이머/워크 초기화 -> percpu_ref 초기화
 * 순으로 진행되며, 실패 시 역순으로 자원을 해제한다. 실제 정의는
 * block/blk-core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (드라이버 probe/init 경로).
 * 호출자: nvme_alloc_ns(), blk_mq_init_queue() 경로.
 * 피호출자: percpu_ref_init(), blk_set_default_limits().
 *
 * 호출 체인:
 *   nvme_alloc_ns / blk_mq_init_queue -> [blk_alloc_queue]
 */
struct request_queue *blk_alloc_queue(struct queue_limits *lim, int node_id);

/*
 * [한국어]
 * disk_scan_partitions - 디스크의 파티션 테이블을 스캔해 파티션들을 등록
 *
 * @disk: 대상 gendisk
 * @mode: open mode (읽기 전용 여부 등)
 * @return: 0=성공, 음수=errno
 *
 * MSDOS/GPT 등 파티션 테이블 포맷을 인식해 bdev_add_partition()으로 각
 * 파티션을 등록한다. NVMe namespace가 처음 add_disk()될 때나 사용자가
 * BLKRRPART ioctl로 재스캔을 요청할 때 호출된다. 실제 정의는
 * block/genhd.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk(), blkdev_ioctl()의 BLKRRPART 처리.
 * 피호출자: bdev_add_partition(), drop_partition() (재스캔 시 기존 정리).
 *
 * 호출 체인:
 *   add_disk -> [disk_scan_partitions]
 */
int disk_scan_partitions(struct gendisk *disk, blk_mode_t mode);

/*
 * [한국어]
 * disk_alloc_events - 디스크의 media change 이벤트(event) 추적 자료구조 할당
 *
 * @disk: 대상 gendisk
 * @return: 0=성공, 음수=errno
 *
 * disk->fops->check_events가 정의된 장치(광학 드라이브 등)를 위해 이벤트
 * polling 자료구조를 준비한다. NVMe에는 보통 해당하지 않지만 공통
 * gendisk 초기화 경로의 일부다. 실제 정의는 block/disk-events.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk() 계열.
 *
 * 호출 체인:
 *   add_disk -> [disk_alloc_events]
 */
int disk_alloc_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_add_events - 이벤트 polling을 시작하고 sysfs 속성을 등록
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * disk_alloc_events()로 준비된 자료구조를 실제로 활성화한다. 실제 정의는
 * block/disk-events.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk().
 *
 * 호출 체인:
 *   add_disk -> disk_alloc_events -> [disk_add_events]
 */
void disk_add_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_del_events - 이벤트 polling 중지 및 sysfs 속성 제거
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * 실제 정의는 block/disk-events.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: del_gendisk().
 *
 * 호출 체인:
 *   del_gendisk -> [disk_del_events]
 */
void disk_del_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_release_events - disk_alloc_events()로 할당한 자료구조 최종 해제
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * 실제 정의는 block/disk-events.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: gendisk 해제 경로(disk_release()).
 *
 * 호출 체인:
 *   disk_release -> [disk_release_events]
 */
void disk_release_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_block_events - 이벤트 polling을 일시 차단(block)
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * open/close 등 이벤트 상태가 일관되어야 하는 임계 구간 동안 polling
 * 워크가 끼어들지 않도록 막는다. 실제 정의는 block/disk-events.c.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bdev_open() 등.
 *
 * 호출 체인:
 *   bdev_open -> [disk_block_events]
 */
void disk_block_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_unblock_events - disk_block_events()로 차단한 이벤트 polling 재개
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * 실제 정의는 block/disk-events.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: disk_block_events()와 짝을 이루는 정리 경로.
 *
 * 호출 체인:
 *   (임계 구간 종료) -> [disk_unblock_events]
 */
void disk_unblock_events(struct gendisk *disk);
/*
 * [한국어]
 * disk_flush_events - 지정한 이벤트 마스크에 대해 즉시 재확인을 예약
 *
 * @disk: 대상 gendisk
 * @mask: 즉시 재확인할 이벤트 종류 비트마스크
 * @return: 없음
 *
 * 예: media eject 버튼 인터럽트 등 비동기 신호를 받았을 때, 다음 polling
 * 주기를 기다리지 않고 즉시 check_events를 재실행하도록 워크를 앞당긴다.
 * 실제 정의는 block/disk-events.c에 있다.
 * 실행 컨텍스트: 임의 컨텍스트 (인터럽트 핸들러에서도 호출 가능).
 * 호출자: 드라이버의 media change 알림 경로.
 *
 * 호출 체인:
 *   (드라이버 이벤트 알림) -> [disk_flush_events]
 */
void disk_flush_events(struct gendisk *disk, unsigned int mask);
/* [한국어] dev_attr_events — sysfs "events" 속성(현재 보류 중인 이벤트 목록 문자열).
 * genhd.c의 disk_events_show()를 show 콜백으로 사용. */
extern struct device_attribute dev_attr_events;
/* [한국어] dev_attr_events_async — sysfs "events_async" 속성(레거시, 항상 빈 값 반환).
 * 하위 호환을 위해 유지되는 사용되지 않는(deprecated) 속성. */
extern struct device_attribute dev_attr_events_async;
/* [한국어] dev_attr_events_poll_msecs — sysfs "events_poll_msecs" 속성.
 * 사용자공간이 media-change 이벤트 polling 주기(ms)를 읽거나 조정할 수 있게 한다. */
extern struct device_attribute dev_attr_events_poll_msecs;

/* [한국어] blk_trace_attr_group — /sys/block/<disk>/trace/ 아래 blktrace 관련 sysfs
 * 속성 그룹(enable, act_mask 등). request_queue의 kobject에 등록되어
 * blktrace(사용자공간 도구)가 NVMe I/O 이벤트를 추적할 수 있게 한다. */
extern struct attribute_group blk_trace_attr_group;

/*
 * [한국어]
 * file_to_blk_mode - struct file의 open flags를 blk_mode_t로 변환
 *
 * @file: 대상 파일 (block device 파일)
 * @return: 변환된 blk_mode_t (BLK_OPEN_READ/WRITE/EXCL 등의 조합)
 *
 * file->f_mode/f_flags를 블록 계층이 이해하는 열거형으로 매핑한다.
 * 실제 정의는 block/fops.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (open 경로).
 * 호출자: blkdev_open() (fops.c).
 *
 * 호출 체인:
 *   blkdev_open -> [file_to_blk_mode]
 */
blk_mode_t file_to_blk_mode(struct file *file);
/*
 * [한국어]
 * truncate_bdev_range - block_device의 페이지 캐시 중 지정 범위를 무효화
 *
 * @bdev: 대상 block_device
 * @mode: open mode (쓰기 권한 확인용)
 * @lstart: 시작 오프셋(바이트)
 * @lend: 끝 오프셋(바이트, inclusive)
 * @return: 0=성공, 음수=errno(예: 매핑된 페이지가 아직 사용 중)
 *
 * BLKDISCARD ioctl이나 파티션 크기 변경 등으로 해당 범위의 디스크 내용이
 * 바뀔 때, 오래된 페이지 캐시를 무효화해 다음 읽기가 NVMe에서 새로
 * 읽어오도록 강제한다. 실제 정의는 block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl()의 BLKDISCARD 등 처리 경로.
 *
 * 호출 체인:
 *   blkdev_ioctl -> [truncate_bdev_range]
 */
int truncate_bdev_range(struct block_device *bdev, blk_mode_t mode,
		loff_t lstart, loff_t lend);
/*
 * [한국어]
 * blkdev_ioctl - block device 파일에 대한 ioctl 시스템 호출의 최상위 디스패처
 *
 * @file: 대상 파일
 * @cmd: ioctl 명령 번호(BLKROSET, BLKGETSIZE64, BLKREPORTZONE 등)
 * @arg: 사용자공간 인자(포인터 또는 값)
 * @return: 0 또는 양수=성공(명령별 의미), 음수=errno
 *
 * cmd에 따라 파티션 관리, zone 관리, 크기/한도 조회 등 세부 핸들러로
 * 분기한다. 실제 정의는 block/ioctl.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl() 시스템 호출).
 * 호출자: VFS의 file_operations.unlocked_ioctl (fops.c에서 등록).
 * 피호출자: bdev_add_partition(), disk_scan_partitions(),
 *   blkdev_report_zones_ioctl() 등.
 *
 * 호출 체인:
 *   sys_ioctl -> blkdev_ioctl(fops) -> [blkdev_ioctl]
 */
long blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg);
/*
 * [한국어]
 * blkdev_uring_cmd - io_uring IORING_OP_URING_CMD를 통한 block device 명령 처리
 *
 * @cmd: io_uring 명령 컨텍스트
 * @issue_flags: io_uring 실행 플래그(IO_URING_F_NONBLOCK 등)
 * @return: 0 또는 양수=성공, 음수=errno
 *
 * io_uring 경로로 들어온 passthrough 명령(예: NVMe 벤더 특수 명령)을
 * blkdev_ioctl()과 유사한 방식으로 처리한다. 실제 정의는 block/ioctl.c.
 * 실행 컨텍스트: io_uring 워커 또는 제출 태스크 컨텍스트.
 * 호출자: io_uring 코어의 uring_cmd 디스패처.
 *
 * 호출 체인:
 *   io_uring_cmd -> [blkdev_uring_cmd]
 */
int blkdev_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);
/*
 * [한국어]
 * compat_blkdev_ioctl - 32비트 호환(compat) 프로세스를 위한 ioctl 디스패처
 *
 * @file: 대상 파일
 * @cmd: ioctl 명령 번호
 * @arg: 32비트 호환 인자
 * @return: 0 또는 양수=성공, 음수=errno
 *
 * 64비트 커널에서 32비트 사용자공간 프로세스가 호출한 ioctl의 인자
 * 레이아웃(예: 포인터 크기)을 보정한 뒤 blkdev_ioctl()과 동등한 처리를
 * 수행한다. 실제 정의는 block/ioctl.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (compat ioctl 경로).
 * 호출자: VFS의 file_operations.compat_ioctl.
 *
 * 호출 체인:
 *   sys_ioctl(compat) -> [compat_blkdev_ioctl]
 */
long compat_blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg);

/* [한국어] def_blk_aops — block device 파일(예: /dev/nvme0n1)에 대한 기본
 * address_space_operations (readahead, writepage 등). 파일시스템 없이
 * 블록 장치를 직접 열었을 때(raw I/O) 페이지 캐시 동작을 정의한다.
 * 실제 정의는 block/fops.c. */
extern const struct address_space_operations def_blk_aops;

/*
 * [한국어]
 * disk_register_independent_access_ranges - IA(Independent Access) range sysfs 등록
 *
 * @disk: 대상 gendisk
 * @return: 0=성공, 음수=errno
 *
 * 멀티 액추에이터 HDD처럼 하나의 디스크 내에서 LBA 범위별로 독립적인
 * 성능 특성을 갖는 장치를 위해, 범위별 sysfs 속성을 등록한다. 대부분의
 * NVMe SSD에는 해당하지 않지만 공용 gendisk 등록 경로의 일부다. 실제
 * 정의는 block/blk-ia-ranges.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: add_disk() 경로 (disk->ia_ranges가 설정된 경우).
 *
 * 호출 체인:
 *   add_disk -> [disk_register_independent_access_ranges]
 */
int disk_register_independent_access_ranges(struct gendisk *disk);
/*
 * [한국어]
 * disk_unregister_independent_access_ranges - IA range sysfs 등록 해제
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * 실제 정의는 block/blk-ia-ranges.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: del_gendisk().
 *
 * 호출 체인:
 *   del_gendisk -> [disk_unregister_independent_access_ranges]
 */
void disk_unregister_independent_access_ranges(struct gendisk *disk);

/*
 * [한국어]
 * should_fail_bio - bio 단위 fault injection 검사
 *
 * @bio: 검사할 bio
 * @return: -EIO=인위적 실패 주입, 0=정상 처리
 *
 * bdev_whole()로 파티션을 whole disk로 승격시켜 should_fail_request()를
 * 호출한다. 파티션별로 설정하더라도 확률 계산은 전체 디스크(whole disk)
 * 기준으로 이루어진다. 실제 정의는 block/blk-core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio_noacct 경로).
 * 호출자: submit_bio_noacct() (blk-core.c).
 * 피호출자: should_fail_request(), bdev_whole().
 *
 * 호출 체인:
 *   submit_bio_noacct -> [should_fail_bio] -> should_fail_request
 */
int should_fail_bio(struct bio *bio);
#ifdef CONFIG_FAIL_MAKE_REQUEST
/*
 * [한국어]
 * should_fail_request - fault injection 프레임워크로 요청 실패를 시뮬레이션
 *
 * @part: 대상 block_device (파티션 또는 whole disk)
 * @bytes: 요청 크기(바이트) — fault injection 확률 계산에 사용
 * @return: true=인위적 실패 발생(호출자가 -EIO 등으로 처리해야 함), false=정상
 *
 * BD_MAKE_IT_FAIL 플래그가 설정된 장치에서, /sys 또는 debugfs의
 * fail_make_request 파라미터에 따라 일정 확률로 I/O 실패를 유발한다.
 * NVMe 드라이버의 에러 처리/재시도 경로를 검증하는 테스트 인프라로
 * 사용된다. 실제 정의는 block/blk-core.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio 경로).
 * 호출자: should_fail_bio() (본 파일).
 * 피호출자: should_fail() (lib/fault-inject.c).
 *
 * 호출 체인:
 *   should_fail_bio -> [should_fail_request] -> should_fail
 */
bool should_fail_request(struct block_device *part, unsigned int bytes);
#else /* CONFIG_FAIL_MAKE_REQUEST */
/*
 * [한국어]
 * should_fail_request - CONFIG_FAIL_MAKE_REQUEST 비활성 시 스텁
 *
 * @part: 사용되지 않음
 * @bytes: 사용되지 않음
 * @return: 항상 false (fault injection 자체가 빌드에서 빠짐)
 */
static inline bool should_fail_request(struct block_device *part,
					unsigned int bytes)
{
	return false;								/* fault injection off; NVMe error injection은 runtime config로 별도 제어 */
}
#endif /* CONFIG_FAIL_MAKE_REQUEST */

/*
 * Optimized request reference counting. Ideally we'd make timeouts be more
 * clever, as that's the only reason we need references at all... But until
 * this happens, this is faster than using refcount_t. Also see:
 *
 * abc54d634334 ("io_uring: switch to atomic_t for io_kiocb reference count")
 */
/*
 * [한국어]
 * req_ref_*() 계열 - request(NVMe CID에 대응)의 참조 카운트를 원자적으로 관리
 *
 * NVMe 드라이버는 request 하나당 CID(Command ID)를 할당하며, 완료
 * 처리(bio_endio, blk_mq_end_request)와 timeout/abort 처리가 동시에 같은
 * request를 건드릴 수 있는 경쟁 상태를 막기 위해 참조 카운트가 필요하다.
 * ref가 0이 되어야 비로소 request 구조체와 tag(CID)를 회수해 재사용할 수
 * 있다. refcount_t 대신 순수 atomic_t를 쓰는 이유는, 이 카운트가 거의
 * 항상 1(단일 소유자)이라 refcount_t의 포화(saturation) 보호 오버헤드가
 * 필요 없기 때문이다(리눅스 커밋 abc54d634334 참고).
 */
/* [한국어] req_ref_zero_or_close_to_overflow(req) — ref 값이 0이거나(이미 반납됨)
 * unsigned 오버플로/언더플로 직전(0에 매우 가까운 큰 값, 즉 음수를 unsigned로
 * 잘못 감소시킨 상태)인지 검사하는 매크로.
 * 왜 필요한가: req_ref_put_and_test()가 "이미 0인 rq를 또 put하는" 이중 해제
 * (double-free) 버그를 조기에 WARN으로 잡아내기 위한 방어적 점검 용도다.
 * +127u <= 127u 트릭은 [0, 128) 범위인지를 부호 없는 산술로 검사한다(0 근처
 * 언더플로 값도 이 범위에 들어오게 됨). */
#define req_ref_zero_or_close_to_overflow(req)	\
	((unsigned int) atomic_read(&(req->ref)) + 127u <= 127u)	/* ref가 0이거나 underflow 임박; NVMe CID/tag 재활용 전 경고 */

/*
 * [한국어]
 * req_ref_inc_not_zero - ref가 0이 아닐 때만 증가시켜 참조를 새로 획득
 *
 * @req: 참조를 늘릴 request
 * @return: true=증가 성공(참조 획득됨), false=이미 ref가 0(이 rq는 이미 완료/해제 중)
 *
 * NVMe timeout/abort 핸들러가 "아직 완료 처리 중일 수도 있는" request를
 * 다시 붙잡으려 할 때 사용한다. 만약 다른 CPU가 이미 마지막 참조를
 * 반납해 ref가 0이 되었다면, 그 rq는 곧 재사용될 수 있으므로 여기서
 * 획득에 실패시켜 use-after-free를 막는다.
 * 실행 컨텍스트: 임의 컨텍스트 (timeout 워크, abort 경로 등).
 * 호출자: blk_mq_find_and_get_req() 등 tag로부터 rq를 역참조하는 경로.
 * 피호출자: atomic_inc_not_zero().
 *
 * 호출 체인:
 *   blk_mq_find_and_get_req -> [req_ref_inc_not_zero]
 */
static inline bool req_ref_inc_not_zero(struct request *req)
{
	return atomic_inc_not_zero(&req->ref);				/* NVMe timeout/abort handler가 완료 중인 rq를 다시 잡을 때 사용; 0이면 실패 */
}

/*
 * [한국어]
 * req_ref_put_and_test - 참조를 하나 반납하고, 그것이 마지막 참조였는지 검사
 *
 * @req: 참조를 반납할 request
 * @return: true=이 반납으로 ref가 0에 도달(호출자가 request/tag를 회수해야 함),
 *          false=아직 다른 참조자가 남아 있음
 *
 * atomic_dec_and_test()로 원자적으로 감소시켜, 여러 CPU(완료 인터럽트와
 * timeout 워크 등)가 동시에 마지막 참조를 반납하려 해도 정확히 한
 * 호출자만 true를 받도록 보장한다. 감소 전에 이미 0/오버플로 근접
 * 상태였다면 WARN_ON_ONCE로 이중 해제를 경고한다.
 * 실행 컨텍스트: 임의 컨텍스트 (NVMe CQ 완료 인터럽트, timeout 워크 등).
 * 호출자: blk_mq_free_request() 등 request 최종 반납 경로.
 * 피호출자: req_ref_zero_or_close_to_overflow(), atomic_dec_and_test().
 *
 * 호출 체인:
 *   blk_mq_free_request -> [req_ref_put_and_test]
 */
static inline bool req_ref_put_and_test(struct request *req)
{
	WARN_ON_ONCE(req_ref_zero_or_close_to_overflow(req));		/* 이미 free된 NVMe rq를 put하지 않도록 방어; CID reuse corruption 방지 */
	return atomic_dec_and_test(&req->ref);				/* ref 0 도달 시 NVMe request/CID/tag 회수 가능; atomic으로 race 방지 */
}

/*
 * [한국어]
 * req_ref_set - ref 값을 임의의 초기값으로 강제 설정
 *
 * @req: 대상 request
 * @value: 설정할 값 (일반적으로 1)
 * @return: 없음
 *
 * request가 tag 풀에서 새로 꺼내져 재사용되기 시작할 때, 이전 사용
 * 흔적을 지우고 참조 카운트를 초기 상태(보통 1, 단일 소유자)로
 * 되돌린다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (request 초기화 경로).
 * 호출자: blk_mq_rq_ctx_init() 직후의 request 초기화 경로.
 * 피호출자: atomic_set().
 *
 * 호출 체인:
 *   blk_mq_get_request -> blk_mq_rq_ctx_init -> [req_ref_set]
 */
static inline void req_ref_set(struct request *req, int value)
{
	atomic_set(&req->ref, value);					/* NVMe rq 초기화 시 ref 설정; blk_mq_get_request 직후 value=1 일반적 */
}

/*
 * [한국어]
 * req_ref_read - 현재 참조 카운트 값을 읽기 전용으로 조회
 *
 * @req: 대상 request
 * @return: 현재 ref 값
 *
 * 값을 변경하지 않고 디버깅/검증 목적으로 조회할 때 사용한다(예: NVMe
 * abort 경로에서 이 rq가 여전히 outstanding인지 확인).
 * 실행 컨텍스트: 임의 컨텍스트.
 * 호출자: 디버그/검증 목적의 진단 코드, timeout 경로의 상태 점검.
 * 피호출자: atomic_read().
 *
 * 호출 체인:
 *   (timeout/디버그 경로) -> [req_ref_read]
 */
static inline int req_ref_read(struct request *req)
{
	return atomic_read(&req->ref);					/* NVMe abort 경로에서 outstanding rq가 여전히 살아있는지 확인 */
}

/*
 * [한국어]
 * blk_time_get_ns - plug 단위로 batching 가능한 나노초 단위 타임스탬프 획득
 *
 * @return: 현재(또는 plug가 캐싱한) 시각, 나노초 단위
 *
 * NVMe I/O latency 모니터링과 io_uring 성능 추적을 위해 request/bio에
 * "발행 시각"을 기록할 때 사용된다. 태스크가 plug(current->plug)를 들고
 * 있는 프로세스 컨텍스트라면, plug 수명 동안 최초 1회만 ktime_get_ns()를
 * 호출하고 이후에는 캐시된 값을 재사용한다 — plug에 쌓인 여러 bio/rq가
 * 사실상 "거의 동시에 제출"되었다고 보고, 매번 시각을 읽는 오버헤드를
 * 줄이기 위함이다(대신 batch 내부의 latency 분산은 실제보다 왜곡될 수
 * 있다). plug가 없거나 인터럽트 컨텍스트(in_task()가 거짓)라면 매번
 * 직접 ktime_get_ns()를 호출한다.
 * 실행 컨텍스트: 임의 컨텍스트 (프로세스 컨텍스트에서만 plug 캐싱이 적용됨).
 * 호출자: bio 발행 시각 기록 경로, blk_time_get() (본 파일).
 * 피호출자: ktime_get_ns(), in_task().
 *
 * 호출 체인:
 *   submit_bio 경로 -> [blk_time_get_ns]
 */
static inline u64 blk_time_get_ns(void)
{
	struct blk_plug *plug = current->plug;				/* 현재 task의 plug; scheduler batching으로 인해 여러 NVMe rq가 동일 timestamp 획득 */

	if (!plug || !in_task())
		return ktime_get_ns();						/* plug 없거나 interrupt context면 직접 측정; NVMe ISR/complete 경로 */

	/*
	 * 0 could very well be a valid time, but rather than flag "this is
	 * a valid timestamp" separately, just accept that we'll do an extra
	 * ktime_get_ns() if we just happen to get 0 as the current time.
	 */
	if (!plug->cur_ktime) {
		plug->cur_ktime = ktime_get_ns();				/* plug 수명 동안 동일한 timestamp; batch 내 NVMe rq들의 latency 분산 왜곡 가능(추정) */
		current->flags |= PF_BLOCK_TS;				/* plug flush 시점까지 timestamp 유효 표시; NVMe multi-queue 타임스탬프 정렬 */
	}
	return plug->cur_ktime;							/* NVMe request 발행 시각; later CQE 수신 시점과 차이로 latency 계산 */
}

/*
 * [한국어]
 * blk_time_get - blk_time_get_ns()의 ktime_t 버전
 *
 * @return: 현재(또는 plug 캐싱된) 시각, ktime_t 형태
 *
 * 나노초 정수 대신 ktime_t 타입이 필요한 호출자(예: 타이머 등록 API)를
 * 위한 변환 래퍼다.
 * 실행 컨텍스트: 임의 컨텍스트.
 * 호출자: ktime_t 타입을 요구하는 타임스탬프 소비 경로.
 * 피호출자: blk_time_get_ns(), ns_to_ktime().
 *
 * 호출 체인:
 *   (타임스탬프 소비 경로) -> [blk_time_get] -> blk_time_get_ns
 */
static inline ktime_t blk_time_get(void)
{
	return ns_to_ktime(blk_time_get_ns());				/* ktime 형태로 변환; NVMe timeout timer 등록 시 사용(추정) */
}

/*
 * [한국어]
 * bdev_release - bdev_open()으로 연 block_device를 닫고 참조 반납
 *
 * @bdev_file: bdev_open()이 채운 struct file
 * @return: 없음
 *
 * 배타적 락(holder) 해제, 참조 카운트 감소, 필요 시 마지막 사용자의
 * bdev_drop() 유발까지 이어지는 닫기 절차를 수행한다. 실제 정의는
 * block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_release() (fops.c), bdev_open() 실패 시 정리 경로.
 *
 * 호출 체인:
 *   blkdev_release -> [bdev_release]
 */
void bdev_release(struct file *bdev_file);
/*
 * [한국어]
 * bdev_open - block_device를 열어 struct file에 연결(배타적 락 처리 포함)
 *
 * @bdev: 열 block_device
 * @mode: BLK_OPEN_READ/WRITE/EXCL 등의 조합
 * @holder: 배타적 접근(BLK_OPEN_EXCL) 시 소유자를 식별하는 포인터
 * @hops: holder 콜백 모음(예: 장치 제거 통지)
 * @bdev_file: 결과를 채워 넣을 struct file
 * @return: 0=성공, 음수=errno(예: -EBUSY — 이미 배타적으로 열려 있음)
 *
 * 파일시스템 마운트, O_EXCL open(mkfs 도구 등)에서 사용되는 저수준 open
 * 경로다. 실제 정의는 block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_open() (fops.c), 파일시스템의 마운트 경로(get_tree_bdev 등).
 *
 * 호출 체인:
 *   blkdev_open / get_tree_bdev -> [bdev_open]
 */
int bdev_open(struct block_device *bdev, blk_mode_t mode, void *holder,
	      const struct blk_holder_ops *hops, struct file *bdev_file);
/*
 * [한국어]
 * bdev_permission - dev_t에 대한 접근 권한(capability, 파일 권한)을 검사
 *
 * @dev: 대상 장치의 dev_t
 * @mode: 요청하는 접근 모드(BLK_OPEN_READ/WRITE 등)
 * @holder: 배타적 접근 시 소유자 식별자
 * @return: 0=허용, 음수=errno(-EACCES, -EPERM 등)
 *
 * bdev_open() 이전에 CAP_SYS_ADMIN 등 커널 권한 모델에 따라 이 장치를
 * 열 수 있는지 사전 검사한다. 실제 정의는 block/bdev.c에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: bdev_open() 진입부, blkdev_open() (fops.c).
 *
 * 호출 체인:
 *   blkdev_open -> [bdev_permission] -> bdev_open(계속)
 */
int bdev_permission(dev_t dev, blk_mode_t mode, void *holder);

/*
 * [한국어]
 * bio_integrity_generate - 쓰기 bio에 대해 PI(Protection Information) 태그 생성
 *
 * @bio: 대상 bio (쓰기 연산, integrity payload가 이미 붙어 있음)
 * @return: 없음
 *
 * NVMe end-to-end data protection이 활성화된 namespace에 데이터를 쓰기
 * 전에, 각 논리 블록에 대응하는 Guard(CRC)/Application/Reference 태그를
 * 계산해 bip(bio_integrity_payload)에 채운다. 이 메타데이터는 NVMe
 * 명령의 별도 메타데이터 버퍼(또는 확장 LBA)로 전송된다. 실제 정의는
 * block/t10-pi.c 계열에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (제출 경로, blk_integrity_prepare에서 호출).
 * 호출자: blk_integrity_prepare() (본 파일).
 *
 * 호출 체인:
 *   blk_mq_submit_bio -> blk_integrity_prepare -> [bio_integrity_generate]
 */
void bio_integrity_generate(struct bio *bio);
/*
 * [한국어]
 * bio_integrity_verify - 읽기 완료된 bio의 PI 태그를 검증
 *
 * @bio: 완료된 읽기 bio
 * @saved_iter: 검증에 사용할 데이터 영역의 원래 bvec_iter(진행된 iter를 복원)
 * @return: BLK_STS_OK=검증 통과, 그 외=BLK_STS_PROTECTION 등 에러 코드
 *
 * NVMe CQE와 함께 반환된 데이터의 Guard/APP/REF 태그를, 쓰기 시 계산된
 * 값과 비교해 매체 손상이나 잘못된 위치의 데이터가 아닌지 확인한다.
 * 실제 정의는 block/t10-pi.c 계열에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 컨텍스트.
 * 호출자: blk_integrity_complete() (본 파일).
 *
 * 호출 체인:
 *   bio_endio -> blk_integrity_complete -> [bio_integrity_verify]
 */
blk_status_t bio_integrity_verify(struct bio *bio,
		struct bvec_iter *saved_iter);

/*
 * [한국어]
 * blk_integrity_prepare - request 제출 전 PI 관련 준비(쓰기 시 태그 생성)
 *
 * @rq: 제출될 request
 * @return: 없음
 *
 * request에 속한 bio가 쓰기이고 integrity가 필요하면
 * bio_integrity_generate()를 호출해 태그를 미리 계산해 둔다. 실제
 * 정의는 block/t10-pi.c 계열에 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (제출 경로).
 * 호출자: blk_mq_submit_bio() 또는 드라이버의 큐 등록 콜백 경로.
 * 피호출자: bio_integrity_generate().
 *
 * 호출 체인:
 *   blk_mq_submit_bio -> [blk_integrity_prepare] -> bio_integrity_generate
 */
void blk_integrity_prepare(struct request *rq);
/*
 * [한국어]
 * blk_integrity_complete - request 완료 시 PI 관련 마무리(읽기 시 태그 검증)
 *
 * @rq: 완료된 request
 * @nr_bytes: 이번에 완료 처리된 바이트 수(부분 완료 지원)
 * @return: 없음
 *
 * request에 속한 bio가 읽기이고 integrity가 필요하면
 * bio_integrity_verify()를 호출해 검증 결과를 bio 상태에 반영한다.
 * 실제 정의는 block/t10-pi.c 계열에 있다.
 * 실행 컨텍스트: NVMe CQ 완료 인터럽트/softirq 컨텍스트.
 * 호출자: blk_mq_end_request() 등 완료 처리 경로(blk-mq.c).
 * 피호출자: bio_integrity_verify().
 *
 * 호출 체인:
 *   nvme_complete_rq -> blk_mq_end_request -> [blk_integrity_complete]
 *   -> bio_integrity_verify
 */
void blk_integrity_complete(struct request *rq, unsigned int nr_bytes);

#ifdef CONFIG_LOCKDEP
/*
 * [한국어]
 * blk_freeze_acquire_lock - freeze 시작 시 lockdep에 가상의 쓰기 락 획득을 기록
 *
 * @q: 동결되는 request_queue
 * @return: 없음
 *
 * 실제로 뮤텍스/세마포어를 잠그는 것이 아니라, "freeze는 이 큐에 대한
 * 배타적 쓰기 접근과 유사한 의존관계를 갖는다"는 사실을 lockdep에 알려,
 * freeze와 다른 잠금(예: io_lockdep_map을 읽기 잠그는 일반 I/O 경로) 간의
 * 잘못된 락 순서로 인한 잠재적 교착을 정적으로 검출할 수 있게 한다.
 * mq_freeze_disk_dead/mq_freeze_queue_dying이 이미 참이면(디스크/큐가
 * 이미 죽어 나가는 중) 중복 lockdep 어노테이션을 피하기 위해 건너뛴다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (freeze 시작 경로).
 * 호출자: blk_freeze_queue_start() (blk-mq.c).
 * 피호출자: rwsem_acquire() (lockdep 전용, 실제 락 없음).
 *
 * 호출 체인:
 *   blk_freeze_queue_start -> [blk_freeze_acquire_lock]
 */
static inline void blk_freeze_acquire_lock(struct request_queue *q)
{
	if (!q->mq_freeze_disk_dead)
		rwsem_acquire(&q->io_lockdep_map, 0, 1, _RET_IP_);	/* NVMe controller live I/O lockdep; reset 중 freeze와의 교차 잠금 검사 */
	if (!q->mq_freeze_queue_dying)
		rwsem_acquire(&q->q_lockdep_map, 0, 1, _RET_IP_);	/* queue 자체 lockdep; NVMe queue remove 시 q_lockdep_map 검증 */
}

/*
 * [한국어]
 * blk_unfreeze_release_lock - blk_freeze_acquire_lock()의 lockdep 어노테이션 해제
 *
 * @q: 동결 해제되는 request_queue
 * @return: 없음
 *
 * freeze/unfreeze는 반드시 짝을 이루어야 하므로, acquire와 정확히 반대
 * 순서로 release를 호출해 lockdep의 락 순서 그래프를 일관되게 유지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (unfreeze 경로).
 * 호출자: blk_mq_unfreeze_queue() 계열 (blk-mq.c).
 * 피호출자: rwsem_release() (lockdep 전용).
 *
 * 호출 체인:
 *   blk_mq_unfreeze_queue -> [blk_unfreeze_release_lock]
 */
static inline void blk_unfreeze_release_lock(struct request_queue *q)
{
	if (!q->mq_freeze_queue_dying)
		rwsem_release(&q->q_lockdep_map, _RET_IP_);		/* NVMe queue dying 상태가 아니면 queue lockdep 해제 */
	if (!q->mq_freeze_disk_dead)
		rwsem_release(&q->io_lockdep_map, _RET_IP_);		/* NVMe disk live 상태면 I/O lockdep 해제; freeze/unfreeze 짝 맞춤 */
}
#else
/*
 * [한국어]
 * blk_freeze_acquire_lock - CONFIG_LOCKDEP 비활성 시 스텁 (아무 동작 없음)
 * @q: 사용되지 않음 / @return: 없음
 */
static inline void blk_freeze_acquire_lock(struct request_queue *q)
{
}
/*
 * [한국어]
 * blk_unfreeze_release_lock - CONFIG_LOCKDEP 비활성 시 스텁 (아무 동작 없음)
 * @q: 사용되지 않음 / @return: 없음
 */
static inline void blk_unfreeze_release_lock(struct request_queue *q)
{
}
#endif

/*
 * debugfs directory and file creation can trigger fs reclaim, which can enter
 * back into the block layer request_queue. This can cause deadlock if the
 * queue is frozen. Use NOIO context together with debugfs_mutex to prevent fs
 * reclaim from triggering block I/O.
 */
/*
 * [한국어]
 * blk_debugfs_lock_nomemsave - debugfs_mutex만 잠금(NOIO 전환은 호출자 책임)
 *
 * @q: 대상 request_queue
 * @return: 없음
 *
 * debugfs 디렉터리/파일 생성·삭제나 상태 dump 동안 동시 접근을 막는
 * 뮤텍스만 잠근다. 이미 NOIO 컨텍스트에 있는 호출자(예: 이미
 * memalloc_noio_save를 호출한 상위 함수)를 위한 저수준 버전이다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_debugfs_lock() (본 파일, NOIO 전환까지 포함한 상위 버전).
 * 피호출자: mutex_lock().
 *
 * 호출 체인:
 *   blk_debugfs_lock -> [blk_debugfs_lock_nomemsave]
 */
static inline void blk_debugfs_lock_nomemsave(struct request_queue *q)
{
	mutex_lock(&q->debugfs_mutex);					/* NVMe debugfs 상태 읽기/쓰기 직렬화; queue freeze와의 deadlock 회피 */
}

/*
 * [한국어]
 * blk_debugfs_unlock_nomemrestore - blk_debugfs_lock_nomemsave()의 짝 함수
 *
 * @q: 대상 request_queue
 * @return: 없음
 *
 * debugfs_mutex만 해제한다(NOIO 복원은 호출자 책임).
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_debugfs_unlock() (본 파일).
 * 피호출자: mutex_unlock().
 *
 * 호출 체인:
 *   blk_debugfs_unlock -> [blk_debugfs_unlock_nomemrestore]
 */
static inline void blk_debugfs_unlock_nomemrestore(struct request_queue *q)
{
	mutex_unlock(&q->debugfs_mutex);				/* NVMe debugfs 접근 종료; 다음 상태 dump 가능 */
}

/*
 * [한국어]
 * blk_debugfs_lock - NOIO 컨텍스트로 전환한 뒤 debugfs_mutex를 획득
 *
 * @q: 대상 request_queue
 * @return: 이후 blk_debugfs_unlock()에 전달해야 하는 memflags(복원용 플래그)
 *
 * debugfs 디렉터리/파일 생성은 커널 내부적으로 fs reclaim을 유발할 수
 * 있는데, 만약 이 reclaim이 다시 블록 계층으로 I/O를 넣으려 하고 그
 * 큐가 마침 freeze된 상태라면 데드락이 발생한다. 이를 막기 위해
 * memalloc_noio_save()로 현재 태스크의 GFP 플래그에서 __GFP_IO/
 * __GFP_FS를 제거한 뒤 뮤텍스를 잠근다. __must_check로 표시되어 있어
 * 반환값(memflags)을 반드시 blk_debugfs_unlock()에 전달해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: debugfs 디렉터리/파일을 만들거나 여는 블록 계층 코드
 *   (blk-mq-debugfs.c 등).
 * 피호출자: memalloc_noio_save(), blk_debugfs_lock_nomemsave().
 *
 * 호출 체인:
 *   (debugfs 생성/조회 경로) -> [blk_debugfs_lock] -> memalloc_noio_save
 */
static inline unsigned int __must_check blk_debugfs_lock(struct request_queue *q)
{
	unsigned int memflags = memalloc_noio_save();			/* GFP_IO/GFP_FS 재귀 방지; NVMe queue debugfs에서 reclaim으로 인한 I/O 재진입 차단 */

	blk_debugfs_lock_nomemsave(q);
	return memflags;							/* 이후 memalloc_noio_restore(memflags)와 짝을 이룸; NVMe debugfs 접근 범위 한정 */
}

/*
 * [한국어]
 * blk_debugfs_unlock - blk_debugfs_lock()으로 얻은 락과 NOIO 상태를 함께 복원
 *
 * @q: 대상 request_queue
 * @memflags: blk_debugfs_lock()이 반환했던 값
 * @return: 없음
 *
 * 뮤텍스를 먼저 풀고, 그 다음 memalloc_noio_restore()로 태스크의 GFP
 * 플래그를 원래 상태로 되돌린다(락 해제 -> NOIO 해제 순서로, 획득의
 * 역순).
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blk_debugfs_lock()과 짝을 이루는 정리 경로.
 * 피호출자: blk_debugfs_unlock_nomemrestore(), memalloc_noio_restore().
 *
 * 호출 체인:
 *   (debugfs 생성/조회 경로) -> [blk_debugfs_unlock] -> memalloc_noio_restore
 */
static inline void blk_debugfs_unlock(struct request_queue *q,
				      unsigned int memflags)
{
	blk_debugfs_unlock_nomemrestore(q);
	memalloc_noio_restore(memflags);				/* NOIO context 복원; NVMe 정상 I/O 메모리 할당 정책으로 되돌림 */
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - block/blk.h는 request_queue, request, bio, flush, elevator, integrity,
 *   zone 등 블록 계층 납비 인프라를 선언하며, NVMe 드라이버가 SQ/CQ,
 *   doorbell, CID, PRP/SGL, FLUSH, Dataset Management로 변환하기 전의
 *   중간 지점이다.
 *
 * - 병합(merge)/분할(split) 정책과 queue_limits(max_sectors, max_segments,
 *   seg_boundary_mask, virt_boundary_mask)는 NVMe command 하나가 PRP/SGL
 *   entry를 얼마나 사용하고, MDTS를 얼마나 채우는지를 직접 결정한다.
 *
 * - blk_flush_queue는 NVMe FLUSH 명령이 outstanding data write들이 모두
 *   완료된 뒤에만 SQ로 발행되도록 보장하여, volatile write cache의
 *   일관성을 유지한다.
 *
 * - q_usage_counter 기반의 큐 진입 제어는 NVMe 컨트롤러 reset/제거 시
 *   bio/request가 죽은 queue로 들어가지 않도록 방어한다.
 *
 * - bio_integrity / blk_integrity 경로는 NVMe Protection Information(DIF/
 *   DIX) 태그를 생성/검증하여 end-to-end data integrity를 제공한다.
 * ============================================================================
 */

#endif /* BLK_INTERNAL_H */
