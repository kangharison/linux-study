// SPDX-License-Identifier: GPL-2.0
/*
 *  Block device elevator/IO-scheduler.
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 * 30042000 Jens Axboe <axboe@kernel.dk> :
 *
 * Split the elevator a bit so that it is possible to choose a different
 * one or even write a new "plug in". There are three pieces:
 * - elevator_fn, inserts a new request in the queue list
 * - elevator_merge_fn, decides whether a new buffer can be merged with
 *   an existing request
 * - elevator_dequeue_fn, called when a request is taken off the active list
 *
 * 20082000 Dave Jones <davej@suse.de> :
 * Removed tests for max-bomb-segments, which was breaking elvtune
 *  when run without -bN
 *
 * Jens:
 * - Rework again to work with bio instead of buffer_heads
 * - loose bi_dev comparisons, partition handling is right now
 * - completely modularize elevator setup and teardown
 *
 */
/*
 * [한국어 설명] 블록 계층 IO 스케줄러(elevator) 핵심 구현 (elevator.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 블록 계층에서 IO 스케줄러(elevator)의 공통 인터페이스와
 * 핵심 자료구조 관리를 담당한다. elevator는 응용이 발행한 bio/request를
 * mq-deadline·BFQ·kyber·none 등의 스케줄러 플러그인에 연결하고, 요청 병합
 * (merge)·LBA 기반 해시·RB-tree를 통해, 드라이버로 내려가기 전 단계에서 요청을
 * 정렬·병합·스케줄링한다. 스케줄러 등록/해제, sysfs 연동, 동적 교체(switch)
 * 로직도 이 파일에 있다.
 *
 * 이 파일의 코드는 장치 종류와 무관한 일반 코드다. 이 파일 안에는 nvme·scsi·
 * virtio 같은 드라이버 식별자가 하나도 없고, 드라이버에 닿는 유일한 통로는
 * blk-mq가 나중에 수행하는 간접 호출 q->mq_ops->queue_rq()뿐이다. 따라서
 * 아래의 NVMe 언급은 모두 "이 일반 코드가 NVMe 장치에서 어떻게 보이는가"라는
 * 맥락 설명이며, elevator.c가 NVMe 함수를 직접 부른다는 뜻이 아니다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IO 경로에서 elevator는 blk-mq 제출 경로와 드라이버 사이에 위치한다:
 *
 *   [응용] write(2) → submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓  → elv_merge(): 기존 request와 병합 시도 (back/front/discard)
 *       ↓  → elv_attempt_insert_merge(): 신규 request를 해시에서 찾은 후보와 병합
 *       ↓  → 스케줄러 ops.insert_requests(): mq-deadline/BFQ 큐에 삽입
 *       ↓
 *   [blk-mq dispatch] ops.dispatch_request(): 스케줄러가 최적 request 선택
 *       ↓
 *   [blk-mq] blk_mq_dispatch_rq_list() → q->mq_ops->queue_rq() (간접 호출)
 *       ↓
 *   [드라이버] NVMe PCIe라면 이 함수 포인터가 nvme_queue_rq()이다
 *              (drivers/nvme/host/pci.c 의 nvme_mq_ops.queue_rq 에서 확인 가능).
 *
 * elevator가 없을 때("none" 선택 시): blk-mq가 직접 request를 드라이버에 전달.
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(bio 제출 경로); elevator_release는
 * kobject_put() 경로이므로 어떤 컨텍스트에서도 호출될 수 있다.
 *
 * === NVMe 독자가 이 파일에서 가장 먼저 알아야 할 것: 기본값이 "none"이다 ===
 * 고성능 NVMe 장치에서는 I/O 스케줄러를 아예 붙이지 않는 것이 기본이다.
 * 근거는 추측이 아니라 이 파일의 elevator_set_default() 코드 그 자체다:
 *
 *     if ((q->nr_hw_queues == 1 || blk_mq_is_shared_tags(q->tag_set->flags)))
 *             err = elevator_change(q, &ctx);   // 여기서만 mq-deadline을 붙인다
 *
 * 즉 하드웨어 큐가 1개이거나 태그를 공유하는 장치에만 mq-deadline이 붙고,
 * 그 조건에 해당하지 않으면 elevator_change()가 아예 호출되지 않아 q->elevator가
 * NULL로 남는다 — 그것이 곧 "none"이다.
 * PCIe NVMe는 nvme_alloc_io_tag_set()에서 set->nr_hw_queues = ctrl->queue_count - 1
 * (drivers/nvme/host/core.c)로 CPU 수에 맞춘 여러 개의 하드웨어 큐를 만들고,
 * BLK_MQ_F_TAG_HCTX_SHARED를 설정하지 않는다. 따라서 위 조건이 거짓이 되어
 * 스케줄러가 붙지 않는다.
 * (BLK_MQ_F_NO_SCHED_BY_DEFAULT 때문이 아니다. 이 트리에서 그 플래그를 설정하는
 *  드라이버는 drivers/block/loop.c와 drivers/block/null_blk/main.c뿐이며,
 *  NVMe 드라이버는 설정하지 않는다.)
 *
 * 왜 그것이 옳은 선택인가:
 *   - 스케줄러의 본래 목적은 회전 디스크의 탐색 시간을 줄이려고 LBA 순서로
 *     재정렬하는 것이었다. NVMe SSD에는 탐색 동작이 없어 이 이득이 사라진다.
 *   - 스케줄러 인스턴스는 큐당 하나(q->elevator)이고 mq-deadline은 내부적으로
 *     dd->lock 하나로 모든 하드웨어 큐의 삽입/디스패치를 직렬화한다. NVMe처럼
 *     CPU 수만큼 하드웨어 큐가 있고 모든 CPU가 동시에 제출하는 환경에서는
 *     이 단일 락이 곧 병목이 된다.
 *   - NVMe는 이미 하드웨어 큐가 CPU 수만큼 있어 CPU 간 경합 없이 병렬 제출이
 *     가능하다. 소프트웨어에서 다시 순서를 만들 이유가 없다.
 * 반대로 nr_hw_queues == 1인 장치(단일 큐 SATA/USB 등, 또는 I/O 큐를 하나만
 * 만든 NVMe 구성)는 어차피 하드웨어 큐 하나로 직렬화되므로, 재정렬·기아 방지의
 * 이득이 락 비용을 넘어선다. 그래서 그 경우에만 mq-deadline이 기본으로 붙는다.
 *
 * 사용자는 언제든 sysfs로 이 기본값을 뒤집을 수 있다:
 *   cat  /sys/block/nvme0n1/queue/scheduler   → elv_iosched_show()
 *   echo mq-deadline > /sys/block/nvme0n1/queue/scheduler → elv_iosched_store()
 * (이 sysfs 파일 자체는 block/blk-sysfs.c가 만들고, 읽기/쓰기 핸들러만
 *  이 파일의 elv_iosched_show()/elv_iosched_store()로 연결된다.)
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-mq.c: blk_mq_submit_bio()→elv_merge() 호출, blk_mq_init_sched()
 *   - block/blk-mq-sched.c: blk_mq_init_sched()·blk_mq_exit_sched() — elevator
 *     초기화·해제 시 tag_set과 연결
 *   - block/mq-deadline.c, bfq-iosched.c, kyber-iosched.c: elv_register()로
 *     elv_list에 등록; ops 콜백(insert/dispatch/allow_merge 등) 제공
 *   - block/blk-merge.c: blk_attempt_req_merge()·blk_try_merge() 구현
 *   - block/blk-ioc.c: ioc_clear_queue() — elevator 종료 시 io_context 정리
 * 공유 자료구조:
 *   - struct elevator_queue (elevator.h): type·kobj·hash·flags·elevator_data
 *   - struct elevator_type (elevator.h): ops vtable·elevator_name·icq_cache
 *   - elv_list (전역): 등록된 스케줄러 목록, elv_list_lock으로 보호
 *
 * === 주요 함수/구조체 요약 ===
 * elv_merge()              - bio가 기존 request와 병합 가능한지 결정; 해시·캐시·스케줄러 순으로 탐색
 * elv_attempt_insert_merge() - 신규 request를 해시에서 찾은 후보에 연속 back-merge; 드라이버로 내려갈 request 수 감소
 * elv_register/unregister()  - 스케줄러 모듈이 전역 elv_list에 등록/해제
 * elevator_change()          - sysfs/내부 요청으로 elevator를 동적 교체; queue freeze→switch→unfreeze
 * elevator_set_default()     - 장치 등록 시 mq-deadline(단일큐) 또는 none(멀티큐) 기본 적용
 * elv_rqhash_{add/del/find}()- 끝 섹터 기반 해시로 back-merge 후보를 O(1) 탐색
 * elv_rb_{add/del/find}()    - LBA 기반 RB-tree로 front-merge·dispatch 순서를 O(log N) 탐색
 * struct elevator_queue      - 디바이스 큐(request_queue)에 붙는 elevator 상태; type·hash·kobj
 * struct elevator_type       - 스케줄러 플러그인 설명자; ops vtable·이름·icq 캐시
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/blktrace_api.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-pm.h"
#include "blk-wbt.h"
#include "blk-cgroup.h"

/* [한국어] 전역 스케줄러 등록 목록(elv_list)을 보호하는 스핀락.
 * 보호 대상: elv_list 자체의 연결 구조와, 그 위에서 수행하는 elevator_tryget().
 * 경쟁 상황: mq-deadline/bfq/kyber는 모듈로 빌드될 수 있어, 한 CPU가
 *   sysfs로 스케줄러를 바꾸는 중에 다른 CPU가 rmmod로 그 스케줄러를 목록에서
 *   빼려 할 수 있다. 락 없이는 이미 해제된 elevator_type을 참조하게 된다.
 * 범위: 시스템 전역(큐별이 아님). 스케줄러 "종류"는 모든 블록 장치가 공유하는
 *   전역 자원이기 때문이다. 큐별 인스턴스는 q->elevator가 따로 갖는다. */
static DEFINE_SPINLOCK(elv_list_lock);
/* [한국어] 커널에 등록된 모든 I/O 스케줄러(struct elevator_type)의 목록.
 * 설정자: elv_register()가 모듈 로드 시 추가, elv_unregister()가 언로드 시 제거.
 * 읽는 자: __elevator_find()(이름으로 조회), elv_iosched_show()(sysfs에서
 *   선택 가능한 목록 출력).
 * NVMe와의 관계: /sys/block/nvme0n1/queue/scheduler를 cat하면 이 목록이
 *   나오고, echo로 쓰면 여기서 이름을 찾아 큐에 붙인다. NVMe 기본값은
 *   "none"(스케줄러 없음)인데, 하드웨어 큐가 여러 개라 소프트웨어에서
 *   순서를 다시 정할 이유가 적기 때문이다. */
static LIST_HEAD(elv_list);

/*
 * Merge hash stuff.
 */
#define rq_hash_key(rq)		(blk_rq_pos(rq) + blk_rq_sectors(rq))

/*
 * [한국어] rq_hash_key - request의 "끝나는 섹터"를 해시 키로 만든다
 *
 * blk_rq_pos(rq)는 시작 섹터, blk_rq_sectors(rq)는 길이이므로 둘을 더하면
 * 이 request가 끝나는 지점(= 다음 섹터의 시작 위치)이 된다.
 *
 * 왜 "끝"을 키로 쓰는가:
 *   back merge는 "새 bio의 시작 == 기존 request의 끝"일 때 성립한다. 따라서
 *   request를 끝 섹터로 해시해 두면, 새 bio가 왔을 때 그 bio의 시작 섹터로
 *   해시를 한 번 조회하는 것만으로 O(1)에 병합 후보를 찾을 수 있다.
 *   시작 섹터로 해시했다면 후보를 찾기 위해 전체를 뒤져야 했을 것이다.
 *
 * front merge는 이 해시로 찾을 수 없다(반대 방향이므로). 그래서 elv_merge()는
 * 해시 조회가 실패하면 스케줄러의 request_merge 콜백으로 넘겨, 정렬된
 * rb-tree에서 front 후보까지 찾게 한다.
 *
 * NVMe 관점: 이 해시 조회가 한 번 성공할 때마다 bio 하나가 기존 request에
 * 흡수되어, 나중에 드라이버로 내려가는 request가 하나 줄어든다. NVMe에서
 * request 하나는 커맨드 하나(= SQ 엔트리 하나 + Command ID 하나 + 완료 CQ
 * 엔트리 하나)에 대응하므로, 그만큼의 제출·완료 비용이 절약된다.
 * 주의: 병합은 커맨드 "개수"를 줄이는 것이지, 한 커맨드의 데이터 서술
 * (PRP/SGL) 길이를 줄이는 것이 아니다. 오히려 한 커맨드가 더 많은 데이터를
 * 담게 되므로 그 커맨드의 PRP 엔트리 수는 늘어난다.
 * 다만 앞서 설명한 대로 멀티큐 NVMe의 기본값은 스케줄러 없음("none")이라,
 * 이 해시가 아니라 blk_attempt_plug_merge()(plug 리스트 기반)가 병합을
 * 담당하는 경우가 실제로는 더 흔하다.
 */
/*
 * Query io scheduler to see if the current process issuing bio may be
 * merged with rq.
 */
/*
 * elv_iosched_allow_bio_merge - IO 스케줄러가 @bio를 @rq와 병합할 수 있는지 확인
 *   호출 경로: elv_bio_merge_ok -> elv_iosched_allow_bio_merge
 *            -> (스케줄러별 allow_merge)
 *   NVMe 연결: mq-deadline/bfq 등에서 정책상 병합을 허용하면, 이후
 *              blk_try_merge() 또는 blk_attempt_req_merge()로 연결되어
 *              두 개였을 request가 하나로 합쳐진다. NVMe에서 request 하나는
 *              커맨드 하나이므로, 발행되는 NVMe 커맨드 수가 그만큼 줄어든다.
 */
static bool elv_iosched_allow_bio_merge(struct request *rq, struct bio *bio)
{
	/* [한국어] request가 속한 큐. 스케줄러 콜백이 큐 컨텍스트를 필요로 한다. */
	struct request_queue *q = rq->q;
	/* [한국어] 이 큐에 붙은 스케줄러 인스턴스. 이 함수는 스케줄러가 반드시
	 * 존재하는 경로(elv_merge 등)에서만 호출되므로 NULL 검사를 하지 않는다.
	 * 스케줄러가 없는 NVMe 기본 구성("none")에서는 이 경로 자체를 타지 않고
	 * blk_attempt_plug_merge()가 병합을 처리한다. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] allow_merge 콜백은 선택 사항이다. 구현한 스케줄러:
	 *   BFQ  - bfq_allow_bio_merge(). 두 I/O가 같은 bfq_queue(대략 같은
	 *          프로세스)에 속하는지 확인한다. 다른 프로세스의 I/O를 합치면
	 *          완료 시 서비스 시간을 누구에게 청구할지 모호해져, BFQ의
	 *          가중치 기반 공정성이 무너진다.
	 *   mq-deadline / kyber - 구현하지 않음. 지연 목표만 관리하고 프로세스
	 *          단위 공정성은 추구하지 않으므로 병합을 막을 이유가 없다. */
	if (e->type->ops.allow_merge)
		return e->type->ops.allow_merge(q, rq, bio);

	/* [한국어] 콜백이 없으면 허용이 기본값 — 병합은 거의 항상 이득이므로
	 * "명시적으로 막지 않는 한 허용"이 올바른 기본 정책이다. */
	return true;
}

/*
 * can we safely merge with this request?
 */
/*
 * elv_bio_merge_ok - 요청 병합의 기본 안전성 검사
 *   blk_rq_merge_ok()로 장치/방향/기타 제약을 확인하고,
 *   스케줄러 정책 허용 여부를 elv_iosched_allow_bio_merge()로 재확인한다.
 *   NVMe 연결: 병합에 성공하면 여러 bio가 하나의 struct request로 묶인다.
 *              그 request가 나중에 NVMe 드라이버에 닿으면 nvme_setup_rw()가
 *              cmnd->rw.slba = blk_rq_pos()/기하학 보정, cmnd->rw.length =
 *              blk_rq_sectors() 기반으로 커맨드 하나를 만든다
 *              (drivers/nvme/host/core.c 에서 확인 가능). 즉 병합된 만큼
 *              SQ 엔트리·Command ID·완료 CQ 엔트리가 각각 하나씩 절약된다.
 *              (elevator.c가 nvme_setup_rw()를 직접 부르는 것은 아니다.
 *               blk-mq → mq_ops->queue_rq 간접 호출을 거친 뒤의 이야기다.)
 *
 * === 두 단계 검사인 이유 ===
 * blk_rq_merge_ok()는 "하드웨어와 데이터 정합성상 합쳐도 되는가"(연산 종류,
 * cgroup, PI, 암호화, 우선순위)를 보고, elv_iosched_allow_bio_merge()는
 * "스케줄러 정책상 합쳐도 되는가"를 본다. 후자가 필요한 이유는 BFQ 같은
 * 공정성 스케줄러 때문이다 — 서로 다른 프로세스의 I/O를 합치면 그 request가
 * 완료될 때 누구의 몫으로 계산할지 모호해져 공정성 보장이 무너진다.
 * mq-deadline은 allow_merge 콜백이 없어 항상 허용한다.
 */
bool elv_bio_merge_ok(struct request *rq, struct bio *bio)
{
	/* [한국어] 1단계 — 하드웨어·정합성 검사(block/blk-merge.c). 연산 종류,
	 * cgroup 소속, 무결성(PI) 설정, 인라인 암호화 컨텍스트, write hint,
	 * write stream, I/O 우선순위, 원자성 요구가 모두 호환되어야 한다.
	 * 하나라도 어긋나면 합쳐진 커맨드가 잘못된 결과를 내므로 즉시 거부한다. */
	if (!blk_rq_merge_ok(rq, bio))
		return false;

	/* [한국어] 2단계 — 스케줄러 정책 검사. 하드웨어적으로는 가능하지만
	 * 스케줄러가 원하지 않을 수 있다(BFQ의 프로세스 간 격리 등).
	 * 순서가 중요하다: 싸고 자주 걸리는 하드웨어 검사를 먼저 해서, 대부분의
	 * 거부를 콜백 호출 없이 처리한다. */
	if (!elv_iosched_allow_bio_merge(rq, bio))
		return false;

	/* [한국어] 두 관문 통과 — 병합 가능. 호출자(elv_merge, elv_rqhash_find
	 * 이후 경로)가 blk_try_merge()로 방향(back/front/discard)을 정한다. */
	return true;
}
EXPORT_SYMBOL(elv_bio_merge_ok);

/**
 * elevator_match - Check whether @e's name or alias matches @name
 * @e: Scheduler to test
 * @name: Elevator name to test
 *
 * Return true if the elevator @e's name or alias matches @name.
 */
/*
 * [한국어]
 * elevator_match - 스케줄러의 정식 이름 또는 별칭이 @name과 일치하는지 확인
 *
 * @e:    검사할 스케줄러 타입
 * @name: 찾는 이름. 보통 사용자가 /sys/block/nvme0n1/queue/scheduler에 쓴 문자열.
 * @return: true = 이 스케줄러가 요청된 것
 *
 * 별칭(elevator_alias)이 존재하는 이유는 하위 호환이다. 예를 들어 예전
 * 단일 큐 시절의 "deadline"이라는 이름으로 쓰는 스크립트가 많은데, 지금의
 * 구현은 "mq-deadline"이다. 별칭 덕분에 옛 스크립트가 그대로 동작한다.
 *
 * 실행 컨텍스트: elv_list_lock을 쥔 상태(호출자 __elevator_find가 잡는다).
 * 순수 문자열 비교로 부수 효과가 없다.
 *
 * 호출 체인:
 *   elv_iosched_store(sysfs) → elevator_find_get → __elevator_find
 *     → [elevator_match]
 */
static bool elevator_match(const struct elevator_type *e, const char *name)
{
	/* [한국어] 정식 이름이 일치하거나, 별칭이 등록되어 있고 그것이 일치하면 참.
	 * 단락 평가 순서상 정식 이름을 먼저 보므로, 별칭 검사는 정식 이름이
	 * 다를 때만 수행된다. elevator_alias NULL 검사를 먼저 하는 것은
	 * strcmp에 NULL을 넘기는 것을 막기 위한 필수 가드다. */
	return !strcmp(e->elevator_name, name) ||
		(e->elevator_alias && !strcmp(e->elevator_alias, name));
}

/*
 * [한국어]
 * __elevator_find - 등록 목록에서 이름으로 스케줄러를 찾는다(참조 획득 없음)
 *
 * @name: 찾을 스케줄러 이름
 * @return: 찾은 elevator_type, 없으면 NULL
 *
 * 앞의 밑줄 두 개는 커널 관례로 "락을 이미 잡은 상태에서 호출하라"는 뜻이다.
 * 호출자 elevator_find_get()이 elv_list_lock을 쥐고 이 함수를 부른 뒤,
 * 같은 락 안에서 참조까지 획득한다. 찾기와 참조 획득이 원자적으로 이루어져야
 * 그 사이에 모듈이 언로드되는 것을 막을 수 있기 때문이다.
 *
 * 실행 컨텍스트: elv_list_lock을 반드시 쥔 상태.
 *
 * 호출 체인:
 *   elevator_find_get → [__elevator_find] → elevator_match
 *   elv_register (중복 등록 검사) → [__elevator_find]
 */
static struct elevator_type *__elevator_find(const char *name)
{
	struct elevator_type *e;

	/* [한국어] 등록된 스케줄러를 선형 탐색한다. 목록에 있는 항목이 보통 3~5개
	 * (none은 목록에 없고, mq-deadline/bfq/kyber 정도)라 해시 같은 자료구조를
	 * 쓸 이유가 없다. 게다가 이 경로는 sysfs로 스케줄러를 바꿀 때만 실행되는
	 * 냉경로(cold path)라 성능이 문제되지 않는다. */
	list_for_each_entry(e, &elv_list, list)
		if (elevator_match(e, name))
			return e;
	/* [한국어] 없음 — 사용자가 존재하지 않는 스케줄러 이름을 썼거나, 해당
	 * 모듈이 아직 로드되지 않았다. 호출자가 request_module()로 자동 로드를
	 * 시도한 뒤 다시 찾기도 한다. */
	return NULL;
}

/*
 * [한국어]
 * elevator_find_get - 이름으로 스케줄러를 찾고 모듈 참조까지 안전하게 획득
 *
 * @name: 찾을 스케줄러 이름
 * @return: 참조를 획득한 elevator_type. 없거나 획득 실패면 NULL.
 *          호출자는 다 쓴 뒤 elevator_put()으로 반드시 반납해야 한다.
 *
 * === 찾기와 참조 획득이 같은 락 안에 있어야 하는 이유 ===
 * 만약 락을 풀고 나서 참조를 잡으려 하면 그 사이에 다른 CPU가 rmmod로
 * 모듈을 내려 elevator_type 구조체 자체가 해제될 수 있다. 그러면 이미
 * 해제된 메모리에 대해 참조를 잡으려 시도하는 use-after-free가 된다.
 * 그래서 락 안에서 "찾기 → 참조 획득"을 원자적으로 끝낸다.
 *
 * elevator_tryget()이 실패할 수 있는 이유: 모듈 언로드가 이미 시작되어
 * 모듈 참조 카운트를 더 올릴 수 없는 상태다. 아직 목록에서 제거되지는
 * 않았지만 곧 사라질 것이므로, 새로 쓰겠다고 잡아서는 안 된다.
 *
 * NVMe 관점: 사용자가 /sys/block/nvme0n1/queue/scheduler에 "mq-deadline"을
 * 쓰면 이 함수로 스케줄러를 확보한 뒤 큐에 붙인다. 참조를 잡아 두므로
 * 그 스케줄러를 사용하는 큐가 하나라도 있는 한 모듈을 내릴 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write 또는 큐 초기화).
 * 내부에서 스핀락을 잡으므로 그 구간에서는 잠들 수 없다.
 *
 * 호출 체인:
 *   elv_iosched_store / elevator_init_mq → [elevator_find_get]
 *     → __elevator_find → elevator_tryget
 */
static struct elevator_type *elevator_find_get(const char *name)
{
	struct elevator_type *e;

	/* [한국어] 목록 보호 락 획득. 이 구간 안에서 찾기와 참조 획득을 모두 끝낸다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 이름으로 조회. 락을 이미 쥐었으므로 __ 접두사 버전을 쓴다. */
	e = __elevator_find(name);
	/* [한국어] 찾았다면 모듈 참조를 시도한다. try_module_get() 기반이라
	 * 언로드가 진행 중이면 실패한다. 실패 시 e를 NULL로 만들어 "못 찾았다"와
	 * 같은 결과로 취급한다 — 호출자 입장에서 "쓸 수 없다"는 점은 동일하다. */
	if (e && (!elevator_tryget(e)))
		e = NULL;
	/* [한국어] 찾기와 참조 획득이 모두 끝났으므로 락 해제. 이 시점 이후
	 * 다른 CPU가 목록을 고쳐도, 우리는 이미 참조를 쥐고 있어 안전하다. */
	spin_unlock(&elv_list_lock);
	/* [한국어] 참조를 쥔 포인터 또는 NULL을 반환한다. 반환값이 NULL이 아니면
	 * 호출자가 elevator_put()으로 반납할 책임을 진다. */
	return e;
}

/* [한국어] elv_ktype의 전방 선언.
 * 정의는 이 파일 아래쪽(elv_sysfs_ops 정의 뒤)에 있는데, elevator_alloc()이
 * kobject_init()에 이 타입을 넘겨야 하므로 여기서 미리 이름만 알린다.
 * kobj_type은 "이 kobject를 sysfs에서 어떻게 다룰 것인가"(show/store 콜백,
 * 참조 0일 때 부를 release 함수)를 정의하는 vtable에 해당한다. */
static const struct kobj_type elv_ktype;

/*
 * [한국어]
 * struct elevator_queue 주요 필드 (정의는 block/elevator.h)
 *   type          : 이 큐가 사용 중인 스케줄러 종류(elevator_type). ops 테이블·
 *                   이름·icq 캐시가 여기 들어 있다. 큐는 이 ops를 통해서만
 *                   삽입/디스패치/병합 정책을 수행한다.
 *   kobj/sysfs_lock: /sys/block/<disk>/queue/iosched/ 디렉터리를 만들기 위한
 *                   kobject와, 그 아래 tunable 파일의 show/store를 직렬화하는
 *                   뮤텍스. NVMe 장치라면 /sys/block/nvme0n1/queue/iosched/.
 *   hash          : request의 "끝 섹터"를 키로 하는 해시. 연속 LBA bio의
 *                   back-merge 후보를 O(1)로 찾는다.
 *   et            : blk_mq_sched가 미리 할당해 둔 스케줄러 태그(sched_tags)
 *                   묶음. 드라이버 태그와는 완전히 별개의 자원이다.
 *   elevator_data : 스케줄러 사설 데이터(mq-deadline의 struct deadline_data,
 *                   bfq의 struct bfq_data 등).
 *   flags         : ELEVATOR_FLAG_REGISTERED(sysfs 등록됨),
 *                   ELEVATOR_FLAG_DYING(해제 진행 중 — sysfs show/store가
 *                   이 비트를 보고 -ENODEV를 돌려준다).
 *
 * elevator_alloc - request_queue에 붙일 elevator_queue를 할당/초기화
 *
 * @q:   이 elevator를 붙일 대상 큐. 할당 노드(q->node)를 얻는 데 쓴다.
 * @e:   붙일 스케줄러 종류. 이 함수가 모듈 참조를 하나 더 잡는다.
 * @res: elevator_change() 경로에서 큐를 얼리기 전에 미리 할당해 둔 자원
 *       (스케줄러 태그 et와 스케줄러 사설 데이터 data). 큐가 얼어 있는
 *       구간에서는 GFP_KERNEL 할당을 할 수 없으므로 미리 준비해 넘긴다.
 * @return: 초기화된 elevator_queue, 메모리 부족이면 NULL.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL을 쓰므로 잠들 수 있다.
 * 호출 시점상 큐는 freeze+quiesce 상태이고 q->elevator_lock을 쥐고 있다.
 *
 * 호출 체인:
 *   elevator_switch → blk_mq_init_sched → 스케줄러의 init_sched 콜백
 *     (dd_init_sched / bfq_init_queue / kyber_init_sched) → [elevator_alloc]
 */
struct elevator_queue *elevator_alloc(struct request_queue *q,
		struct elevator_type *e, struct elevator_resources *res)
{
	/* [한국어] 새로 만들 elevator 인스턴스를 가리킬 지역 포인터. */
	struct elevator_queue *eq;

	/* [한국어] 큐가 붙은 NUMA 노드(q->node)에서 0으로 초기화된 메모리를 얻는다.
	 * kzalloc_node를 쓰는 이유: I/O 제출·완료 경로가 이 구조체(특히 hash)를
	 * 매우 자주 만지므로, 큐를 소유한 노드의 로컬 메모리에 두어야 원격 노드
	 * 접근 지연을 피할 수 있다. 0 초기화 덕분에 flags/last_merge 등을 따로
	 * 클리어할 필요가 없다. */
	eq = kzalloc_node(sizeof(*eq), GFP_KERNEL, q->node);
	/* [한국어] 할당 실패는 드문 경로이므로 unlikely()로 분기 예측을 돕는다.
	 * 실패 시 NULL을 반환하면 호출자 체인이 -ENOMEM으로 이어지고,
	 * elevator_change()는 스케줄러 부착을 포기한 채 "none"으로 남긴다. */
	if (unlikely(!eq))
		/* [한국어] 아직 아무 자원도 잡지 않았으므로 그냥 반환하면 된다. */
		return NULL;

	/* [한국어] 스케줄러 모듈 참조를 하나 올린다(try 없이 무조건 성공하는 버전).
	 * 호출자가 이미 elevator_find_get()으로 참조를 하나 쥐고 있어 모듈이
	 * 사라질 수 없는 상태이기 때문에 tryget이 아니어도 안전하다. 여기서 잡은
	 * 참조는 elevator_release()의 elevator_put()이 짝으로 반납한다. */
	__elevator_get(e);
	/* [한국어] 이 큐 인스턴스가 어느 스케줄러 종류인지 기록. 이후 모든
	 * 콜백 호출(e->type->ops.xxx)이 이 포인터를 거친다. */
	eq->type = e;
	/* [한국어] kobject 참조 카운트를 1로 세팅하고 elv_ktype(sysfs ops +
	 * release 콜백)을 연결한다. 아직 sysfs에 나타나지는 않는다 —
	 * 실제 등록은 나중에 elv_register_queue()의 kobject_add()가 한다.
	 * init과 add를 분리하는 이유: 큐가 얼어 있는 구간에서 자료구조만 먼저
	 * 만들고, 큐를 녹인 뒤에 sysfs 노출을 하기 위해서다. */
	kobject_init(&eq->kobj, &elv_ktype);
	/* [한국어] /sys/.../queue/iosched/ 아래 tunable(read_expire 등)의
	 * show/store를 직렬화할 뮤텍스. sysfs 접근은 프로세스 컨텍스트에서만
	 * 일어나므로 스핀락이 아니라 뮤텍스로 충분하다. */
	mutex_init(&eq->sysfs_lock);
	/* [한국어] back-merge 후보 탐색용 해시 버킷을 모두 빈 상태로 만든다.
	 * 키는 rq_hash_key(rq) = 시작 섹터 + 길이, 즉 request의 "끝 섹터"다. */
	hash_init(eq->hash);
	/* [한국어] 큐를 얼리기 전에 미리 할당해 둔 스케줄러 태그 자원을 넘겨받는다.
	 * 이 태그(sched_tags)는 드라이버 태그와 별개다 — 자세한 구분은
	 * block/blk-mq-sched.c의 설명을 참고. */
	eq->et = res->et;
	/* [한국어] 마찬가지로 미리 할당된 스케줄러 사설 데이터를 연결한다.
	 * mq-deadline이면 struct deadline_data, bfq면 struct bfq_data. */
	eq->elevator_data = res->data;

	/* [한국어] 완성된 인스턴스 반환. 호출자(blk_mq_init_sched 경유)가
	 * q->elevator에 대입한다. */
	return eq;
}

/*
 * [한국어]
 * elevator_release - elevator_queue의 kobject 참조가 0이 됐을 때 메모리 해제
 *
 * @kobj: 해제할 elevator_queue에 내장된 kobject 포인터
 *
 * kobject_put()이 참조 카운트를 0으로 만들면 kobj_type.release로 등록된
 * 이 함수가 호출된다. elevator_queue는 직접 kfree할 수 없고 반드시 kobject
 * 생명주기를 통해 해제해야 하는데, 이 함수가 그 최종 단계이다.
 * elevator_exit() → kobject_del() → kobject_put() → elevator_release() 순.
 *
 * 호출 체인:
 *   elevator_exit/elv_exit_and_release → kobject_put(&e->kobj) → [elevator_release]
 */
static void elevator_release(struct kobject *kobj)
{
	/* [한국어] kobject를 감싸고 있는 바깥 구조체를 가리킬 포인터. */
	struct elevator_queue *e;

	/* [한국어] kobject는 elevator_queue 안에 값으로 박혀 있으므로,
	 * 멤버 주소에서 오프셋을 빼면 바깥 구조체 주소가 나온다.
	 * 커널의 표준 "내장 객체 → 소유자" 역산 관용구다. */
	e = container_of(kobj, struct elevator_queue, kobj);
	/* [한국어] elevator_alloc()의 __elevator_get()과 짝을 이루는 반납.
	 * 이 참조가 마지막이면 mq-deadline/bfq 모듈을 rmmod할 수 있게 된다. */
	elevator_put(e->type);
	/* [한국어] 구조체 메모리 해제. kobject 참조가 0이 된 뒤에만 이 함수가
	 * 불리므로, 이 시점에 이 객체를 보고 있는 다른 주체는 없다. */
	kfree(e);
}

/*
 * [한국어]
 * elevator_exit - request_queue에서 현재 elevator의 동작을 정지시킨다
 *
 * @q: 대상 큐. q->elevator가 반드시 NULL이 아니어야 한다(호출자가 확인).
 * @return: 없음
 *
 * 주의: 이름과 달리 이 함수는 elevator_queue 메모리를 해제하지 않는다.
 * 스케줄러 콜백(exit_sched)을 불러 동작을 멈추게 할 뿐이고, 구조체 자체는
 * 나중에 elevator_change_done()/elv_exit_and_release()가 kobject_put()으로
 * 참조를 떨어뜨릴 때 elevator_release()에서 해제된다. 이렇게 두 단계로
 * 나눈 이유는, 큐가 얼어 있는 구간에서는 sysfs 제거(kobject_del)를 할 수
 * 없기 때문이다 — sysfs 제거는 큐를 녹인 뒤에 해야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 q->elevator_lock을 쥐고 있고,
 * 큐는 freeze(신규 진입 차단) + quiesce(진행 중 디스패치 종료) 상태다.
 * 즉 이 시점에 이 스케줄러를 참조하는 I/O는 하나도 남아 있지 않다.
 *
 * 호출 체인:
 *   elevator_switch / elv_exit_and_release → [elevator_exit]
 *     → ioc_clear_queue → blk_mq_exit_sched → 스케줄러의 exit_sched 콜백
 */
static void elevator_exit(struct request_queue *q)
{
	/* [한국어] 정리 대상 elevator 인스턴스. 호출자가 NULL이 아님을 보장한다. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] q->elevator_lock을 쥔 채 불려야 한다는 계약을 lockdep으로 검증.
	 * 이 락이 q->elevator 포인터 자체와 스케줄러 교체 과정을 보호한다.
	 * CONFIG_LOCKDEP이 꺼져 있으면 컴파일 시 사라진다. */
	lockdep_assert_held(&q->elevator_lock);

	/* [한국어] 이 큐에 붙어 있던 모든 io_context의 icq(io_cq)를 떼어낸다.
	 * icq는 "프로세스 × 큐" 쌍마다 만들어지는 스케줄러 사설 객체(BFQ의
	 * bfq_io_cq 등)라서, 스케줄러가 사라지기 전에 반드시 먼저 없애야 한다.
	 * 순서가 뒤바뀌면 해제된 스케줄러 자료구조를 icq가 가리키게 된다.
	 * 구현은 block/blk-ioc.c. */
	ioc_clear_queue(q);

	/* [한국어] sysfs show/store와의 경쟁을 막기 위해 sysfs_lock을 잡는다.
	 * 다른 CPU가 마침 iosched tunable을 읽는 중일 수 있는데, 그 핸들러가
	 * 곧 해제될 elevator_data를 만지면 use-after-free가 된다.
	 * elv_attr_show/store는 이 락 안에서 ELEVATOR_FLAG_DYING을 확인한다. */
	mutex_lock(&e->sysfs_lock);
	/* [한국어] 스케줄러 종료 본체(block/blk-mq-sched.c). 여기서
	 * ELEVATOR_FLAG_DYING을 세우고, 스케줄러의 exit_sched 콜백(dd_exit_sched
	 * 등)을 부르고, q->elevator를 NULL로 만든다. 이 호출 이후 큐는
	 * 스케줄러가 없는 상태("none")가 된다. */
	blk_mq_exit_sched(q, e);
	/* [한국어] sysfs 접근 재개 허용. 이 시점 이후의 show/store는 DYING
	 * 비트를 보고 -ENODEV를 반환한다. */
	mutex_unlock(&e->sysfs_lock);
}

/*
 * [한국어]
 * __elv_rqhash_del - request를 병합 후보 해시에서 제거한다(무조건 버전)
 *
 * @rq: 해시에서 뺄 request. 반드시 현재 해시에 들어 있어야 한다.
 * @return: 없음
 *
 * 앞의 밑줄 두 개는 "호출 전 조건을 호출자가 보장한다"는 관례다. 여기서는
 * "rq가 실제로 해시에 있다"가 그 조건이며, 확인 책임은 호출자에게 있다
 * (elv_rqhash_del은 ELV_ON_HASH로 확인하고, elv_rqhash_find는 해시를
 *  순회하다 찾은 노드에 대해 부르므로 이미 조건이 참이다).
 *
 * 실행 컨텍스트: 스케줄러 락(dd->lock 등)을 쥔 상태. 해시는 큐 단위 공유
 * 자료구조이므로 보호 없이 만지면 리스트가 깨진다.
 *
 * 호출 체인:
 *   elv_rqhash_del / elv_rqhash_reposition / elv_rqhash_find → [__elv_rqhash_del]
 */
static inline void __elv_rqhash_del(struct request *rq)
{
	/* [한국어] hlist에서 노드를 떼어낸다. 이 request는 이후 back-merge
	 * 후보로 검색되지 않는다 — 이미 드라이버로 내려갔거나, 병합으로
	 * 흡수되었거나, 키가 바뀌어 다시 넣어야 하는 상태이기 때문이다. */
	hash_del(&rq->hash);
	/* [한국어] "이 request는 해시에 들어 있다"는 표식 비트를 끈다.
	 * ELV_ON_HASH()가 이 비트를 보고 중복 제거/중복 삽입을 막는다.
	 * ~로 비트를 지우는 이유: rq_flags에는 다른 상태 비트가 함께 들어
	 * 있으므로 대입이 아니라 해당 비트만 클리어해야 한다. */
	rq->rq_flags &= ~RQF_HASHED;
}

/*
 * [한국어]
 * elv_rqhash_del - request를 병합 후보 해시에서 안전하게 제거
 *
 * @q:  request가 속한 큐 (현재 구현에서는 쓰이지 않지만 API 일관성상 유지)
 * @rq: 해시에서 뺄 request
 * @return: 없음
 *
 * request가 드라이버로 dispatch되거나 병합으로 소멸하면 더 이상 병합 대상이
 * 아니므로 해시에서 빼야 한다. 남겨 두면 이미 드라이버에 넘어간(또는 해제된)
 * request에 새 bio를 붙이려는 시도가 발생한다. NVMe라면 이미 SQ에 실려
 * 컨트롤러가 처리 중인 커맨드의 길이를 사후에 바꾸려는 셈이 되어 치명적이다.
 *
 * ELV_ON_HASH() 검사가 필요한 이유: 이 함수는 여러 경로에서 호출되고, 그중
 * 일부는 해당 request가 애초에 해시에 들어간 적이 없다(스케줄러를 거치지 않은
 * passthrough, 이미 다른 경로에서 제거된 경우). 해시에 없는 노드를
 * hash_del()하면 리스트 포인터가 손상되므로 반드시 확인해야 한다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 상태. 해시는 큐 단위 자료구조라 보호가 필요하다.
 *
 * 호출 체인:
 *   elv_merge_requests / 스케줄러의 dispatch·완료 경로
 *     (dd_finish_request, bfq_finish_requeue_request 등) → [elv_rqhash_del]
 */
void elv_rqhash_del(struct request_queue *q, struct request *rq)
{
	/* [한국어] RQF_HASHED 플래그로 "실제로 해시에 들어 있는지"를 확인한다.
	 * 이 검사 덕분에 호출자가 중복 호출하거나 해시에 없는 request를 넘겨도
	 * 안전하다(멱등성). */
	if (ELV_ON_HASH(rq))
		__elv_rqhash_del(rq);
}
EXPORT_SYMBOL_GPL(elv_rqhash_del);

/*
 * [한국어]
 * elv_rqhash_add - request를 "끝 섹터"를 키로 병합 후보 해시에 넣는다
 *
 * @q:  request가 속한 큐. 해시는 q->elevator 안에 있다.
 * @rq: 해시에 넣을 request. 아직 해시에 없어야 한다.
 * @return: 없음
 *
 * 스케줄러가 request를 자기 큐에 삽입할 때 이 함수를 함께 불러 둔다.
 * 그래야 나중에 도착한 bio가 elv_rqhash_find()로 이 request를 O(1)에 찾아
 * back merge할 수 있다. 해시에 넣지 않으면 병합 기회를 통째로 잃는다.
 *
 * NVMe 관점: 병합이 한 번 성사될 때마다 나중에 발행될 NVMe 커맨드가 하나
 * 줄어든다(= SQ 엔트리 하나, Command ID 하나, 완료 CQ 엔트리 하나 절약).
 * 반대로 한 커맨드가 담는 데이터가 커지므로 그 커맨드의 PRP/SGL 엔트리
 * 수는 오히려 늘어난다 — 줄어드는 것은 커맨드 "개수"이지 서술자 길이가 아니다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 프로세스 컨텍스트(dd_insert_request 등).
 *
 * 호출 체인:
 *   스케줄러의 insert_requests 콜백(dd_insert_request/bfq_insert_request)
 *     또는 elv_rqhash_reposition → [elv_rqhash_add]
 */
void elv_rqhash_add(struct request_queue *q, struct request *rq)
{
	/* [한국어] 해시 테이블을 소유한 elevator 인스턴스. 큐마다 하나씩 있으므로
	 * 서로 다른 디스크(예: nvme0n1과 nvme0n2)의 request가 섞이지 않는다. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] 이미 해시에 들어 있는 request를 또 넣으면 hlist 노드 하나가
	 * 두 버킷에 걸치게 되어 리스트가 영구히 손상된다. 조용히 넘어가면
	 * 나중에 엉뚱한 곳에서 터지므로, 여기서 즉시 멈춰 원인 지점을 남긴다. */
	BUG_ON(ELV_ON_HASH(rq));
	/* [한국어] 키는 rq_hash_key(rq) = blk_rq_pos + blk_rq_sectors,
	 * 즉 이 request가 "끝나는" 섹터다. back merge 조건이
	 * "새 bio의 시작 == 기존 request의 끝"이므로, 끝을 키로 두어야
	 * 새 bio의 시작 섹터 하나로 곧장 조회할 수 있다. */
	hash_add(e->hash, &rq->hash, rq_hash_key(rq));
	/* [한국어] "해시에 들어 있음" 표식. ELV_ON_HASH()가 이 비트를 읽어
	 * 중복 삽입(위 BUG_ON)과 중복 제거를 모두 막는다.
	 * |=로 다른 상태 비트를 보존하며 이 비트만 켠다. */
	rq->rq_flags |= RQF_HASHED;
}
EXPORT_SYMBOL_GPL(elv_rqhash_add);

/*
 * [한국어]
 * elv_rqhash_reposition - request의 길이가 바뀐 뒤 해시 위치를 갱신한다
 *
 * @q:  request가 속한 큐
 * @rq: 길이(blk_rq_sectors)가 바뀐 request
 * @return: 없음
 *
 * 해시 키가 "시작 섹터 + 길이"이므로, back merge로 request가 길어지면 키가
 * 달라진다. 그런데 해시 자료구조는 키가 바뀌었다고 알아서 옮겨 주지 않는다.
 * 그대로 두면 이 request는 옛 키의 버킷에 남아, 새로 맞닿는 bio가 와도
 * 검색되지 않는다 — 즉 연속된 순차 I/O에서 두 번째 병합부터 실패한다.
 * 그래서 제거 후 재삽입이라는 가장 단순한 방법으로 위치를 다시 잡는다.
 *
 * front merge(앞쪽으로 늘어남)에서는 시작 섹터가 앞으로 당겨지고 길이가
 * 그만큼 늘어 끝 섹터는 그대로이므로 키가 변하지 않는다. 그래서
 * elv_merged_request()는 ELEVATOR_BACK_MERGE일 때만 이 함수를 부른다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   elv_merged_request / elv_merge_requests → [elv_rqhash_reposition]
 *     → __elv_rqhash_del → elv_rqhash_add
 */
void elv_rqhash_reposition(struct request_queue *q, struct request *rq)
{
	/* [한국어] 옛 키 위치에서 제거. 여기서는 rq가 해시에 있음이 보장되므로
	 * ELV_ON_HASH 검사 없는 __ 버전을 쓴다. */
	__elv_rqhash_del(rq);
	/* [한국어] 새로 계산된 rq_hash_key로 다시 삽입. 위에서 RQF_HASHED가
	 * 꺼졌기 때문에 elv_rqhash_add 안의 BUG_ON에 걸리지 않는다.
	 * 이 두 줄 사이에는 스케줄러 락이 계속 잡혀 있어야 한다 — 중간에
	 * 다른 CPU가 이 request를 검색하면 "존재하지 않는" 순간을 보게 된다. */
	elv_rqhash_add(q, rq);
}

/*
 * [한국어]
 * elv_rqhash_find - 끝 섹터가 @offset인 back merge 후보 request를 찾는다
 *
 * @q:      탐색할 큐. 해시는 q->elevator->hash.
 * @offset: 새로 들어온 bio(또는 request)의 시작 섹터.
 * @return: 끝 섹터가 정확히 @offset인 병합 가능한 request, 없으면 NULL.
 *
 * 이 함수 하나가 순차 워크로드의 병합 성능을 좌우한다. 해시 조회 한 번으로
 * "바로 앞에서 끝나는 request"를 O(1)에 찾아내기 때문이다. front merge는
 * 방향이 반대라 이 해시로는 찾을 수 없고, 실패 시 호출자가 스케줄러의
 * request_merge 콜백(정렬 RB-tree 이분 탐색)으로 폴백한다.
 *
 * 부수 효과: 순회 도중 더 이상 병합 불가능해진 request를 발견하면 그 자리에서
 * 해시에서 빼 버린다(지연 청소). 그래서 순수 조회 함수처럼 보이지만 실제로는
 * 자료구조를 수정하며, 반드시 스케줄러 락 안에서 불려야 한다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 프로세스 컨텍스트(bio 제출 경로).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → elv_merge → [elv_rqhash_find]
 *   elv_attempt_insert_merge → [elv_rqhash_find]
 */
struct request *elv_rqhash_find(struct request_queue *q, sector_t offset)
{
	/* [한국어] 해시 테이블은 스케줄러 인스턴스(elevator_queue)마다 하나씩 있다.
	 * 큐마다 독립적이므로 다른 디스크(예: nvme0n1과 nvme0n2는 각자 별도의
	 * request_queue를 가진다)의 request가 섞이지 않는다. */
	struct elevator_queue *e = q->elevator;
	/* [한국어] _safe 순회에 필요한 다음 노드 보관용. 순회 도중 현재 노드를
	 * 삭제할 수 있기 때문에 반드시 필요하다(아래 __elv_rqhash_del 참고). */
	struct hlist_node *next;
	/* [한국어] 순회 커서. 매 반복마다 버킷 안의 hlist 노드에서
	 * container_of로 역산된 request가 들어온다. */
	struct request *rq;

	/* [한국어] offset을 키로 같은 버킷에 있는 request들을 순회한다.
	 * 해시 충돌 때문에 키가 다른 request도 같은 버킷에 있을 수 있어,
	 * 아래에서 rq_hash_key를 한 번 더 비교해야 한다.
	 * _safe 변형인 이유: 루프 안에서 __elv_rqhash_del()로 현재 노드를
	 * 제거하는데, 일반 순회 매크로는 제거된 노드의 next 포인터를 읽어
	 * use-after-free를 일으킨다. */
	hash_for_each_possible_safe(e->hash, rq, next, hash, offset) {
		/* [한국어] 해시 버킷에 들어 있는데 RQF_HASHED가 없다면 자료구조가
		 * 손상된 것이다. 조용히 넘어가면 이후 병합에서 잘못된 request를
		 * 건드리므로 즉시 커널을 멈춰 디버깅을 강제한다. */
		BUG_ON(!ELV_ON_HASH(rq));

		/* [한국어] 이 request가 더 이상 병합 가능한 상태가 아니라면(REQ_NOMERGE가
		 * 붙었거나 이미 dispatch되었거나 FLUSH 계열이면) 해시에 남아 있을
		 * 이유가 없다. 찾는 김에 청소하는 지연 제거(lazy deletion) 전략으로,
		 * 별도의 청소 순회를 돌 필요를 없앤다.
		 * continue로 같은 버킷의 다음 후보를 계속 본다. */
		if (unlikely(!rq_mergeable(rq))) {
			__elv_rqhash_del(rq);
			continue;
		}

		/* [한국어] 해시 충돌을 걸러내는 최종 확인. rq_hash_key(rq)는 이
		 * request가 끝나는 섹터이고, offset은 새 bio가 시작하는 섹터다.
		 * 둘이 같다는 것은 두 영역이 정확히 맞닿는다는 뜻이므로 back merge
		 * 후보가 확정된다. 호출자 elv_merge()가 이어서 blk_try_merge()로
		 * 방향을 확정하고 실제 병합을 수행한다. */
		if (rq_hash_key(rq) == offset)
			return rq;
	}

	/* [한국어] 인접한 request가 없다 — 이 bio는 새 request가 되거나, 호출자가
	 * 스케줄러의 request_merge 콜백으로 front merge까지 찾아본다. */
	return NULL;
}

/*
 * RB-tree support functions for inserting/lookup/removal of requests
 * in a sorted RB tree.
 */
/*
 * [한국어]
 * elv_rb_add - request를 시작 LBA 기준으로 정렬된 RB-tree에 삽입
 *
 * @root: 삽입 대상 RB-tree 루트. 스케줄러가 자기 자료구조 안에 갖고 있다
 *        (mq-deadline의 dd_per_prio.sort_list[], BFQ의 bfq_queue.sort_list).
 * @rq:   삽입할 request. blk_rq_pos(rq)(시작 섹터)를 정렬 키로 쓴다.
 * @return: 없음
 *
 * === 왜 LBA로 정렬된 트리가 필요한가 ===
 * 해시(elv_rqhash_find)는 "정확히 맞닿는" back merge 후보만 O(1)에 찾을 수
 * 있다. 그런데 스케줄러는 두 가지를 더 해야 한다:
 *   1) front merge 후보 찾기 — "이 bio 바로 뒤에서 시작하는 request"를
 *      찾으려면 정렬된 구조에서 이분 탐색을 해야 한다.
 *   2) dispatch 순서 정하기 — 다음에 내보낼 request를 LBA 순으로 고르면
 *      장치 입장에서 접근이 순차적으로 보인다.
 * 이 두 용도를 위해 스케줄러는 해시와 별개로 RB-tree를 유지한다.
 *
 * === NVMe에서도 LBA 정렬이 의미가 있는가 ===
 * 회전 디스크만큼 극적이지는 않지만 여전히 이득이 있다. SSD 내부의 FTL은
 * 논리적으로 인접한 LBA를 같은 물리 블록/채널에 배치하는 경향이 있어,
 * 순차 접근이 읽기 증폭과 GC 부담을 줄인다. 또 LBA 순으로 나가면 블록 계층의
 * 병합 기회 자체가 늘어나 커맨드 수가 줄어든다.
 * 다만 이 이득이 스케줄러의 락 경합 비용보다 작기 때문에, 멀티큐 NVMe의
 * 기본값은 여전히 "none"이다(elevator_set_default 참고).
 *
 * === 중복 키 처리 ===
 * 같은 시작 LBA를 가진 request가 이미 있으면 오른쪽 서브트리로 내려간다.
 * 즉 중복을 허용하며, 같은 키끼리는 나중에 삽입된 것이 뒤에 온다. 두 요청이
 * 같은 LBA에서 시작하는 상황은 서로 다른 길이의 겹치는 I/O일 때 발생한다.
 *
 * 실행 컨텍스트: 스케줄러 락(dd->lock, bfqd->lock)을 쥔 상태.
 *
 * 호출 체인:
 *   dd_insert_request / bfq_add_request → [elv_rb_add]
 *     → rb_link_node → rb_insert_color(레드-블랙 균형 복원)
 */
void elv_rb_add(struct rb_root *root, struct request *rq)
{
	/* [한국어] p는 "새 노드를 매달 위치를 가리키는 포인터의 포인터"다.
	 * 이중 포인터를 쓰는 이유: 트리가 비었을 때는 root->rb_node에, 아니면
	 * 부모의 rb_left 또는 rb_right에 써야 하는데, 이중 포인터로 두면
	 * 세 경우를 분기 없이 동일하게 처리할 수 있다. */
	struct rb_node **p = &root->rb_node;
	/* [한국어] 새 노드의 부모가 될 노드. 루프가 끝나면 마지막으로 들른 노드가
	 * 담긴다. 트리가 비어 있으면 NULL로 남아 새 노드가 루트가 된다. */
	struct rb_node *parent = NULL;
	/* [한국어] 순회 중 비교 대상 request. 이름 앞의 밑줄은 인자 rq와
	 * 구분하기 위한 관례다. */
	struct request *__rq;

	/* [한국어] 루트에서 시작해 리프까지 내려가며 삽입 위치를 찾는다.
	 * *p가 NULL이 되는 지점이 빈 자리다. 평균 O(log n). */
	while (*p) {
		/* [한국어] 현재 노드를 부모 후보로 기억한다. */
		parent = *p;
		/* [한국어] rb_node 포인터에서 그것을 품고 있는 request 구조체 주소를
		 * 역산한다(container_of 매크로 기반). 커널의 내장형 자료구조 관용구로,
		 * 별도의 노드 객체를 할당하지 않아 메모리와 캐시 미스를 아낀다. */
		__rq = rb_entry(parent, struct request, rb_node);

		/* [한국어] 새 request의 시작 LBA가 더 작으면 왼쪽 서브트리로 내려간다.
		 * 그 결과 트리를 중위 순회하면 LBA 오름차순이 된다. */
		if (blk_rq_pos(rq) < blk_rq_pos(__rq))
			p = &(*p)->rb_left;
		/* [한국어] 크거나 같으면 오른쪽. >= 이므로 중복 키는 뒤쪽에 쌓인다.
		 * (else if로 쓰였지만 앞 조건의 부정과 같아 항상 참이다 — 명시적으로
		 *  조건을 적어 의도를 드러내는 스타일이다.) */
		else if (blk_rq_pos(rq) >= blk_rq_pos(__rq))
			p = &(*p)->rb_right;
	}

	/* [한국어] 찾은 빈 자리에 새 노드를 물리적으로 연결한다. 부모 포인터를
	 * 설정하고, *p에 새 노드 주소를 써 넣는다. 이 시점의 트리는 연결은
	 * 되었지만 레드-블랙 속성이 깨져 있을 수 있다. */
	rb_link_node(&rq->rb_node, parent, p);
	/* [한국어] 색칠과 회전으로 레드-블랙 속성을 복원해 트리 높이를 O(log n)로
	 * 유지한다. 이 두 단계(연결 → 균형)를 분리하는 것이 커널 RB-tree API의
	 * 표준 사용법이다. */
	rb_insert_color(&rq->rb_node, root);
}
EXPORT_SYMBOL(elv_rb_add);

/*
 * [한국어]
 * elv_rb_del - LBA 기준 RB-tree에서 request를 제거
 *
 * @root: 스케줄러 내부의 RB-tree 루트
 * @rq:   제거할 request
 *
 * dispatch된 request 또는 병합으로 사라지는 request를 스케줄러 큐에서 제거.
 * RB_EMPTY_NODE: 이미 제거된 노드를 다시 제거하면 커널 패닉이 일어나므로
 * BUG_ON으로 사전 방어한다. rb_erase 후 RB_CLEAR_NODE로 "트리 밖" 상태 명시.
 *
 * 호출 체인:
 *   스케줄러 ops.dispatch_request / ops.requests_merged → [elv_rb_del]
 */
void elv_rb_del(struct rb_root *root, struct request *rq)
{
	/* [한국어] RB_CLEAR_NODE로 표시된 "트리 밖" 노드를 다시 지우려는 시도를
	 * 잡는다. rb_erase는 노드가 트리 안에 있다고 가정하고 부모/자식 포인터를
	 * 따라가므로, 트리 밖 노드를 넘기면 엉뚱한 메모리를 트리 구조로 해석해
	 * 트리 전체를 망가뜨린다. 그래서 조용히 무시하지 않고 즉시 멈춘다. */
	BUG_ON(RB_EMPTY_NODE(&rq->rb_node));
	/* [한국어] 노드를 떼어내고 레드-블랙 속성을 복원한다(필요하면 색 변경과
	 * 회전을 수행). O(log n). */
	rb_erase(&rq->rb_node, root);
	/* [한국어] 노드를 "어느 트리에도 속하지 않음" 상태로 표시한다.
	 * 이 표시가 있어야 위의 BUG_ON이 중복 제거를 탐지할 수 있고,
	 * 스케줄러가 RB_EMPTY_NODE()로 "이 request가 아직 정렬 트리에 있는가"를
	 * 물어볼 수 있다(mq-deadline의 deadline_remove_request 등). */
	RB_CLEAR_NODE(&rq->rb_node);
}
EXPORT_SYMBOL(elv_rb_del);

/*
 * [한국어]
 * elv_rb_find - 특정 @sector(LBA)로 시작하는 request를 RB-tree에서 탐색
 *
 * @root:   스케줄러 내부의 RB-tree 루트
 * @sector: 찾을 LBA(논리 블록 주소)
 * @return: 정확히 @sector에서 시작하는 request; 없으면 NULL
 *
 * BFQ·mq-deadline이 front-merge 후보(뒤쪽이 @sector인 request) 탐색,
 * 또는 dispatch 순서 결정에 O(log N) 탐색으로 사용한다.
 * 주의: back-merge 후보는 끝 섹터 기준 해시(elv_rqhash_find)로 찾는다.
 *
 * 호출 체인:
 *   스케줄러 ops.request_merge → [elv_rb_find]
 */
struct request *elv_rb_find(struct rb_root *root, sector_t sector)
{
	/* [한국어] 탐색 커서를 루트에 놓는다. 트리가 비어 있으면 곧바로 NULL이라
	 * 아래 while이 한 번도 돌지 않는다. */
	struct rb_node *n = root->rb_node;
	/* [한국어] 현재 노드에서 역산한 request를 담을 지역 변수. */
	struct request *rq;

	/* [한국어] 표준 이진 탐색 루프. 트리 높이가 O(log n)으로 유지되므로
	 * 비교 횟수도 O(log n)이다. */
	while (n) {
		/* [한국어] rb_node 멤버 주소에서 그것을 품은 request 주소를 역산.
		 * request 안에 rb_node가 값으로 박혀 있어 가능한 관용구다. */
		rq = rb_entry(n, struct request, rb_node);

		/* [한국어] 찾는 LBA가 현재 노드보다 작으면 더 작은 값들이 모인
		 * 왼쪽 서브트리로 내려간다. */
		if (sector < blk_rq_pos(rq))
			/* [한국어] 커서를 왼쪽 자식으로 이동. */
			n = n->rb_left;
		/* [한국어] 크면 오른쪽 서브트리로 내려간다. */
		else if (sector > blk_rq_pos(rq))
			/* [한국어] 커서를 오른쪽 자식으로 이동. */
			n = n->rb_right;
		/* [한국어] 두 조건 모두 거짓 = 정확히 일치. */
		else
			/* [한국어] 시작 LBA가 @sector인 request를 찾았다.
			 * 호출자(스케줄러의 request_merge 콜백)는 이 request 앞에
			 * bio를 붙이는 front merge를 시도한다. */
			return rq;
	}

	/* [한국어] 해당 LBA에서 시작하는 request가 없다 — front merge 후보 없음.
	 * 호출자는 ELEVATOR_NO_MERGE를 반환해 새 request를 만들게 된다. */
	return NULL;
}
EXPORT_SYMBOL(elv_rb_find);

/*
 * [한국어]
 * elv_merge - bio를 기존 request에 병합할 수 있는지 판정하고 후보를 돌려준다
 *
 * @q:   대상 큐. 스케줄러가 붙어 있는 상태에서만 불린다.
 * @req: [출력] 병합 후보 request를 여기에 써 준다. 반환값이 NO_MERGE가
 *       아닐 때만 유효하다.
 * @bio: 새로 도착한 bio.
 * @return: ELEVATOR_NO_MERGE(병합 불가) / ELEVATOR_BACK_MERGE(뒤에 붙임) /
 *          ELEVATOR_FRONT_MERGE(앞에 붙임) / ELEVATOR_DISCARD_MERGE
 *          (discard 요청끼리 하나로 묶음).
 *
 * === 3단계 탐색 전략 ===
 * 싼 것부터 순서대로 시도해 대부분을 첫 단계에서 끝낸다.
 *   1) one-hit 캐시(q->last_merge): 포인터 하나 비교. 순차 I/O에서 적중률이
 *      매우 높다.
 *   2) 해시(elv_rqhash_find): 끝 섹터 키로 back merge 후보를 O(1) 조회.
 *   3) 스케줄러 콜백(ops.request_merge): 정렬 RB-tree에서 front merge까지 탐색.
 *      O(log n)이고 스케줄러 자료구조를 만지므로 가장 비싸다.
 * 큐 플래그로 이 단계를 잘라낼 수 있다: nomerges면 아예 시도하지 않고,
 * noxmerges면 1단계까지만 한다.
 *
 * 판정만 하고 실제 데이터 연결은 하지 않는다는 점이 중요하다. 실제 병합은
 * 호출자(blk_mq_sched_try_merge)가 bio_attempt_back/front_merge로 수행한다.
 *
 * NVMe 관점: 병합이 성사되면 나중에 발행될 NVMe 커맨드가 하나 줄어든다.
 * 다만 멀티큐 NVMe의 기본값은 스케줄러 없음("none")이라 이 함수 자체가
 * 호출되지 않고, blk_attempt_plug_merge()가 병합을 담당하는 경우가 흔하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출). 호출자가 스케줄러 락을 쥔다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_sched_bio_merge → blk_mq_sched_try_merge
 *     → [elv_merge] → elv_rqhash_find / e->type->ops.request_merge
 */
enum elv_merge elv_merge(struct request_queue *q, struct request **req,
		struct bio *bio)
{
	/* [한국어] 3단계에서 request_merge 콜백을 부르기 위해 스케줄러 인스턴스를
	 * 미리 잡아 둔다. 이 함수는 스케줄러가 있는 경로에서만 불리므로 NULL이 아니다. */
	struct elevator_queue *e = q->elevator;
	/* [한국어] 해시에서 찾은 back merge 후보를 담을 임시 포인터. */
	struct request *__rq;

	/*
	 * Levels of merges:
	 * 	nomerges:  No merges at all attempted
	 * 	noxmerges: Only simple one-hit cache try
	 * 	merges:	   All merge tries attempted
	 */
	/* [한국어] 두 가지 조기 탈출:
	 *   1) QUEUE_FLAG_NOMERGES — 관리자가
	 *      /sys/block/<disk>/queue/nomerges 에 2를 써서 병합을 완전히 끈 경우.
	 *      병합 탐색 비용조차 아까운 초저지연 구성에서 쓴다.
	 *   2) !bio_mergeable(bio) — bio에 REQ_NOMERGE_FLAGS(FLUSH/FUA 등)가 붙어
	 *      다른 요청과 합치면 순서 보장이 깨지는 경우.
	 * 어느 쪽이든 이 bio는 자기만의 request가 되어 그대로 내려간다. */
	if (blk_queue_nomerges(q) || !bio_mergeable(bio))
		/* [한국어] 병합 후보 없음. *req는 건드리지 않는다. */
		return ELEVATOR_NO_MERGE;

	/*
	 * First try one-hit cache.
	 */
	/* [한국어] 1단계 — one-hit 캐시. q->last_merge가 비어 있지 않고(아직 아무
	 * 병합도 없었으면 NULL), 그 request와 이 bio가 안전·정책상 합쳐질 수
	 * 있는지 확인한다. 단락 평가로 NULL 역참조를 막는다. */
	if (q->last_merge && elv_bio_merge_ok(q->last_merge, bio)) {
		/* [한국어] one-hit 캐시 적중. q->last_merge는 "가장 최근에 병합이
		 * 성공한 request" 하나만 기억하는 극단적으로 단순한 캐시다.
		 * 이것만으로도 효과가 큰 이유: 순차 I/O에서는 같은 request에 연달아
		 * 붙는 경우가 압도적이라, 해시 조회조차 하지 않고 포인터 비교 한 번으로
		 * 끝나는 경우가 대부분이다. 실패하면 아래 해시 조회로 넘어간다. */
		/* [한국어] 두 영역의 위치 관계를 보고 방향을 판정한다(block/blk-merge.c).
		 * bio 시작 == rq 끝이면 BACK, bio 끝 == rq 시작이면 FRONT,
		 * 둘 다 discard이고 병합 가능하면 DISCARD, 아니면 NO_MERGE. */
		enum elv_merge ret = blk_try_merge(q->last_merge, bio);

		/* [한국어] 방향이 정해졌다면 캐시 적중이다 — 해시도 스케줄러 콜백도
		 * 건너뛴다. 순차 워크로드에서 이 빠른 경로의 비중이 가장 크다. */
		if (ret != ELEVATOR_NO_MERGE) {
			/* [한국어] 호출자에게 병합 대상 request를 알려준다. */
			*req = q->last_merge;
			/* [한국어] 판정된 방향을 그대로 반환. 실제 bio 연결은 호출자가 한다. */
			return ret;
		}
		/* [한국어] 캐시 미스면 아래 해시 조회로 이어진다(여기서 return하지 않음). */
	}

	/* [한국어] QUEUE_FLAG_NOXMERGES — nomerges에 1을 쓴 상태.
	 * "복잡한(extended) 병합 탐색만 끄고 one-hit 캐시는 유지"하는 중간 설정이다.
	 * 랜덤 워크로드에서는 해시·트리를 뒤져 봐야 거의 실패하므로, 그 탐색
	 * 비용만 제거하고 싼 캐시 적중은 계속 챙기겠다는 절충이다. */
	if (blk_queue_noxmerges(q))
		/* [한국어] 2·3단계를 생략하고 병합 없음으로 끝낸다. */
		return ELEVATOR_NO_MERGE;

	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	/* [한국어] 2단계 — 해시 조회. bio의 시작 섹터를 키로, 정확히 그 지점에서
	 * 끝나는 request(= back merge 상대)를 O(1)에 찾는다. */
	__rq = elv_rqhash_find(q, bio->bi_iter.bi_sector);
	/* [한국어] 후보를 찾았고(첫 조건), 안전·정책 검사도 통과하면(둘째 조건)
	 * 병합 확정이다. elv_rqhash_find는 위치만 보고 판정하므로, 연산 종류·
	 * cgroup·PI 같은 호환성 검사를 여기서 따로 해야 한다. */
	if (__rq && elv_bio_merge_ok(__rq, bio)) {
		/* [한국어] 호출자에게 병합 대상을 알려준다. */
		*req = __rq;

		/* [한국어] 후보가 discard 요청이고 discard 병합이 허용되는 큐라면
		 * 일반 back merge와 다른 처리가 필요하다. 데이터 전송이 없는
		 * discard는 "연속하지 않은 여러 구간"을 한 요청에 담을 수 있기
		 * 때문이다(request 안에 bio 여러 개가 나란히 매달린다).
		 * NVMe 관점: 이렇게 묶인 request는 나중에 nvme_setup_discard()에서
		 * __rq_for_each_bio()로 순회되어, Dataset Management(DSM) 커맨드
		 * 하나에 bio마다 하나씩 nvme_dsm_range 엔트리로 들어간다
		 * (drivers/nvme/host/core.c). 즉 DSM 커맨드 개수가 줄어든다. */
		if (blk_discard_mergable(__rq))
			/* [한국어] 데이터 연결이 아니라 bio를 request에 덧붙이는
			 * 별도 경로를 쓰라고 호출자에게 알린다. */
			return ELEVATOR_DISCARD_MERGE;
		/* [한국어] 일반적인 back merge — bio를 request 뒤에 이어 붙인다. */
		return ELEVATOR_BACK_MERGE;
	}

	/* [한국어] 3단계 — 스케줄러 콜백. 여기까지 왔다는 것은 back merge 후보가
	 * 없다는 뜻이므로, 해시로는 찾을 수 없는 front merge를 스케줄러의 정렬
	 * RB-tree에서 찾아본다. 구현 여부는 스케줄러마다 다르다:
	 *   mq-deadline - dd_request_merge(). sort_list[] rb-tree에서
	 *                 elv_rb_find(bio 끝 섹터)로 front 후보를 찾는다.
	 *   bfq         - bfq_request_merge(). 같은 방식.
	 *   kyber       - 구현하지 않음. 도메인별 토큰 제어에만 집중하고
	 *                 정렬 트리를 유지하지 않으므로 front merge를 못 한다. */
	if (e->type->ops.request_merge)
		/* [한국어] 콜백이 직접 *req를 채우고 방향을 반환한다. */
		return e->type->ops.request_merge(q, req, bio);

	/* [한국어] 세 단계 모두 실패 — 이 bio는 새로운 request가 된다.
	 * NVMe라면 결국 별도의 커맨드 하나로 발행된다. */
	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to do an insertion back merge. Only check for the case where
 * we can append 'rq' to an existing request, so we can throw 'rq' away
 * afterwards.
 *
 * Returns true if we merged, false otherwise. 'free' will contain all
 * requests that need to be freed.
 */
/*
 * [한국어]
 * elv_attempt_insert_merge - 스케줄러에 넣기 직전의 request를 기존 request에 흡수시킨다
 *
 * @q:    대상 큐
 * @rq:   방금 만들어져 스케줄러에 삽입하려는 request
 * @free: [출력] 병합으로 소멸해 해제해야 할 request들을 매달 리스트.
 *        여기서 직접 해제하지 않고 호출자에게 넘기는 이유는, 이 함수가
 *        스케줄러 락을 쥔 상태에서 불리기 때문이다. 락 안에서 request를
 *        해제하면 태그 반납 경로와 락 순서가 꼬일 수 있다.
 * @return: true = @rq가 흡수되어 사라졌다(호출자는 삽입하면 안 된다),
 *          false = 병합 실패, @rq를 그대로 스케줄러에 넣어야 한다.
 *
 * elv_merge()와의 차이: elv_merge()는 "bio를 request에" 붙이는 판정이고,
 * 이 함수는 "request를 request에" 통째로 붙이는 시도다. bio 단계에서 놓친
 * 병합 기회를 삽입 직전에 한 번 더 잡는 마지막 관문이다.
 *
 * back merge만 시도하는 이유: @rq를 없앨 수 있는 방향이어야 이득이 명확하다.
 * front merge를 하면 기존 request가 사라지고 @rq가 남는데, 그러면 이미
 * 스케줄러 자료구조에 등록된 기존 request를 빼내는 추가 작업이 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 스케줄러 락을 쥔 상태
 * (dd_insert_request가 dd->lock을 쥐고 부른다).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → 스케줄러 insert_requests 콜백(dd_insert_request 등)
 *     → [elv_attempt_insert_merge] → elv_rqhash_find → blk_attempt_req_merge
 */
bool elv_attempt_insert_merge(struct request_queue *q, struct request *rq,
			      struct list_head *free)
{
	/* [한국어] 해시에서 찾아낸 병합 상대. 루프마다 새로 갱신된다. */
	struct request *__rq;
	/* [한국어] 한 번이라도 병합이 성공했는지. 호출자는 이 값으로 rq가
	 * 살아 있는지(false) 소멸했는지(true) 판단한다. */
	bool ret;

	/* [한국어] 관리자가 /sys/block/<dev>/queue/nomerges에 2를 써서 병합을 전면
	 * 금지한 경우. 병합 판정 자체가 CPU를 쓰므로, 랜덤 워크로드처럼 병합이
	 * 절대 성공하지 않는 상황에서는 끄는 편이 빠르다. */
	if (blk_queue_nomerges(q))
		return false;

	/*
	 * First try one-hit cache.
	 */
	/* [한국어] 가장 최근에 병합에 성공한 request와 먼저 시도한다. 순차 I/O에서는
	 * 이 한 번의 시도로 끝나는 경우가 대부분이라, 해시 조회 비용조차 아낀다.
	 * 성공하면 rq는 last_merge에 흡수되어 사라지므로 free 목록에 넣는다. */
	if (q->last_merge && blk_attempt_req_merge(q, q->last_merge, rq)) {
		/* [한국어] 소멸한 rq를 호출자가 나중에 해제하도록 free 리스트에 넣는다.
		 * 여기서 직접 해제하지 않는 이유는 호출자가 스케줄러 락을 쥔 상태라,
		 * 락을 푼 뒤에 해제해야 락 순서 역전을 피할 수 있기 때문이다. */
		list_add(&rq->queuelist, free);
		return true;
	}

	/* [한국어] nomerges에 1을 쓰면 noxmerges — "단순 캐시만 시도하고 복잡한
	 * 탐색은 하지 말라"는 중간 설정이다. 캐시는 이미 위에서 시도했으므로
	 * 여기서 종료한다. 해시 조회와 반복 병합의 CPU 비용을 아끼면서 가장
	 * 쉬운 병합 기회는 놓치지 않는 절충안이다. */
	if (blk_queue_noxmerges(q))
		return false;

	/* [한국어] "아직 한 번도 병합하지 못했다"로 초기화. 아래 루프가 한 번이라도
	 * 성공하면 true가 되고, 그 값이 그대로 반환되어 호출자에게 "@rq는 소멸했다"를
	 * 알린다. 루프 안에서 rq 포인터가 바뀌기 때문에, 성공 여부를 별도 변수로
	 * 기억해 두어야 한다. */
	ret = false;
	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	/* [한국어] ★ 연쇄 병합 루프 ★
	 * 한 번 병합에 성공하면 결과 request가 더 커지고, 그 커진 request가
	 * 또 다른 이웃과 맞닿을 수 있다. 예를 들어 [200~300]이 들어왔을 때
	 * [100~200]에 흡수되어 [100~300]이 되면, 이제 [300~400]과도 인접해진다.
	 * 더 이상 병합할 것이 없을 때까지 반복해 최대한 큰 request를 만든다.
	 * NVMe 관점에서 이 루프가 한 번 돌 때마다 나중에 발행될 NVMe 커맨드가 하나씩
	 * 줄어든다(= SQ 엔트리 하나, Command ID 하나, 완료 CQ 엔트리 하나 절약).
	 * 단, 남는 커맨드 하나가 담는 데이터가 커지므로 그 커맨드의 PRP/SGL
	 * 엔트리 수는 오히려 늘고, MDTS(max_hw_sectors) 상한에 걸리면 더 이상
	 * 커지지 못한다 — 그 상한 검사는 blk_attempt_req_merge() 내부에서 한다. */
	while (1) {
		/* [한국어] 현재 rq의 "시작 섹터"로 해시를 조회한다. 해시는 request의
		 * "끝 섹터"를 키로 하므로, 이 조회는 "rq 바로 앞에서 끝나는 request"를
		 * 찾는 것이다. 즉 rq를 그 뒤에 붙이는 back merge 후보를 찾는다. */
		__rq = elv_rqhash_find(q, blk_rq_pos(rq));
		/* [한국어] 후보가 없거나 실제 병합에 실패하면(하드웨어 한계 등) 종료한다.
		 * blk_attempt_req_merge()가 blk-merge.c의 attempt_merge()를 불러
		 * 세그먼트 수·크기·virt boundary를 모두 검사한다. */
		if (!__rq || !blk_attempt_req_merge(q, __rq, rq))
			break;

		/* [한국어] rq가 __rq에 흡수되어 소멸했다. 해제 목록에 넣는다. */
		list_add(&rq->queuelist, free);
		/* The merged request could be merged with others, try again */
		/* [한국어] 최소 한 번 병합했음을 기록. 이후 루프에서 실패해도 이
		 * 값은 유지되어 호출자에게 "원래 rq는 사라졌다"고 알린다. */
		ret = true;
		/* [한국어] 이제 커진 __rq를 새로운 기준으로 삼아 다시 시도한다.
		 * 다음 반복에서는 __rq의 시작 섹터로 조회하므로, 더 앞쪽으로
		 * 이어지는 request를 찾게 된다. */
		rq = __rq;
	}

	/* [한국어] true = 원래 rq는 병합되어 사라졌다(free 목록에 있음).
	 * false = 병합 실패, rq는 그대로 살아 있으니 호출자가 큐에 삽입해야 한다. */
	return ret;
}

/*
 * [한국어]
 * elv_merged_request - bio 병합 직후 스케줄러 자료구조와 캐시를 일관되게 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   bio를 흡수해 커진 request
 * @type: 병합 방향(ELEVATOR_BACK_MERGE / ELEVATOR_FRONT_MERGE 등)
 * @return: 없음
 *
 * request의 위치나 크기가 바뀌면 그것을 색인하고 있던 세 자료구조가 모두
 * 낡은 상태가 된다. 이 함수가 그 셋을 한꺼번에 맞춘다:
 *   1) 스케줄러 내부 색인 — request_merged 콜백(mq-deadline이면 정렬 rb-tree의
 *      키가 바뀌었으므로 재삽입)
 *   2) 병합 후보 해시 — back merge면 끝 섹터가 바뀌었으므로 재배치
 *   3) one-hit 캐시 — 방금 병합에 성공한 request를 캐시에 올려 다음 시도를 가속
 *
 * === 왜 back merge에서만 해시를 재배치하는가 ===
 * 해시 키는 rq_hash_key(rq) = 시작 섹터 + 길이 = "끝 섹터"다.
 *   back merge  : 뒤에 붙였으므로 끝 섹터가 바뀐다 → 재배치 필요
 *   front merge : 앞에 붙였으므로 시작 섹터와 길이가 함께 바뀌지만, 끝 섹터는
 *                 그대로다 → 해시 키가 변하지 않아 재배치 불필요
 * 이 비대칭은 "끝 섹터를 키로 삼은" 설계에서 자연스럽게 따라 나온다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 상태(호출자가 이미 잡고 있다).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → [elv_merged_request]
 *     → ops.request_merged (dd_request_merged / bfq_request_merged)
 *     → elv_rqhash_reposition
 */
void elv_merged_request(struct request_queue *q, struct request *rq,
		enum elv_merge type)
{
	/* [한국어] 이 큐의 스케줄러 인스턴스. 콜백 테이블 접근용. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] 스케줄러에게 "이 request가 커졌다"고 알린다. 콜백은 선택 사항이며,
	 * mq-deadline은 dd_request_merged()에서 정렬 rb-tree에 다시 넣는다
	 * (시작 LBA가 바뀌는 front merge에서 특히 중요하다). 콜백이 없는
	 * 스케줄러는 위치 색인을 유지하지 않으므로 할 일이 없다. */
	if (e->type->ops.request_merged)
		e->type->ops.request_merged(q, rq, type);

	/* [한국어] back merge라 끝 섹터가 바뀌었다 — 병합 후보 해시에서 뺐다가
	 * 새 키로 다시 넣는다. 이걸 빠뜨리면 이후 조회가 옛 끝 섹터로 이 request를
	 * 찾아내, 실제로는 인접하지 않은 bio를 붙이려 시도하게 된다. */
	if (type == ELEVATOR_BACK_MERGE)
		elv_rqhash_reposition(q, rq);

/* last_merge 캐시 갱신 */
	q->last_merge = rq;
}

/*
 * [한국어] (아래 상세 블록 참고) 두 request 병합을 스케줄러에 통지하는 진입점.
 * 병합 결과 살아남은 request 하나가 nvme_setup_rw()에서 SLBA/NLB가 확장된
 * 커맨드 하나로 변환되므로, SQ 엔트리·Command ID·완료 CQ 엔트리가 각각
 * 하나씩 절약된다.
 */
/*
 * [한국어]
 * elv_merge_requests - request 두 개가 병합됐음을 스케줄러에 알리고 자료구조 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   병합 결과로 남을 request (next를 흡수한 쪽)
 * @next: 병합되어 사라질 request (rq에 흡수됨)
 *
 * blk_attempt_req_merge()가 두 request를 하나로 합친 직후에 호출된다.
 * 스케줄러별 requests_merged 콜백으로 내부 큐 상태(예: BFQ의 두 rq 간
 * 연결)를 갱신하고, rq의 끝 섹터가 바뀌었으므로 해시를 재배치하여
 * 이후 back-merge 탐색이 올바른 위치에서 일어나도록 한다.
 *
 * 호출 체인:
 *   blk_attempt_req_merge → [elv_merge_requests]
 */
void elv_merge_requests(struct request_queue *q, struct request *rq,
			     struct request *next)
{
	/* [한국어] 콜백 테이블에 접근하기 위한 현재 스케줄러 인스턴스. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] 스케줄러에게 "next가 rq에 흡수되어 사라진다"를 알린다.
	 * 스케줄러는 자기 자료구조에서 next를 빼야 한다:
	 *   mq-deadline - dd_merged_requests(). next를 fifo_list와 sort_list
	 *                 rb-tree에서 제거하고, next의 만료 시각이 rq보다
	 *                 이르면 rq의 만료 시각을 앞당긴다(기아 방지 승계).
	 *   bfq         - bfq_requests_merged(). 두 bfq_queue 사이의 통계·
	 *                 위치 정보를 정리한다.
	 * 이 콜백을 빠뜨리면 이미 해제된 next를 스케줄러가 계속 가리키게 된다. */
	if (e->type->ops.requests_merged)
		e->type->ops.requests_merged(q, rq, next);

	/* [한국어] rq가 next를 흡수해 길어졌으므로 rq_hash_key(= 끝 섹터)가
	 * 달라졌다. 옛 버킷에 그대로 두면 이후 back merge 탐색에서 찾지 못하므로
	 * 새 키 위치로 옮긴다. */
	elv_rqhash_reposition(q, rq);
	/* [한국어] one-hit 캐시를 방금 커진 rq로 갱신한다. 순차 I/O에서는 바로
	 * 다음 bio가 이 rq 뒤에 붙을 확률이 높아, 해시 조회 없이 적중하게 된다. */
	q->last_merge = rq;
}

/*
 * [한국어]
 * elv_latter_request - 스케줄러 dispatch 순서에서 @rq 다음에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 다음 request; 없으면 NULL
 *
 * 스케줄러 내부 자료구조(BFQ의 B-WF2Q 큐, mq-deadline의 fifo/RB-tree 등)에서
 * @rq 바로 뒤에 위치한 요청을 찾는다. front-merge 시 merge 체인을 따라갈
 * 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_latter_request] → 스케줄러 ops.next_request
 */
struct request *elv_latter_request(struct request_queue *q, struct request *rq)
{
	/* [한국어] 콜백을 꺼내기 위한 현재 스케줄러 인스턴스. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] next_request는 선택 콜백이다. mq-deadline과 bfq는
	 * elv_rb_latter_request()(정렬 rb-tree의 rb_next)를 그대로 등록한다.
	 * kyber는 정렬 트리를 유지하지 않아 이 콜백이 없다. */
	if (e->type->ops.next_request)
		/* [한국어] LBA 순서상 바로 뒤에 오는 request를 반환. */
		return e->type->ops.next_request(q, rq);

	/* [한국어] 콜백이 없으면 "이웃을 알 수 없다"는 뜻으로 NULL.
	 * 호출자(blk-merge의 attempt_merge 경로)는 병합 시도를 포기한다. */
	return NULL;
}

/*
 * [한국어]
 * elv_former_request - 스케줄러 dispatch 순서에서 @rq 이전에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 이전 request; 없으면 NULL
 *
 * @rq 앞의 요청을 찾아 front-merge 후보를 탐색하거나 dispatch 순서를
 * 역방향으로 추적할 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_former_request] → 스케줄러 ops.former_request
 */
struct request *elv_former_request(struct request_queue *q, struct request *rq)
{
	/* [한국어] 콜백을 꺼내기 위한 현재 스케줄러 인스턴스. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] former_request도 선택 콜백. mq-deadline/bfq는
	 * elv_rb_former_request()(rb_prev)를 등록한다. */
	if (e->type->ops.former_request)
		/* [한국어] LBA 순서상 바로 앞에 오는 request를 반환. */
		return e->type->ops.former_request(q, rq);

	/* [한국어] 콜백 없음 → 이웃을 알 수 없다. */
	return NULL;
}

/* [한국어] sysfs attribute 포인터에서 그것을 감싼 elv_fs_entry를 역산하는 매크로.
 * elv_fs_entry는 { struct attribute attr; show(); store(); } 구조로, sysfs가
 * 콜백에 넘겨 주는 것은 내부의 attr 포인터뿐이다. 그래서 오프셋을 빼서
 * show/store 함수 포인터가 들어 있는 바깥 구조체를 되찾아야 한다.
 * _const 변형은 const 한정자를 보존해 읽기 전용 attribute 테이블에도
 * 쓸 수 있게 한다. */
#define to_elv(atr) container_of_const((atr), struct elv_fs_entry, attr)

/*
 * [한국어]
 * elv_attr_show - /sys/block/<disk>/queue/iosched/<attr> 읽기 핸들러
 *
 * @kobj: elevator_queue에 내장된 kobject (iosched sysfs 노드)
 * @attr: 읽을 sysfs attribute (elv_fs_entry로 캐스팅)
 * @page: 출력 버퍼 (PAGE_SIZE 크기)
 * @return: 쓴 바이트 수 또는 에러 코드
 *
 * sysfs kobject ops.show로 등록되어 사용자가 scheduler 파라미터를 읽을 때
 * 호출된다. sysfs_lock을 잡아 elevator 구조체와의 race를 막고,
 * ELEVATOR_FLAG_DYING이면(장치 제거 중) -ENODEV를 반환해 dangling 접근을 차단.
 *
 * 호출 체인:
 *   sysfs read → elv_sysfs_ops.show → [elv_attr_show] → entry->show(e, page)
 */
static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	/* [한국어] sysfs가 넘겨 준 attribute에서 스케줄러가 등록한 show/store
	 * 함수 포인터가 들어 있는 elv_fs_entry를 역산한다. */
	const struct elv_fs_entry *entry = to_elv(attr);
	/* [한국어] kobject를 품고 있는 elevator 인스턴스. 아래에서 역산한다. */
	struct elevator_queue *e;
	/* [한국어] 기본 반환값을 -ENODEV로 잡아 둔다. DYING이면 아래 if를
	 * 통과하지 못해 이 값이 그대로 반환된다 — "장치가 사라지는 중"이라는
	 * 뜻으로 사용자 공간에 전달된다. */
	ssize_t error = -ENODEV;

	/* [한국어] 쓰기 전용 attribute라면 show가 NULL이다. -EIO(잘못된 연산)를
	 * 돌려준다. 이 검사는 락 밖에서 해도 안전하다 — entry는 스케줄러 모듈의
	 * 읽기 전용 테이블이라 변하지 않는다. */
	if (!entry->show)
		return -EIO;

	/* [한국어] kobject → elevator_queue 역산. sysfs 콜백은 kobject만 주므로
	 * 여기서 소유 구조체를 되찾아야 스케줄러 사설 데이터에 닿을 수 있다. */
	e = container_of(kobj, struct elevator_queue, kobj);
	/* [한국어] sysfs 접근과 elevator 해제(elevator_exit)를 직렬화한다.
	 * 이 락이 없으면 show 콜백이 elevator_data를 읽는 동안 다른 CPU가
	 * 그것을 해제해 use-after-free가 난다. */
	mutex_lock(&e->sysfs_lock);
	/* [한국어] DYING 비트는 blk_mq_exit_sched()가 같은 sysfs_lock 안에서
	 * 세운다. 따라서 이 검사를 통과했다면 콜백 실행이 끝날 때까지
	 * elevator_data가 살아 있음이 보장된다. */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		/* [한국어] 스케줄러별 show 구현 호출(예: mq-deadline의
		 * read_expire_show). page에 값을 찍고 길이를 돌려준다. */
		error = entry->show(e, page);
	/* [한국어] 락 해제. 이후 elevator 해제가 진행될 수 있다. */
	mutex_unlock(&e->sysfs_lock);
	/* [한국어] 출력 바이트 수 또는 음수 에러를 VFS에 반환한다. */
	return error;
}

/*
 * [한국어]
 * elv_attr_store - /sys/block/<disk>/queue/iosched/<attr> 쓰기 핸들러
 *
 * @kobj:   elevator_queue에 내장된 kobject
 * @attr:   쓸 sysfs attribute
 * @page:   사용자 입력 버퍼
 * @length: 입력 길이
 * @return: 소비한 바이트 수 또는 에러 코드
 *
 * 사용자가 scheduler 파라미터(예: mq-deadline의 read_expire, write_expire)를
 * 변경할 때 호출된다. sysfs_lock으로 elevator_exit()와의 race를 막고,
 * DYING 상태이면 변경을 거부한다.
 *
 * 호출 체인:
 *   sysfs write → elv_sysfs_ops.store → [elv_attr_store] → entry->store(e, page, length)
 */
static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
	/* [한국어] attribute → elv_fs_entry 역산. store 함수 포인터를 여기서 얻는다. */
	const struct elv_fs_entry *entry = to_elv(attr);
	/* [한국어] kobject를 소유한 elevator 인스턴스(아래에서 역산). */
	struct elevator_queue *e;
	/* [한국어] DYING이면 그대로 반환될 기본 에러값. */
	ssize_t error = -ENODEV;

	/* [한국어] 읽기 전용 attribute면 store가 NULL이다 → -EIO. */
	if (!entry->store)
		return -EIO;

	/* [한국어] kobject → elevator_queue 역산. */
	e = container_of(kobj, struct elevator_queue, kobj);
	/* [한국어] show와 동일한 락. 튜너블을 바꾸는 도중 스케줄러가 해제되면
	 * 안 되므로 반드시 잡아야 한다. */
	mutex_lock(&e->sysfs_lock);
	/* [한국어] 해제가 시작된 스케줄러의 파라미터를 바꾸는 것은 의미가 없고
	 * 위험하므로 DYING이면 건너뛴다(-ENODEV 반환). */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		/* [한국어] 스케줄러별 store 구현 호출(예: mq-deadline의
		 * read_expire_store가 문자열을 정수로 파싱해 dd->fifo_expire[READ]에
		 * 밀리초→jiffies 변환해 저장). 소비한 바이트 수를 돌려준다. */
		error = entry->store(e, page, length);
	/* [한국어] 락 해제. */
	mutex_unlock(&e->sysfs_lock);
	/* [한국어] 소비 바이트 수 또는 음수 에러 반환. */
	return error;
}

/* [한국어] iosched kobject의 sysfs 연산 테이블.
 * sysfs 계층은 파일을 읽거나 쓸 때 이 두 함수만 부르고, 실제 어떤 튜너블인지는
 * attribute 포인터로 구분한다. 즉 스케줄러마다 파일 개수가 달라도 진입점은
 * 항상 이 두 개다.
 * 설정자: 아래 elv_ktype 정의에서 한 번만 연결.  읽는 자: fs/sysfs.
 * 동기화: 읽기 전용(const) 전역이라 동기화 불필요. */
static const struct sysfs_ops elv_sysfs_ops = {
	/* [한국어] 읽기 진입점 — /sys/.../queue/iosched/<attr> cat. */
	.show	= elv_attr_show,
	/* [한국어] 쓰기 진입점 — 같은 경로에 echo. */
	.store	= elv_attr_store,
};

/* [한국어] elevator_queue에 내장된 kobject의 타입 정의(위쪽에서 전방 선언됨).
 * kobj_type은 "이 kobject를 sysfs가 어떻게 다루고, 참조가 0이 되면 무엇을
 * 할 것인가"를 정한다.
 * 설정자: elevator_alloc()의 kobject_init()이 이 타입을 연결.
 * 읽는 자: sysfs 코어(show/store 디스패치)와 kobject 코어(마지막 put).
 * 동기화: const 전역이라 불필요. */
static const struct kobj_type elv_ktype = {
	/* [한국어] 위에서 정의한 show/store 디스패처. */
	.sysfs_ops	= &elv_sysfs_ops,
	/* [한국어] 참조 카운트가 0이 되는 순간 호출되어 구조체를 kfree한다.
	 * 이 콜백이 없으면 kobject 코어가 경고를 내고 메모리가 샌다. */
	.release	= elevator_release,
};

/*
 * [한국어]
 * elv_register_queue - 스케줄러를 sysfs/debugfs에 노출해 사용자에게 공개
 *
 * @q:      스케줄러가 붙은 request_queue
 * @e:      노출할 스케줄러 인스턴스
 * @uevent: udev에 KOBJ_ADD 이벤트를 보낼지 여부. 부팅 초기의 기본 스케줄러
 *          설정처럼 udev가 반응할 필요가 없는 경우에는 false를 넘긴다.
 * @return: 0 성공, 음수 errno 실패
 *
 * 스케줄러를 큐에 붙이는 실제 동작(elevator_switch)이 끝난 뒤, 그것을 사용자
 * 공간에서 보고 조정할 수 있게 만드는 마지막 단계다. 세 가지를 만든다:
 *   1) /sys/block/nvme0n1/queue/iosched/ 디렉터리
 *   2) 그 안의 튜너블 파일들 (mq-deadline이면 read_expire, write_expire,
 *      fifo_batch, writes_starved 등)
 *   3) debugfs의 스케줄러 상태 덤프
 *
 * ELEVATOR_FLAG_REGISTERED를 마지막에 세우는 순서가 중요하다. 이 비트가
 * 서 있어야 elv_unregister_queue()가 실제로 정리 작업을 수행하므로,
 * 등록이 완료된 뒤에 세워야 부분적으로 등록된 상태를 해제하려다 꼬이는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. sysfs 조작은 잠들 수 있다.
 * q->elevator_lock을 쥔 상태에서 호출된다.
 *
 * 에러 경로: kobject_add()가 실패하면 그대로 반환하고, 호출자
 * elevator_change_done()이 스케줄러를 해제한다. 개별 튜너블 파일 생성 실패는
 * 치명적이지 않다고 보고 루프만 중단한다(아래 참고).
 *
 * 호출 체인:
 *   elevator_change → elevator_change_done → [elv_register_queue]
 *     → kobject_add → sysfs_create_file → blk_mq_sched_reg_debugfs
 */
static int elv_register_queue(struct request_queue *q,
			      struct elevator_queue *e,
			      bool uevent)
{
	/* [한국어] kobject_add()의 결과를 담아 그대로 반환할 변수.
	 * 0이면 성공, 음수면 errno. */
	int error;

	/* [한국어] 큐의 sysfs 디렉터리(NVMe라면 /sys/block/nvme0n1/queue/) 아래에
	 * "iosched"라는 이름으로 kobject를 등록한다. 이 호출이 성공해야 디렉터리가
	 * 생긴다. 부모가 q->disk->queue_kobj이므로 위치가 queue/ 밑으로 고정된다.
	 * 주의: 스케줄러 선택 파일 자체(queue/scheduler)는 여기가 아니라
	 * block/blk-sysfs.c의 큐 attribute 목록이 만든다. 이 함수가 만드는 것은
	 * 선택된 스케줄러의 튜너블이 들어갈 iosched/ 하위 디렉터리다. */
	error = kobject_add(&e->kobj, &q->disk->queue_kobj, "iosched");
	/* [한국어] 디렉터리 생성에 성공한 경우에만 그 안을 채운다. 실패하면
	 * 아무것도 만들지 않고 error를 그대로 반환해 호출자가 롤백하게 한다. */
	if (!error) {
		/* [한국어] 스케줄러가 노출할 튜너블 목록. NULL로 끝나는 배열이며,
		 * 스케줄러마다 다르다(none은 아예 없고, mq-deadline은 5~6개). */
		const struct elv_fs_entry *attr = e->type->elevator_attrs;
		/* [한국어] 튜너블이 하나도 없는 스케줄러도 있으므로 NULL 검사.
		 * (kyber는 kyber_sched_attrs를 등록하고, mq-deadline은
		 *  read_expire/write_expire/writes_starved/front_merges/fifo_batch
		 *  등을 등록한다.) */
		if (attr) {
			/* [한국어] 배열 끝(name == NULL)까지 순회하며 파일을 만든다. */
			while (attr->attr.name) {
				/* [한국어] 파일 생성에 실패하면 루프를 중단하되 함수 전체는
				 * 성공으로 처리한다. 튜너블 하나가 없어도 스케줄러 자체는
				 * 정상 동작하므로, 여기서 실패시켜 I/O를 못 하게 만드는 것보다
				 * 일부 튜너블 없이 진행하는 편이 낫다는 판단이다. */
				if (sysfs_create_file(&e->kobj, &attr->attr))
					break;
				/* [한국어] 다음 튜너블로 전진. 배열은 name == NULL인
				 * 원소로 끝나므로 이 증가가 곧 종료 조건을 만든다. */
				attr++;
			}
		}
		/* [한국어] udev에 "새 kobject가 생겼다"고 알린다. udev 규칙이 스케줄러
		 * 변경에 반응해 추가 튜닝을 하도록 만들 수 있다. 부팅 시 기본 스케줄러
		 * 설정(elevator_set_default)은 no_uevent=true로 이를 건너뛴다 —
		 * 아직 udev가 준비되지 않았고 불필요한 이벤트 폭주를 막기 위해서다. */
		if (uevent)
			kobject_uevent(&e->kobj, KOBJ_ADD);

		/*
		 * Sched is initialized, it is ready to export it via
		 * debugfs
		 */
		/* [한국어] debugfs에 스케줄러 상태를 노출한다.
		 * /sys/kernel/debug/block/nvme0n1/sched/ 아래에 dispatch 큐 내용,
		 * 각 우선순위별 FIFO 상태 등이 덤프되어, NVMe I/O가 스케줄러에서
		 * 얼마나 대기하는지 직접 관찰할 수 있다. */
		blk_mq_sched_reg_debugfs(q);
		/* [한국어] 등록 완료 표시. 이 비트를 마지막에 세우는 이유는 위
		 * 함수 주석 참고 — 해제 경로가 이 비트를 보고 동작하기 때문이다. */
		set_bit(ELEVATOR_FLAG_REGISTERED, &e->flags);
	}
	/* [한국어] kobject_add의 결과를 그대로 반환. 실패하면 호출자가 롤백한다. */
	return error;
}

/*
 * [한국어]
 * elv_unregister_queue - request_queue의 elevator sysfs/debugfs 노드 제거
 *
 * @q: elevator가 연결된 request_queue
 * @e: 제거할 elevator_queue; NULL이면 no-op
 *
 * ELEVATOR_FLAG_REGISTERED를 원자적으로 클리어하고(test_and_clear_bit), sysfs의
 * iosched kobject를 제거한다. 이 시점부터 /sys/block/<disk>/queue/iosched는
 * 접근 불가하다. elevator_exit() 또는 elevator_change_done() 안에서 호출된다.
 *
 * 호출 체인:
 *   elevator_change_done / elv_exit_and_release → [elv_unregister_queue]
 */
static void elv_unregister_queue(struct request_queue *q,
				 struct elevator_queue *e)
{
	/* [한국어] 두 가지를 한꺼번에 처리한다:
	 *   e == NULL — 스케줄러가 애초에 없던 큐("none")에서도 그냥 부를 수 있게 한다.
	 *   test_and_clear_bit — REGISTERED 비트를 읽고 지우는 것을 원자적으로 한다.
	 *     비트가 서 있던 CPU 하나만 참을 받아 안으로 들어가므로, 두 CPU가
	 *     동시에 해제를 시도해도 kobject_del()이 두 번 불리지 않는다.
	 *     일반 test_bit + clear_bit 조합이라면 그 사이에 경쟁이 생긴다. */
	if (e && test_and_clear_bit(ELEVATOR_FLAG_REGISTERED, &e->flags)) {
		/* [한국어] udev에 "이 kobject가 사라진다"고 먼저 알린다. 제거보다
		 * 먼저 보내야 udev가 아직 유효한 sysfs 경로를 읽을 수 있다. */
		kobject_uevent(&e->kobj, KOBJ_REMOVE);
		/* [한국어] sysfs에서 iosched/ 디렉터리와 그 아래 튜너블을 모두 없앤다.
		 * kobject_del은 sysfs 표현만 지우고 참조 카운트는 건드리지 않는다.
		 * 메모리 해제는 나중에 kobject_put()이 참조를 0으로 만들 때
		 * elevator_release()에서 일어난다. 이 시점 이후 진행 중이던
		 * show/store는 sysfs 코어가 완료를 기다려 준다. */
		kobject_del(&e->kobj);

		/* unexport via debugfs before exiting sched */
		/* [한국어] /sys/kernel/debug/block/<disk>/sched/ 아래의 스케줄러
		 * 상태 덤프를 제거한다. 스케줄러 자료구조를 해제하기 전에 먼저
		 * 없애야, debugfs 파일을 읽고 있던 사용자가 해제된 메모리를
		 * 들여다보는 일이 없다. */
		blk_mq_sched_unreg_debugfs(q);
	}
}

/*
 * [한국어]
 * struct elevator_type 주요 필드 (정의는 block/elevator.h)
 *   elevator_name/alias : "mq-deadline", "bfq", "kyber" 같은 등록 이름과 별칭.
 *                         사용자가 /sys/block/<disk>/queue/scheduler 에 쓰는
 *                         문자열이 이 이름과 비교된다.
 *                         ("none"은 elevator_type이 없다 — q->elevator가
 *                          NULL인 상태를 가리키는 이름일 뿐이라 elv_list에
 *                          들어 있지 않다.)
 *   ops                 : insert_requests, dispatch_request, allow_merge,
 *                         request_merge 등 스케줄러 동작 전체를 담은 vtable.
 *                         블록 계층은 오직 이 함수 포인터를 통해서만 스케줄러를
 *                         호출한다.
 *   icq_size/icq_align  : io_cq(ICQ, "프로세스 × 큐" 단위 사설 객체) 크기와 정렬.
 *                         0이면 이 스케줄러는 ICQ를 쓰지 않는다(mq-deadline,
 *                         kyber). BFQ만 bfq_io_cq를 쓴다.
 *   elevator_attrs      : /sys/block/<disk>/queue/iosched/ 아래에 만들 튜너블 배열.
 *   icq_cache           : 위 icq_size로 만든 전용 slab 캐시. elv_register()가
 *                         만들고 elv_unregister()가 없앤다.
 *   list                : 전역 elv_list에 매달리기 위한 링크. elv_list_lock 보호.
 *
 * elv_register - 새 IO 스케줄러를 전역 elv_list에 등록한다
 *
 * @e: 등록할 스케줄러 설명자. 보통 모듈의 전역 static 변수라서 등록 후에도
 *     주소가 유지된다(그래서 목록에 포인터만 넣어도 안전하다).
 * @return: 0 성공, -EINVAL(필수 콜백 누락/icq 크기 오류),
 *          -ENOMEM(icq slab 캐시 생성 실패), -EBUSY(같은 이름이 이미 등록됨).
 *
 * 하는 일은 세 가지다: (1) 필수 콜백이 다 있는지 검증, (2) 이 스케줄러가
 * icq를 쓴다면 전용 slab 캐시 생성, (3) 이름 중복 확인 후 elv_list에 삽입.
 * 등록이 끝나면 그 순간부터 다른 CPU가 sysfs로 이 스케줄러를 큐에 붙일 수 있다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 컨텍스트). 내부에서 스핀락 구간을
 * 짧게 잡고, 그 밖에서 GFP_KERNEL 할당을 한다.
 *
 * NVMe 관점: 여기에 등록된 스케줄러만 sysfs에서 고를 수 있다. 다만 멀티큐
 * NVMe의 기본값은 이 목록에 없는 "none"이며, elevator_set_default()가
 * nr_hw_queues > 1이면 아예 붙이지 않는다.
 *
 * 호출 체인:
 *   스케줄러 모듈 initcall(deadline_init/bfq_init/kyber_init)
 *     → elv_register → __elevator_find(중복 검사) → list_add_tail
 */
int elv_register(struct elevator_type *e)
{
	/* finish request is mandatory */
	/* [한국어] 필수 콜백 검사 1 — finish_request.
	 * request가 완료될 때(성공이든 실패든) 스케줄러가 내부 상태를 정리할
	 * 기회다. 이것이 없으면 스케줄러가 자기가 발행한 request를 회수하지 못해
	 * 자료구조가 계속 부풀어 오른다. WARN_ON_ONCE로 개발자에게 스택 트레이스를
	 * 남기는 이유: 이것은 사용자 오류가 아니라 스케줄러 구현 버그이므로
	 * 조용히 거부하기보다 명확히 알려야 한다. */
	if (WARN_ON_ONCE(!e->ops.finish_request))
		return -EINVAL;
	/* insert_requests and dispatch_request are mandatory */
	/* [한국어] 필수 콜백 검사 2 — 요청의 입구와 출구.
	 *   insert_requests  : blk-mq가 request를 스케줄러에 맡기는 경로
	 *   dispatch_request : 스케줄러가 다음에 내보낼 request를 고르는 경로
	 * 이 둘이 스케줄러 존재 이유 자체이므로 없으면 등록할 수 없다.
	 * dispatch_request가 고른 request는 blk-mq가 드라이버로 넘기며,
	 * NVMe 장치라면 그것이 곧 다음 SQ 엔트리 하나가 된다. */
	if (WARN_ON_ONCE(!e->ops.insert_requests || !e->ops.dispatch_request))
		return -EINVAL;

	/* create icq_cache if requested */
	/* [한국어] icq(io_cq, I/O Context Queue)를 쓰는 스케줄러라면 전용 slab
	 * 캐시를 만든다. icq는 "프로세스 × 큐" 조합마다 하나씩 만들어지는 객체로,
	 * 그 프로세스가 이 장치에 대해 갖는 스케줄링 상태(BFQ의 가중치, 대기
	 * 이력 등)를 담는다. BFQ만 이것을 쓰고 mq-deadline/kyber는 쓰지 않는다.
	 * 자주 할당·해제되는 고정 크기 객체라 전용 slab 캐시가 효율적이다. */
	if (e->icq_size) {
		/* [한국어] icq는 struct io_cq를 첫 멤버로 품어야 한다(상속 관용구).
		 * 크기나 정렬이 그보다 작으면 공통 코드가 구조체 밖을 침범한다. */
		if (WARN_ON(e->icq_size < sizeof(struct io_cq)) ||
		    WARN_ON(e->icq_align < __alignof__(struct io_cq)))
			return -EINVAL;

		/* [한국어] "bfq_io_cq" 같은 캐시 이름을 만든다. /proc/slabinfo에
		 * 이 이름으로 나타나 메모리 사용량을 추적할 수 있다. */
		snprintf(e->icq_cache_name, sizeof(e->icq_cache_name),
			 "%s_io_cq", e->elevator_name);
		/* [한국어] slab 캐시 생성. 생성자(마지막 인자 NULL)를 두지 않으므로
		 * 할당 시점에 스케줄러가 직접 초기화한다. */
		e->icq_cache = kmem_cache_create(e->icq_cache_name, e->icq_size,
						 e->icq_align, 0, NULL);
		/* [한국어] slab 캐시 생성 실패 = 메모리 부족. 아직 elv_list에는
		 * 넣지 않았으므로 되돌릴 것이 없다. 모듈 로드가 실패로 끝난다. */
		if (!e->icq_cache)
			return -ENOMEM;
	}

	/* register, don't allow duplicate names */
	/* [한국어] 전역 목록을 보호하며 등록한다. 검사와 삽입이 같은 락 안에 있어야
	 * 두 모듈이 동시에 같은 이름으로 등록하는 경쟁을 막을 수 있다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 이름 중복 검사. 같은 이름이 둘이면 elevator_find_get()이
	 * 어느 것을 돌려줄지 불확정해지고, 사용자가 sysfs로 고른 스케줄러가
	 * 의도한 것이 아닐 수 있다. */
	if (__elevator_find(e->elevator_name)) {
		/* [한국어] 실패 경로 — 목록을 더 만질 일이 없으니 락부터 푼다.
		 * 아래 kmem_cache_destroy()는 잠들 수 있으므로 반드시 락 밖에서
		 * 해야 한다(스핀락 안에서 잠들면 데드락). */
		spin_unlock(&elv_list_lock);
		/* [한국어] 위에서 만든 slab 캐시를 되돌린다. icq_size가 0이었다면
		 * icq_cache는 NULL이고, kmem_cache_destroy(NULL)은 안전한 no-op다. */
		kmem_cache_destroy(e->icq_cache);
		/* [한국어] "이미 같은 이름이 쓰이고 있다"를 -EBUSY로 알린다.
		 * 같은 모듈을 두 번 로드하려 했거나, 이름이 겹치는 새 스케줄러다. */
		return -EBUSY;
	}
	/* [한국어] 목록 끝에 추가한다. 이 순간부터 다른 CPU가 이 스케줄러를
	 * 조회하고 큐에 붙일 수 있다. tail에 넣는 이유는 등록 순서를 유지해
	 * /sys/.../queue/scheduler 출력이 안정적으로 보이게 하기 위해서다. */
	list_add_tail(&e->list, &elv_list);
	/* [한국어] 삽입 완료 — 락 해제. 이 시점 이후 elevator_find_get()이
	 * 이 스케줄러를 찾아낼 수 있다. */
	spin_unlock(&elv_list_lock);

	/* [한국어] dmesg에 등록 사실을 남긴다. 부팅 로그에서 어떤 스케줄러가
	 * 사용 가능한지 확인할 수 있다. */
	printk(KERN_INFO "io scheduler %s registered\n", e->elevator_name);

	return 0;
}
EXPORT_SYMBOL_GPL(elv_register);

/*
 * [한국어]
 * elv_unregister - IO 스케줄러를 전역 목록에서 빼고 icq 캐시를 정리한다
 *
 * @e: 해제할 스케줄러 설명자
 * @return: 없음
 *
 * 모듈 언로드 경로에서 불린다. 이 함수가 불릴 때 이 스케줄러를 사용 중인
 * 큐는 하나도 없다 — elevator_find_get()/elevator_alloc()이 모듈 참조를
 * 잡아 두기 때문에, 사용 중이라면 애초에 rmmod가 -EBUSY로 거부된다.
 * 따라서 여기서는 "이미 아무도 안 쓴다"를 전제로 정리만 하면 된다.
 *
 * 실행 컨텍스트: 모듈 종료(프로세스 컨텍스트). rcu_barrier()가 잠들 수
 * 있으므로 락 밖에서 호출한다.
 *
 * 호출 체인:
 *   모듈 exit(deadline_exit/bfq_exit/kyber_exit) → [elv_unregister]
 */
void elv_unregister(struct elevator_type *e)
{
	/* unregister */
	/* [한국어] 전역 목록 보호 락. 제거는 짧으므로 스핀락 구간도 짧다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 목록에서 뺀다. _init 변형이라 링크가 자기 자신을 가리키도록
	 * 초기화되어, 나중에 실수로 다시 지워도 리스트가 깨지지 않는다.
	 * 이 순간 이후 __elevator_find()는 이 스케줄러를 찾지 못하므로,
	 * 사용자가 sysfs로 이 이름을 써도 -EINVAL을 받는다. */
	list_del_init(&e->list);
	/* [한국어] 락 해제. 아래 rcu_barrier()는 잠들 수 있어 락 안에서 못 한다. */
	spin_unlock(&elv_list_lock);

	/*
	 * Destroy icq_cache if it exists.  icq's are RCU managed.  Make
	 * sure all RCU operations are complete before proceeding.
	 */
	/* [한국어] icq를 쓰는 스케줄러(현재는 BFQ뿐)만 정리할 캐시가 있다. */
	if (e->icq_cache) {
		/* [한국어] icq 객체는 RCU로 해제된다 — ioc_destroy_icq()가
		 * call_rcu()/kfree_rcu()로 해제를 유예하므로, 이 함수가 불리는
		 * 시점에도 "아직 실행되지 않은 해제 콜백"이 남아 있을 수 있다.
		 * 그 콜백은 나중에 kmem_cache_free(e->icq_cache, ...)를 부르는데,
		 * 캐시를 먼저 없애 버리면 이미 파괴된 캐시에 객체를 반납하게 된다.
		 * rcu_barrier()는 "이미 등록된 모든 RCU 콜백이 실행 완료될 때까지"
		 * 기다려 그 경쟁을 없앤다.
		 * (synchronize_rcu()로는 부족하다 — 그것은 유예 기간만 기다릴 뿐
		 *  콜백 실행 완료를 보장하지 않는다.)
		 * 이것은 NVMe와 무관한 일반 RCU 수명 문제다. */
		rcu_barrier();
		/* [한국어] 이제 안전하게 slab 캐시를 파괴한다. 캐시에 남은 객체가
		 * 있으면 커널이 경고를 낸다 — 그것 자체가 누수 탐지 장치다. */
		kmem_cache_destroy(e->icq_cache);
		/* [한국어] dangling 포인터를 남기지 않는다. 같은 모듈이 다시
		 * 로드되면 elv_register()가 새 캐시를 만들어 채운다. */
		e->icq_cache = NULL;
	}
}
EXPORT_SYMBOL_GPL(elv_unregister);

/*
 * Switch to new_e io scheduler.
 *
 * If switching fails, we are most likely running out of memory and not able
 * to restore the old io scheduler, so leaving the io scheduler being none.
 */
/*
 * [한국어]
 * elevator_switch - 큐의 I/O 스케줄러를 @ctx->name이 가리키는 것으로 교체한다
 *
 * @q:   대상 request_queue
 * @ctx: 교체 컨텍스트. 입력으로 name(새 스케줄러 이름)과 res(미리 할당된
 *       스케줄러 자원)를 받고, 출력으로 old(내려간 인스턴스)와
 *       new(올라온 인스턴스)를 채워 준다. 호출자가 나중에 이 둘을 보고
 *       sysfs 등록/해제와 자원 해제를 마무리한다.
 * @return: 0 성공, -EINVAL(그런 이름의 스케줄러 없음),
 *          blk_mq_init_sched()의 실패 코드(대개 -ENOMEM).
 *
 * === 교체 순서가 이렇게 짜인 이유 ===
 * 호출 시점에 큐는 이미 freeze되어 있고(호출자 elevator_change가 건다),
 * 이 함수가 그 위에 quiesce를 얹는다. 두 가지는 서로 다른 것을 막는다:
 *   freeze  — 새 request가 큐에 "들어오는" 것을 막고, 이미 있는 것이
 *             전부 완료되기를 기다린다(참조 카운터 기반).
 *   quiesce — 큐에 있는 request가 드라이버로 "나가는" 것을 막는다.
 *             내부적으로 SRCU/RCU 유예 기간을 기다려, 이미 실행 중인
 *             dispatch(= mq_ops->queue_rq 호출 중)가 끝나게 만든다.
 * 스케줄러 교체는 자료구조를 통째로 바꾸는 일이므로, 제출 쪽과 디스패치 쪽이
 * 모두 정지해야 안전하다. 그래서 둘 다 필요하다.
 *
 * 전체 순서:
 *   1. (호출자) blk_mq_freeze_queue        — 신규 진입 차단 + 진행 중 완료 대기
 *   2. (호출자) mutex_lock(&q->elevator_lock) — 교체끼리의 직렬화
 *   3. elevator_find_get(새 이름)          — "none"이면 생략
 *   4. blk_mq_quiesce_queue                — 디스패치 정지
 *   5. elevator_exit(옛 스케줄러)          — exit_sched 콜백, q->elevator = NULL
 *   6. blk_mq_init_sched(새 스케줄러)      — 또는 "none"이면 큐 파라미터 원복
 *   7. blk_mq_unquiesce_queue              — 디스패치 재개
 *   8. (호출자) blk_mq_unfreeze_queue      — 신규 진입 재개
 *   9. (호출자) elevator_change_done       — sysfs 등록/해제, 옛 자원 해제
 * sysfs 작업(9)이 락 구간(2~8) 밖으로 빠진 것이 중요하다. kobject 제거는
 * 진행 중인 sysfs 읽기/쓰기가 끝나기를 기다리는데, 그 핸들러가 다시
 * elevator_lock을 잡으려 하면 데드락이 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. q->elevator_lock을 쥐고 있으며 큐는 freeze
 * 상태다. quiesce/unquiesce가 잠들 수 있다.
 *
 * NVMe 관점: 이 함수가 도는 동안 mq_ops->queue_rq(= NVMe PCIe에서는
 * nvme_queue_rq)가 호출되지 않으므로 새 커맨드가 SQ에 실리지 않는다.
 * 사용자가 sysfs로 스케줄러를 바꾸는 순간 짧은 I/O 정지가 생기는 이유다.
 *
 * 호출 체인:
 *   elv_iosched_store(sysfs) / elevator_set_default / elevator_set_none
 *     → elevator_change → [elevator_switch]
 *       → blk_mq_quiesce_queue → elevator_exit → blk_mq_init_sched
 */
static int elevator_switch(struct request_queue *q, struct elv_change_ctx *ctx)
{
	/* [한국어] 새로 붙일 스케줄러 타입. "none"으로 전환하는 경우 NULL로 남는다. */
	struct elevator_type *new_e = NULL;
	/* [한국어] 반환할 결과. 성공을 기본값으로 두고, blk_mq_init_sched()가
	 * 실패했을 때만 음수로 바뀐다. out_unfreeze 라벨에서 이 값을 보고
	 * 경고를 낼지 결정하므로 반드시 미리 초기화해야 한다. */
	int ret = 0;

	/* [한국어] 큐가 freeze된 상태여야 한다는 불변식 검사. freeze는 "새 요청
	 * 진입을 막고 진행 중인 요청이 모두 끝나기를 기다린" 상태를 뜻한다.
	 * 스케줄러를 바꾸는 동안 그것을 참조하는 request가 살아 있으면 해제된
	 * 자료구조를 건드리게 되므로, 호출자가 반드시 먼저 freeze해야 한다. */
	WARN_ON_ONCE(q->mq_freeze_depth == 0);
	/* [한국어] 스케줄러 교체 직렬화 락도 쥐고 있어야 한다. freeze는 I/O를
	 * 막을 뿐, 두 스레드가 동시에 스케줄러를 바꾸는 것은 막지 못한다. */
	lockdep_assert_held(&q->elevator_lock);

	/* [한국어] "none"이 아니면 실제 스케줄러 모듈을 찾아 참조를 획득한다.
	 * strncmp의 반환값이 0이 아닐 때(= 이름이 "none"이 아닐 때) 진입한다.
	 * "none"은 등록 목록에 없는 특별한 이름으로, "스케줄러를 붙이지 않음"을
	 * 뜻한다. NVMe 멀티큐의 기본 상태가 바로 이것이다. */
	if (strncmp(ctx->name, "none", 4)) {
		new_e = elevator_find_get(ctx->name);
		/* [한국어] 없는 스케줄러 이름 — 사용자에게 -EINVAL을 돌려준다.
		 * 아직 아무것도 바꾸지 않았으므로 롤백할 것이 없다. */
		if (!new_e)
			return -EINVAL;
	}

	/* [한국어] freeze에 더해 quiesce까지 건다. 둘의 차이가 중요하다:
	 *   freeze  - 새 요청이 큐에 "들어오는 것"을 막는다.
	 *   quiesce - 이미 큐에 있는 요청이 드라이버로 "나가는 것"을 막는다.
	 * 스케줄러 교체는 dispatch 경로가 완전히 멈춰야 안전하므로 둘 다 필요하다.
	 * 내부적으로 SRCU 유예 기간을 기다려 실행 중인 dispatch가 끝나게 한다. */
	blk_mq_quiesce_queue(q);

	/* [한국어] 원래 스케줄러가 있던 경우에만 내리는 절차를 밟는다.
	 * 이미 "none"이었다면(q->elevator == NULL) 내릴 것이 없다. */
	if (q->elevator) {
		/* [한국어] 기존 스케줄러 포인터를 ctx에 보관한다. 여기서 바로 해제하지
		 * 않는 이유는, 해제에 sysfs 조작이 포함되어 있어 지금 쥐고 있는 락
		 * 아래에서 하면 락 순서 역전이 발생하기 때문이다. 호출자
		 * elevator_change_done()이 락을 푼 뒤 정리한다. */
		ctx->old = q->elevator;
		/* [한국어] 스케줄러의 exit_sched 콜백을 부르고 q->elevator를 떼어낸다.
		 * 이 시점 이후 큐는 잠시 스케줄러가 없는 상태가 된다. */
		elevator_exit(q);
	}

	/* [한국어] 새로 붙일 스케줄러가 있는 경우와 "none"인 경우로 갈린다. */
	if (new_e) {
		/* [한국어] 새 스케줄러를 초기화해 큐에 연결한다. 내부에서 각 hctx의
		 * sched_tags를 (미리 할당된 res->et에서) 연결하고 init_sched 콜백을 부른다.
		 *
		 * 스케줄러 태그(sched_tags)와 드라이버 태그의 분리가 핵심이다.
		 * sched_tags는 "스케줄러가 대기시켜 둘 수 있는 request 수"를 정하고,
		 * 드라이버 태그는 "실제로 장치에 떠 있을 수 있는 커맨드 수"를 정한다.
		 * NVMe에서 드라이버 태그 번호가 곧 커맨드의 Command ID이고,
		 * 스케줄러 태그는 CID가 아니다 — 자세한 내용은 block/blk-mq-sched.c 참고.
		 *
		 * 크기: blk_mq_alloc_sched_res()가 blk_mq_default_nr_requests(set)
		 * = 2 * min(set->queue_depth, BLKDEV_DEFAULT_RQ=128)을 쓴다
		 * (block/blk-mq.h). 즉 큐 깊이가 큰 NVMe(보통 1023)에서는
		 * 2 * 128 = 256으로, 오히려 드라이버 태그보다 적다. */
		ret = blk_mq_init_sched(q, new_e, &ctx->res);
		/* [한국어] 초기화 실패(대개 메모리 부족) — 큐는 스케줄러 없는 상태로
		 * 남는다. 위 영문 주석이 밝히듯 옛 스케줄러를 되살리는 것도 메모리를
		 * 요구하므로, 그냥 "none"으로 두는 것이 가장 안전한 폴백이다. */
		if (ret)
			goto out_unfreeze;
		/* [한국어] 새 인스턴스를 ctx에 기록. 호출자가 나중에 sysfs 등록을 한다. */
		ctx->new = q->elevator;
	} else {
		/* [한국어] "none"으로 전환하는 경로.
		 * QUEUE_FLAG_SQ_SCHED는 "단일 큐 스케줄러가 붙어 있다"는 표시로,
		 * dispatch 경로가 스케줄러를 경유할지 결정한다. 해제하면 이후
		 * request가 소프트웨어 큐에서 하드웨어 큐로 직행한다. */
		blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);
		/* [한국어] 큐에서 스케줄러를 완전히 떼어낸다. 이후 이 포인터가
		 * NULL이라는 사실 하나로 블록 계층 전체가 "스케줄러 없음"을 판단한다
		 * (blk_mq_sched_bio_merge, __blk_mq_sched_dispatch_requests,
		 *  blk_mq_get_driver_tag 경로가 모두 이 검사를 한다).
		 * 멀티큐 NVMe의 정상 상태가 바로 이 NULL이다. */
		q->elevator = NULL;
		/* [한국어] 큐 깊이를 드라이버 태그 개수로 되돌린다. 스케줄러가 있을
		 * 때는 sched_tags 크기(보통 2배)를 쓰지만, 없으면 실제 드라이버가
		 * 받을 수 있는 만큼만 담는 것이 맞다. NVMe에서 tag_set->queue_depth는
		 * I/O 큐 하나의 깊이(nvme_dev의 q_depth, 보통 1024)에서 온다. */
		q->nr_requests = q->tag_set->queue_depth;
		/* [한국어] 비동기 요청 전용 깊이 제한도 함께 되돌린다. 스케줄러가
		 * 있을 때는 async 쓰기가 태그를 독점해 동기 읽기를 굶기지 않도록
		 * 더 낮은 값을 두지만, 스케줄러가 없으면 그런 제한이 무의미하다. */
		q->async_depth = q->tag_set->queue_depth;
	}
	/* [한국어] blktrace 로그에 전환 사실을 남긴다. I/O 성능이 특정 시점부터
	 * 달라졌을 때 스케줄러 변경이 원인인지 추적하는 데 쓴다. */
	blk_add_trace_msg(q, "elv switch: %s", ctx->name);

out_unfreeze:
	/* [한국어] dispatch를 재개한다. 이 시점부터 새 스케줄러(또는 none)를
	 * 통해 요청이 드라이버로 흐른다. freeze 해제는 호출자가 담당한다. */
	blk_mq_unquiesce_queue(q);

	/* [한국어] blk_mq_init_sched()가 실패해 goto로 뛰어온 경우에만 참이다.
	 * 정상 경로에서는 ret이 0이라 이 블록을 건너뛴다. */
	if (ret) {
		/* [한국어] 실패를 사용자에게 알린다. 큐는 "none" 상태로 남아 I/O는
		 * 계속 동작하므로 치명적이지 않다. new_e는 여기 도달할 때 반드시
		 * NULL이 아니다(ret이 0이 아닌 유일한 경로가 blk_mq_init_sched
		 * 실패이고, 그것은 new_e가 있을 때만 실행되므로). */
		pr_warn("elv: switch to \"%s\" failed, falling back to \"none\"\n",
			new_e->elevator_name);
	}

	/* [한국어] elevator_find_get()으로 잡았던 모듈 참조를 반납한다. 성공했다면
	 * blk_mq_init_sched()가 자체 참조를 이미 잡았으므로 여기서 놓아도 안전하다. */
	if (new_e)
		elevator_put(new_e);
	return ret;
}

/*
 * [한국어]
 * elv_exit_and_release - elevator를 완전히 내리고 자원을 해제하는 rollback 경로
 *
 * @ctx: elevator 전환 컨텍스트 (전환 중 할당한 res/type 정보)
 * @q:   elevator를 제거할 request_queue
 *
 * elevator_change_done()에서 새 elevator의 sysfs 등록이 실패하면 이 함수로
 * rollback한다. queue를 freeze해 진행 중인 I/O를 멈추고, elevator_exit()으로
 * 스케줄러 상태를 해제한 뒤, sched_res와 kobject를 최종 반환한다.
 * 이 경로를 거치면 request_queue는 elevator가 없는("none") 상태가 된다.
 *
 * 호출 체인:
 *   elevator_change_done → elv_register_queue 실패 → [elv_exit_and_release]
 */
static void elv_exit_and_release(struct elv_change_ctx *ctx,
		struct request_queue *q)
{
	/* [한국어] 내려갈 elevator 인스턴스를 락 안에서 붙잡아 둘 지역 변수.
	 * elevator_exit()이 q->elevator를 NULL로 만들기 때문에, 그 전에
	 * 포인터를 따로 챙겨 두어야 나중에 kobject_put()을 할 수 있다. */
	struct elevator_queue *e;
	/* [한국어] blk_mq_freeze_queue()가 저장해 주는 memalloc 상태.
	 * freeze 구간에서는 이 태스크를 PF_MEMALLOC_NOIO 성격으로 바꿔
	 * "메모리 할당이 다시 이 큐로 I/O를 내려보내는" 재귀를 막는다.
	 * unfreeze 시 이 값으로 원래 상태를 복원한다. */
	unsigned memflags;

	/* [한국어] 신규 요청 진입을 막고 진행 중인 요청이 모두 끝나기를 기다린다.
	 * 이 함수는 다른 경로들과 달리 스스로 freeze를 건다 — 롤백 전용 경로라
	 * 호출자가 이미 큐를 녹인 뒤에 불리기 때문이다. */
	memflags = blk_mq_freeze_queue(q);
	/* [한국어] 스케줄러 교체 직렬화 락. elevator_exit()의 전제 조건이다. */
	mutex_lock(&q->elevator_lock);
	/* [한국어] 아래 elevator_exit()이 q->elevator를 지우기 전에 포인터 확보. */
	e = q->elevator;
	/* [한국어] icq 정리 + exit_sched 콜백 + q->elevator = NULL.
	 * 이 호출 이후 큐는 "none" 상태가 된다. */
	elevator_exit(q);
	/* [한국어] 락 해제. 아래 자원 해제는 락 밖에서 해야 안전하다. */
	mutex_unlock(&q->elevator_lock);
	/* [한국어] I/O 재개. 스케줄러 없이 blk-mq가 직접 디스패치한다. */
	blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] 애초에 스케줄러가 있었을 때만 자원을 반납한다. */
	if (e) {
		/* [한국어] 큐를 얼리기 전에 미리 할당해 두었던 스케줄러 태그(et)와
		 * 사설 데이터(data)를 해제한다. 이 자원들은 elevator_change()가
		 * 선할당했고 elevator_alloc()이 elevator_queue에 연결했던 것이다. */
		blk_mq_free_sched_res(&ctx->res, ctx->type, q->tag_set);
		/* [한국어] kobject 참조 반납. 마지막 참조였다면 elevator_release()가
		 * 불려 구조체가 kfree된다. sysfs를 통해 아직 접근 중인 주체가 있으면
		 * 그가 끝날 때까지 해제가 미뤄진다. */
		kobject_put(&e->kobj);
	}
}

/*
 * [한국어]
 * elevator_change_done - 락 밖에서 해야 하는 뒷정리: 옛 스케줄러 해제 + 새 것 등록
 *
 * @q:   전환이 일어난 request_queue
 * @ctx: 전환 컨텍스트. ctx->old(내려간 것)와 ctx->new(올라온 것)가 채워져 있다.
 * @return: 0 성공, 음수 errno(새 스케줄러 sysfs 등록 실패)
 *
 * === 왜 elevator_switch()에서 다 하지 않고 이 함수로 미뤘는가 ===
 * elevator_switch()는 q->elevator_lock을 쥐고 큐가 freeze/quiesce된 상태에서
 * 실행된다. 그런데 이 함수가 하는 두 가지 일은 그 상태에서 하면 안 된다:
 *   - sysfs kobject 조작(생성/제거)은 내부적으로 자체 락을 잡는데, 그것이
 *     elevator_lock과 반대 순서로 잡히는 경로가 있어 데드락 위험이 있다.
 *   - kobject_put()으로 인한 최종 해제는 잠들 수 있다.
 * 그래서 elevator_switch()는 포인터만 ctx에 담아 두고, 호출자가 락을 푼 뒤
 * 이 함수를 불러 실제 정리를 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. q->elevator_lock을 쥐지 않은 상태이며,
 * 큐도 이미 unfreeze되어 I/O가 흐르고 있다. 옛 스케줄러는 이미 큐에서
 * 분리되었으므로 지금 해제해도 아무도 참조하지 않는다.
 *
 * 에러 경로: 새 스케줄러의 sysfs 등록이 실패하면 elv_exit_and_release()로
 * 롤백해 큐를 "none" 상태로 만든다. I/O는 계속 동작한다.
 *
 * 호출 체인:
 *   elevator_change → [elevator_change_done]
 *     → elv_unregister_queue / blk_mq_free_sched_res / kobject_put (옛 것)
 *     → elv_register_queue (새 것) → 실패 시 elv_exit_and_release
 */
static int elevator_change_done(struct request_queue *q,
				struct elv_change_ctx *ctx)
{
	/* [한국어] 반환값. 새 스케줄러의 sysfs 등록이 실패할 때만 음수가 된다.
	 * 옛 스케줄러 정리는 실패할 수 없는 경로라 ret에 영향을 주지 않는다. */
	int ret = 0;

	/* [한국어] 옛 스케줄러가 있었다면(none → X 전환이 아니라면) 먼저 정리한다.
	 * 순서가 중요하다 — 옛 것을 완전히 치우고 나서 새 것을 등록해야
	 * /sys/.../queue/iosched 라는 같은 이름의 kobject가 충돌하지 않는다. */
	if (ctx->old) {
		/* [한국어] 해제할 자원 정보를 스택 구조체에 미리 복사해 둔다.
		 * elv_unregister_queue()나 kobject_put()이 진행되면서 ctx->old의
		 * 필드가 무효해질 수 있으므로, 그 전에 필요한 값(et = 스케줄러 태그
		 * 세트, data = 스케줄러 사설 데이터)을 안전한 곳으로 옮긴다. */
		struct elevator_resources res = {
			/* [한국어] 옛 스케줄러가 쓰던 스케줄러 태그 세트. */
			.et = ctx->old->et,
			/* [한국어] 옛 스케줄러의 사설 데이터(deadline_data 등). */
			.data = ctx->old->elevator_data
		};

		/* [한국어] sysfs에서 iosched 디렉터리와 튜너블 파일들을 제거한다.
		 * 이 시점부터 사용자 공간은 옛 스케줄러의 파라미터에 접근할 수 없다. */
		elv_unregister_queue(q, ctx->old);
		/* [한국어] 스케줄러 전용 태그 세트(sched_tags)와 사설 데이터를 해제한다.
		 * sched_tags는 하드웨어 큐마다 하나씩 있으므로 tag_set 정보가 필요하다.
		 * NVMe 관점: 여기서 해제하는 것은 스케줄러 태그다. NVMe Command ID로
		 * 쓰이는 것은 드라이버 태그(tag_set->tags)이고 그쪽은 그대로 남으므로,
		 * 이 해제가 컨트롤러 쪽 상태에 영향을 주지 않는다. */
		blk_mq_free_sched_res(&res, ctx->old->type, q->tag_set);
		/* [한국어] kobject 참조를 놓는다. 마지막 참조였다면 elevator_release()가
		 * 호출되어 elevator_queue 구조체 자체가 kfree된다. sysfs를 통해 누군가
		 * 아직 파일을 열고 있다면 그가 닫을 때까지 해제가 지연된다 —
		 * 참조 계수 방식이 주는 안전성이다. */
		kobject_put(&ctx->old->kobj);
	}
	/* [한국어] 새 스케줄러가 올라왔다면("none"으로의 전환이 아니라면) 사용자
	 * 공간에 노출한다. */
	if (ctx->new) {
		/* [한국어] no_uevent를 뒤집어 uevent 인자로 넘긴다. 부팅 시 기본
		 * 스케줄러 설정은 no_uevent=true라 udev 이벤트를 보내지 않고,
		 * 사용자가 sysfs로 명시적으로 바꾼 경우에는 이벤트를 보낸다. */
		ret = elv_register_queue(q, ctx->new, !ctx->no_uevent);
		/* [한국어] sysfs 등록 실패 — 스케줄러는 이미 큐에 붙어 동작 중이지만
		 * 사용자가 제어할 수 없는 반쪽 상태다. 그런 상태로 두느니 완전히
		 * 내려서 "none"으로 만드는 편이 낫다. */
		if (ret)
			elv_exit_and_release(ctx, q);
	}
	return ret;
}

/*
 * Switch this queue to the given IO scheduler.
 */
/*
 * elevator_change - 사용자/sysfs 요청에 따라 elevator를 변경
 *   호출 경로: elevator_set_default / elv_iosched_store -> elevator_change
 *   NVMe 연결:
 *     - queue freeze -> blk_mq_cancel_work_sync -> elevator_switch ->
 *       blk_mq_init_sched -> blk_mq_unquiesce_queue.
 *     - 이 과정에서 NVMe SQ/CID는 그대로 두고, 상위 request_queue의
 *       스케줄링 레이어만 교체한다.
 */
static int elevator_change(struct request_queue *q, struct elv_change_ctx *ctx)
{
	/* [한국어] freeze 구간 동안의 memalloc 상태를 담아 두었다가 unfreeze 시
	 * 원복하기 위한 값. 자세한 이유는 아래 blk_mq_freeze_queue() 주석 참고. */
	unsigned int memflags;
	/* [한국어] 이 큐가 속한 태그 세트. nr_hw_queues와 queue_depth를 여기서
	 * 읽어 스케줄러 자원 크기를 정한다. NVMe라면 nvme_ctrl의 tagset이다. */
	struct blk_mq_tag_set *set = q->tag_set;
	/* [한국어] 반환값. 자원 할당 실패나 전환 실패 시 음수 errno가 된다. */
	int ret = 0;

	/* [한국어] update_nr_hwq_lock을 읽기 모드로 쥐고 있어야 한다는 계약을 검증.
	 * 이 rwsem은 "하드웨어 큐 개수 변경"과 "스케줄러 교체"가 겹치지 않게 한다.
	 * 겹치면 방금 계산한 nr_hw_queues만큼 sched_tags를 잡았는데 그 사이에
	 * hctx 개수가 달라져 배열 크기가 어긋난다. NVMe에서는 컨트롤러 리셋 후
	 * blk_mq_update_nr_hw_queues()가 이 락을 쓰기 모드로 잡는다. */
	lockdep_assert_held(&set->update_nr_hwq_lock);

	/* [한국어] "none"이 아니라면 스케줄러 자원을 미리 할당한다.
	 * 왜 미리 하는가: 아래에서 큐를 freeze한 뒤에 할당하면, 메모리 회수가
	 * 이 큐로의 write-back I/O를 필요로 할 때 데드락이 발생한다(freeze된 큐는
	 * I/O를 받지 않으므로 회수가 영원히 끝나지 않는다). freeze 전에 할당하면
	 * 이 재귀를 피할 수 있다.
	 * nr_hw_queues만큼 할당하는 이유: 스케줄러 태그 세트는 하드웨어 큐마다
	 * 하나씩 필요하기 때문이다. NVMe에서는 SQ/CQ 쌍 개수만큼 만들어진다. */
	if (strncmp(ctx->name, "none", 4)) {
		ret = blk_mq_alloc_sched_res(q, ctx->type, &ctx->res,
				/* [한국어] 하드웨어 큐 개수 = 만들 sched_tags 배열 길이.
				 * (태그를 공유하는 장치라면 blk_mq_alloc_sched_tags가
				 *  내부에서 1개만 만든다.) */
				set->nr_hw_queues);
		/* [한국어] 메모리 부족 — 아직 아무것도 바꾸지 않았으므로 그대로 반환.
		 * 기존 스케줄러가 계속 동작한다. */
		if (ret)
			return ret;
	}

	/* [한국어] 큐를 freeze한다. 새 요청 진입을 막고 진행 중인 요청이 모두
	 * 완료되기를 기다린다. 반환값 memflags는 freeze 동안 설정된
	 * PF_MEMALLOC_NOIO 상태를 담고 있어, unfreeze 시 원복하는 데 쓴다.
	 * 이 플래그가 필요한 이유: freeze 구간에서 메모리 할당이 I/O를 유발하면
	 * 자기 자신이 막아 놓은 큐를 기다리는 데드락이 되므로, 커널에 "이 구간에서는
	 * I/O를 유발하는 회수를 하지 말라"고 알려 둔다. */
	memflags = blk_mq_freeze_queue(q);
	/*
	 * May be called before adding disk, when there isn't any FS I/O,
	 * so freezing queue plus canceling dispatch work is enough to
	 * drain any dispatch activities originated from passthrough
	 * requests, then no need to quiesce queue which may add long boot
	 * latency, especially when lots of disks are involved.
	 *
	 * Disk isn't added yet, so verifying queue lock only manually.
	 */
	/* [한국어] 예약된 dispatch 워크(run_work, requeue_work)를 취소하고 이미
	 * 실행 중인 것이 끝나기를 기다린다. freeze는 새 요청을 막을 뿐, 워커가
	 * 이미 큐에 있던 요청을 드라이버로 내보내는 것은 막지 못한다. 스케줄러를
	 * 교체하는 동안 워커가 옛 스케줄러 자료구조를 만지면 안 되므로 필요하다.
	 * 위 영문 주석이 설명하듯, freeze + 워크 취소만으로 충분해서 quiesce까지
	 * 걸지 않는다 — 디스크가 여러 개인 시스템에서 quiesce의 SRCU 대기가
	 * 부팅 지연을 크게 늘리기 때문이다. */
	blk_mq_cancel_work_sync(q);
	/* [한국어] 스케줄러 교체를 직렬화하는 뮤텍스. 두 스레드가 동시에 sysfs로
	 * 스케줄러를 바꾸는 상황을 막는다. */
	mutex_lock(&q->elevator_lock);
	/* [한국어] 요청된 스케줄러가 이미 붙어 있으면 아무것도 하지 않는다.
	 * 조건을 풀어 읽으면 "현재 스케줄러가 있고 그 이름이 요청과 일치하는"
	 * 경우가 아닐 때만 전환한다. 같은 스케줄러로 다시 바꾸는 것은 무의미한
	 * freeze/quiesce 비용만 발생시키므로 걸러 낸다.
	 * q->elevator가 NULL("none")인데 ctx->name도 "none"인 경우는 이 조건에
	 * 걸리지 않아 elevator_switch()가 호출되지만, 그쪽에서 다시 무해하게
	 * 처리된다. */
	if (!(q->elevator && elevator_match(q->elevator->type, ctx->name)))
		/* [한국어] 실제 교체 수행. 이 안에서 quiesce → exit → init →
		 * unquiesce가 일어난다. */
		ret = elevator_switch(q, ctx);
	/* [한국어] 교체 완료(또는 생략) — 직렬화 락 해제. */
	mutex_unlock(&q->elevator_lock);
	/* [한국어] 큐를 다시 열어 I/O를 재개한다. memflags로 freeze 전의 메모리
	 * 할당 컨텍스트를 원복한다. 이 시점부터 새 스케줄러로 요청이 흐른다. */
	blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] 전환이 성공했다면 락 밖에서 해야 하는 뒷정리(옛 스케줄러 해제,
	 * 새 스케줄러 sysfs 등록)를 수행한다. 실패했다면 정리할 것이 없다. */
	if (!ret)
		ret = elevator_change_done(q, ctx);

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	/* [한국어] ctx->new가 NULL이면 새 스케줄러가 큐에 붙지 못했다는 뜻이다
	 * (전환 실패, 또는 "none"으로 전환, 또는 이미 같은 스케줄러라 생략).
	 * 그렇다면 위에서 미리 할당해 둔 자원이 주인 없이 남으므로 해제한다.
	 * 반대로 ctx->new가 있으면 그 자원은 스케줄러가 소유하고 있으므로
	 * 여기서 건드리면 안 된다. */
	if (!ctx->new)
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);

	return ret;
}

/*
 * The I/O scheduler depends on the number of hardware queues, this forces a
 * reattachment when nr_hw_queues changes.
 */
/*
 * [한국어]
 * elv_update_nr_hw_queues - 하드웨어 큐 개수가 바뀌었을 때 스케줄러를 다시 붙인다
 *
 * @q:   대상 큐. 호출자가 이미 freeze해 둔 상태다.
 * @ctx: 전환 컨텍스트. ctx->type/ctx->name/ctx->res가 미리 채워져 있다.
 * @return: 없음(실패해도 큐는 "none"으로 계속 동작하므로 상위에 보고할
 *          의미 있는 에러가 없다).
 *
 * === 왜 재부착이 필요한가 ===
 * 스케줄러 태그(sched_tags)는 하드웨어 큐마다 하나씩 배열로 잡힌다
 * (blk_mq_alloc_sched_tags의 et->tags[i]). 그런데 hctx 개수가 바뀌면 그
 * 배열의 길이가 실제 hctx 수와 어긋나 버린다. 배열을 그 자리에서 늘리거나
 * 줄이는 것은 진행 중인 I/O와 얽혀 매우 복잡하므로, 스케줄러를 통째로
 * 내렸다가 새 개수로 다시 올리는 쪽을 택했다. 위 영문 주석의
 * "forces a reattachment"가 그 뜻이다.
 *
 * === NVMe에서 언제 일어나는가 ===
 * PCIe NVMe 드라이버가 컨트롤러 리셋 후 I/O 큐 개수를 다시 협상하면
 * blk_mq_update_nr_hw_queues(&dev->tagset, dev->online_queues - 1)를
 * 부른다(drivers/nvme/host/pci.c). 그 안에서 큐마다 이 함수가 불린다.
 * 다만 멀티큐 NVMe의 기본값은 "none"이라 ctx->type이 NULL이고, 그러면
 * 아래 조건에서 걸러져 실제 재부착은 일어나지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 큐는 이미 freeze되어 있고
 * set->update_nr_hwq_lock을 쓰기 모드로 쥔 상태다. 그래서 이 함수는
 * elevator_change()와 달리 freeze를 직접 걸지 않고 해제만 한다.
 *
 * 호출 체인:
 *   (NVMe 등 드라이버) blk_mq_update_nr_hw_queues
 *     → [elv_update_nr_hw_queues] → elevator_switch → elevator_change_done
 */
void elv_update_nr_hw_queues(struct request_queue *q,
		struct elv_change_ctx *ctx)
{
	/* [한국어] 자원 해제 시 필요한 태그 세트. 아래에서 q가 이미 정리되었을
	 * 수도 있으므로 미리 지역 변수에 담아 둔다. */
	struct blk_mq_tag_set *set = q->tag_set;
	/* [한국어] 기본값을 -ENODEV로 둔다. 아래 조건에 걸려 전환을 아예 하지
	 * 않은 경우 "장치가 그럴 상태가 아니었다"는 의미로 남아, 뒤의
	 * elevator_change_done() 호출을 건너뛰게 만든다. */
	int ret = -ENODEV;

	/* [한국어] 호출자가 반드시 freeze한 뒤에 불러야 한다는 불변식 검증.
	 * hctx 배열이 통째로 재구성되는 동안 I/O가 흐르면 안 되기 때문이다. */
	WARN_ON_ONCE(q->mq_freeze_depth == 0);

	/* [한국어] 재부착을 실제로 시도할 조건 세 가지:
	 *   ctx->type            - 원래 붙어 있던 스케줄러가 있어야 한다.
	 *                          "none"이었다면 다시 붙일 것이 없다.
	 *   !blk_queue_dying(q)  - 사라지는 중인 큐에 스케줄러를 붙일 이유가 없다.
	 *   blk_queue_registered(q) - 아직 add_disk() 전이면 sysfs 노드가 없어
	 *                          elv_register_queue()가 실패한다. */
	if (ctx->type && !blk_queue_dying(q) && blk_queue_registered(q)) {
		/* [한국어] 스케줄러 교체 직렬화 락. */
		mutex_lock(&q->elevator_lock);
		/* force to reattach elevator after nr_hw_queue is updated */
		/* [한국어] 같은 이름으로 다시 붙이는 것이지만, elevator_change()의
		 * "이미 같으면 생략" 검사를 거치지 않고 곧장 elevator_switch()를
		 * 부르므로 강제로 재부착된다. 새 nr_hw_queues에 맞춰 미리 할당된
		 * ctx->res가 여기서 연결된다. */
		ret = elevator_switch(q, ctx);
		/* [한국어] 락 해제. */
		mutex_unlock(&q->elevator_lock);
	}
	/* [한국어] freeze를 푼다. _nomemrestore 변형인 이유: freeze를 건 주체가
	 * 이 함수가 아니라 호출자(blk_mq_update_nr_hw_queues)이고, memflags도
	 * 그쪽이 들고 있다. 여기서 memalloc 상태까지 복원해 버리면 아직 끝나지
	 * 않은 상위 작업의 컨텍스트를 망가뜨린다. */
	blk_mq_unfreeze_queue_nomemrestore(q);
	/* [한국어] 재부착에 성공했을 때만 뒷정리(옛 것 해제 + 새 것 sysfs 등록)를
	 * 한다. WARN_ON_ONCE로 감싼 이유: 이 경로에서는 실패를 돌려줄 상위 호출자가
	 * 없으므로, 실패하면 조용히 넘어가지 말고 로그를 남겨야 한다. */
	if (!ret)
		WARN_ON_ONCE(elevator_change_done(q, ctx));

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	/* [한국어] 새 스케줄러가 끝내 붙지 못했다면 미리 할당한 자원이 주인 없이
	 * 남으므로 해제한다. 붙었다면 스케줄러가 소유하므로 건드리면 안 된다. */
	if (!ctx->new)
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);
}

/*
 * Use the default elevator settings. If the chosen elevator initialization
 * fails, fall back to the "none" elevator (no elevator).
 */
/*
 * elevator_set_default - 디스크 등록 시 기본 I/O 스케줄러를 결정해 붙인다
 *
 * @q: 방금 등록되는 디스크의 request_queue
 * @return: 없음. 실패해도 큐는 "none"으로 정상 동작하므로 에러를 올리지 않고
 *          pr_warn만 남긴다.
 *
 * 이 함수가 이 파일에서 NVMe 독자에게 가장 중요한 함수다. "왜 내 NVMe는
 * scheduler가 none인가"의 답이 전부 여기 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 등록 경로). 내부의
 * elevator_change()가 큐를 freeze하므로 잠들 수 있다.
 *
 *   호출 경로: add_disk -> blk_register_queue -> elevator_set_default
 *   NVMe 연결:
 *     - NVMe namespace가 디스크로 등록될 때 호출.
 *     - 단일 hw_queue이거나 shared tags인 경우에만 mq-deadline을 시도하고,
 *       그 외(멀티큐)에는 "none"을 그대로 둔다.
 *
 * === NVMe가 보통 "none"이 되는 실제 이유 ===
 * 조건은 코드 그대로 (q->nr_hw_queues == 1 || blk_mq_is_shared_tags(...)) 이다.
 * NVMe PCIe는 CPU 수에 맞춰 여러 개의 하드웨어 큐(SQ/CQ 쌍)를 만들므로
 * nr_hw_queues > 1 이고 shared tags도 쓰지 않아, 이 조건에 걸리지 않아
 * "none"으로 남는다. NVMe 드라이버가 BLK_MQ_F_NO_SCHED_BY_DEFAULT를 설정해서가
 * 아니다(설정하지 않는다).
 *
 * 왜 멀티큐에 스케줄러를 붙이지 않는가:
 *   I/O 스케줄러의 본래 목적은 회전 디스크에서 탐색(seek)을 줄이려고 요청을
 *   재정렬하는 것이었다. NVMe SSD는 탐색이 없고 내부적으로 수십 개의 채널을
 *   병렬 처리하므로 재정렬 이득이 없다. 반면 스케줄러는 큐 전역 락을 쓰기
 *   때문에 여러 CPU가 동시에 제출할 때 심각한 경합을 만든다. 즉 멀티큐 NVMe에서
 *   스케줄러는 이득 없이 비용만 발생시킨다.
 *   다만 지연 격리나 cgroup 대역폭 제어가 필요하면 사용자가 sysfs로
 *   mq-deadline이나 bfq를 명시적으로 붙일 수 있다.
 *
 * 하드웨어 큐가 하나뿐인 장치(SATA AHCI, 일부 eMMC/UFS, 큐를 하나만 만든
 * NVMe 구성)는 어차피 락이 하나로 직렬화되므로 스케줄러의 재정렬 이득이
 * 비용을 넘어선다. 그래서 mq-deadline이 기본값이 된다.
 */
void elevator_set_default(struct request_queue *q)
{
	/* [한국어] 전환 요청을 담는 스택 구조체. 나머지 필드(old/new/res)는
	 * 지정 초기화 덕분에 0/NULL로 채워지고, 전환 과정에서 채워진다. */
	struct elv_change_ctx ctx = {
		/* [한국어] 커널이 고르는 유일한 기본 스케줄러 이름. 조건이 맞을
		 * 때만 실제로 붙는다(아래 nr_hw_queues 검사 참고). */
		.name = "mq-deadline",
		/* [한국어] udev에 KOBJ_ADD 이벤트를 보내지 않는다. 디스크 등록
		 * 과정에서 이미 디스크 자체의 uevent가 나가므로, 스케줄러 부착으로
		 * 이벤트를 하나 더 만들면 부팅 시 udev 부하만 늘어난다. */
		.no_uevent = true,
	};
	/* [한국어] elevator_change()의 결과를 받아 경고 메시지에 쓸 변수. */
	int err;

	/* now we allow to switch elevator */
	/* [한국어] QUEUE_FLAG_NO_ELV_SWITCH는 "지금은 스케줄러를 바꾸지 말라"는
	 * 일시 금지 플래그다(컨트롤러 리셋 등으로 hctx가 재구성되는 동안 켜진다).
	 * 디스크가 정상 등록되는 이 시점에 해제해, 이후 사용자가 sysfs로
	 * 스케줄러를 바꿀 수 있게 한다. elv_iosched_store()가 이 플래그를 확인한다. */
	blk_queue_flag_clear(QUEUE_FLAG_NO_ELV_SWITCH, q);

	/* [한국어] 드라이버가 "기본적으로 스케줄러를 붙이지 말라"고 명시한 경우
	 * 즉시 반환해 "none" 상태를 유지한다. 스케줄링이 오히려 해로운 장치
	 * (예: 자체적으로 순서를 최적화하는 가상 장치)를 위한 탈출구다.
	 * 참고: NVMe PCIe 드라이버는 이 플래그를 설정하지 않는다. NVMe가 보통
	 * "none"이 되는 이유는 이 플래그가 아니라 아래의 nr_hw_queues 조건이다. */
	if (q->tag_set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT)
		return;

	/*
	 * For single queue devices, default to using mq-deadline. If we
	 * have multiple queues or mq-deadline is not available, default
	 * to "none".
	 */
	/* [한국어] mq-deadline을 찾고 모듈 참조를 잡는다. 이 참조 덕분에 아래
	 * elevator_change()가 도는 동안 모듈이 언로드되지 않는다. */
	ctx.type = elevator_find_get(ctx.name);
	/* [한국어] CONFIG_MQ_IOSCHED_DEADLINE=n으로 빌드했거나 모듈이 없으면
	 * NULL이다. 그때는 아무것도 붙이지 않고 "none"으로 남는다. */
	if (!ctx.type)
		return;

	/* [한국어] ★ NVMe 독자가 봐야 할 바로 그 조건 ★
	 * 두 경우에만 mq-deadline을 붙인다:
	 *   q->nr_hw_queues == 1
	 *     하드웨어 큐가 하나뿐이라 어차피 그 큐에서 직렬화되는 장치.
	 *     소프트웨어 재정렬과 기아 방지의 이득이 락 비용보다 크다.
	 *   blk_mq_is_shared_tags(q->tag_set->flags)
	 *     = flags & BLK_MQ_F_TAG_HCTX_SHARED (block/blk-mq.h).
	 *     여러 hctx가 태그 풀 하나를 나눠 쓰는 장치(SCSI 호스트 단위 공유 등).
	 *     이 경우에도 태그 경합이 이미 존재하므로 스케줄러를 붙일 만하다.
	 *
	 * PCIe NVMe는 둘 다 거짓이다:
	 *   - nvme_alloc_io_tag_set()이 set->nr_hw_queues = ctrl->queue_count - 1
	 *     로 CPU 수만큼(정확히는 컨트롤러가 허용한 I/O 큐 수만큼) 잡는다
	 *     (drivers/nvme/host/core.c). 컨트롤러 리셋 뒤에도
	 *     blk_mq_update_nr_hw_queues(&dev->tagset, dev->online_queues - 1)로
	 *     여러 개를 유지한다(drivers/nvme/host/pci.c).
	 *   - NVMe는 BLK_MQ_F_TAG_HCTX_SHARED를 설정하지 않는다. 하드웨어 큐마다
	 *     독립된 태그 풀을 갖는다.
	 * 따라서 이 if 블록을 통째로 건너뛰고, q->elevator는 NULL —
	 * 곧 "none"인 채로 디스크 등록이 끝난다. */
	if ((q->nr_hw_queues == 1 ||
			blk_mq_is_shared_tags(q->tag_set->flags))) {
		/* [한국어] 조건을 만족하는 장치에서만 실제 부착을 시도한다.
		 * 내부에서 자원 선할당 → freeze → switch → unfreeze → sysfs 등록이
		 * 순서대로 일어난다. */
		err = elevator_change(q, &ctx);
		/* [한국어] 부착 실패(대개 메모리 부족)는 치명적이지 않다. 경고만
		 * 남기고 "none"으로 계속 진행한다 — 스케줄러 없이도 I/O는 된다. */
		if (err < 0)
			pr_warn("\"%s\" elevator initialization, failed %d, falling back to \"none\"\n",
					ctx.name, err);
	}
	/* [한국어] elevator_find_get()으로 잡은 모듈 참조 반납. 부착에 성공했다면
	 * elevator_alloc()이 자체 참조를 이미 하나 더 잡아 두었으므로, 여기서
	 * 놓아도 모듈이 사라지지 않는다. 실패했거나 조건에 걸려 시도조차
	 * 하지 않았다면 이 반납으로 참조가 0이 된다. */
	elevator_put(ctx.type);
}

/*
 * [한국어]
 * elevator_set_none - 큐에서 스케줄러를 떼어 "none" 상태로 만든다
 *
 * @q: 대상 request_queue
 * @return: 없음. 실패해도 되돌릴 방법이 없으므로 경고만 남긴다.
 *
 * 커널 내부에서 "지금은 스케줄러가 있으면 곤란하다"고 판단할 때 쓰는
 * 진입점이다. 이 트리에서의 호출자는 block/blk-mq.c의 hctx 개수 변경
 * 경로로, 스케줄러가 붙은 채로 hctx 배열을 재구성할 수 없기 때문에
 * 먼저 떼어 낸다.
 * (사용자가 sysfs에 "none"을 쓰는 경우는 이 함수가 아니라
 *  elv_iosched_store() → elevator_change()가 직접 처리한다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 내부에서 큐를 freeze하므로 잠들 수 있고,
 * set->update_nr_hwq_lock을 이미 쥔 상태여야 한다(elevator_change의 전제).
 *
 * 호출 체인:
 *   blk-mq의 hctx 재구성 경로 → [elevator_set_none] → elevator_change
 *     → elevator_switch("none")
 */
void elevator_set_none(struct request_queue *q)
{
	/* [한국어] "none"은 elv_list에 없는 특별한 이름이라, elevator_switch()의
	 * strncmp(ctx->name, "none", 4) 검사에서 걸러져 new_e가 NULL로 남고
	 * 스케줄러를 붙이지 않는 분기로 간다. ctx.type도 NULL 그대로다. */
	struct elv_change_ctx ctx = {
		.name	= "none",
	};
	/* [한국어] 전환 결과. 실패 시 경고 메시지에 쓴다. */
	int err;

	/* [한국어] 공통 전환 경로를 그대로 탄다. 자원 선할당은 "none"이라 생략되고,
	 * freeze → quiesce → elevator_exit → 큐 파라미터 원복 → unquiesce 순으로
	 * 진행된다. */
	err = elevator_change(q, &ctx);
	/* [한국어] "none"으로 가는 길에는 메모리 할당이 없어 사실상 실패하지
	 * 않지만, 만일을 대비해 로그를 남긴다. 호출자가 처리할 수 있는 실패가
	 * 아니므로 반환값을 올리지 않는다. */
	if (err < 0)
		pr_warn("%s: set none elevator failed %d\n", __func__, err);
}

/*
 * [한국어]
 * elv_iosched_load_module - 아직 로드되지 않은 스케줄러 모듈을 자동 로드
 *
 * @elevator_name: 로드할 스케줄러 이름 (예: "bfq", "mq-deadline")
 *
 * 사용자가 sysfs로 특정 스케줄러를 요청했을 때, 해당 스케줄러가 아직
 * elv_list에 없으면 "<name>-iosched" 모듈을 request_module()로 자동 로드한다.
 * 이렇게 하면 "bfq-iosched.ko"를 명시적으로 insmod하지 않아도 sysfs 쓰기만으로
 * BFQ를 활성화할 수 있다. 모듈 로드 이후 elv_list에 등록되므로
 * 이후 elevator_find_get()이 성공한다.
 *
 * 호출 체인:
 *   elv_iosched_store → [elv_iosched_load_module] → request_module()
 */
static void elv_iosched_load_module(const char *elevator_name)
{
	/* [한국어] 조회 결과. 여기서는 존재 여부만 보므로 참조를 잡지 않는다
	 * (elevator_find_get이 아니라 __elevator_find를 쓰는 이유). 락을 푼 뒤
	 * 이 포인터를 역참조하면 안 되고, 실제로 NULL 비교에만 쓴다. */
	struct elevator_type *found;

	/* [한국어] 목록 보호 락. 조회만 하므로 구간이 아주 짧다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 이름으로 등록 여부를 확인한다. 별칭도 함께 비교되므로
	 * "deadline"을 써도 이미 로드된 mq-deadline을 찾아낸다. */
	found = __elevator_find(elevator_name);
	/* [한국어] 락 해제. request_module()은 사용자 공간 modprobe를 실행하며
	 * 오래 잠들 수 있으므로 절대 락 안에서 부르면 안 된다. */
	spin_unlock(&elv_list_lock);

	/* [한국어] 못 찾았다면 아직 모듈이 로드되지 않았을 수 있다. */
	if (!found)
		/* [한국어] "bfq" → "bfq-iosched" 처럼 관례적인 모듈 이름을 만들어
		 * modprobe를 요청한다. 반환값을 확인하지 않는 이유: 정말로 존재하지
		 * 않는 이름이면 뒤이은 elevator_find_get()이 NULL을 돌려주고
		 * 사용자에게 -EINVAL이 전달되므로, 여기서 따로 에러를 낼 필요가 없다.
		 * 실행 컨텍스트상 이 호출은 잠들 수 있다(사용자 공간 헬퍼 실행 대기). */
		request_module("%s-iosched", elevator_name);
}

/*
 * [한국어]
 * elv_iosched_store - /sys/block/<disk>/queue/scheduler 에 쓰기가 들어왔을 때
 *
 * @disk:  대상 디스크(NVMe라면 nvme0n1 등). q = disk->queue.
 * @buf:   사용자가 쓴 문자열. 끝에 개행이 붙어 있을 수 있다.
 * @count: 그 길이.
 * @return: 성공 시 count(소비한 바이트 수), 실패 시 음수 errno.
 *          -ENOENT(큐가 등록되지 않았거나 전환이 금지됨),
 *          -EBUSY(hw 큐 수 변경과 경합), -EINVAL(없는 스케줄러 이름).
 *
 * sysfs 파일 자체는 block/blk-sysfs.c의 큐 attribute 테이블이 만들고,
 * 이 함수는 그 store 콜백으로 연결된다. 즉 사용자가
 *   echo mq-deadline > /sys/block/nvme0n1/queue/scheduler
 *   echo none        > /sys/block/nvme0n1/queue/scheduler
 * 를 실행하면 여기로 들어온다.
 *
 * NVMe 관점: 멀티큐 NVMe의 기본값은 elevator_set_default()가 결정한 "none"인데,
 * 그것을 뒤집는 유일한 경로가 이 함수다. 즉 NVMe에 mq-deadline이나 bfq를
 * 붙이는 것은 커널이 알아서 하는 일이 아니라 사용자(또는 udev 규칙)의
 * 명시적 선택이다. 여기서는 nr_hw_queues 검사를 하지 않으므로, 멀티큐
 * NVMe에도 원하면 스케줄러를 붙일 수 있다 — 커널이 막지는 않는다.
 * 다만 그 대가로 큐 전역 스케줄러 락 경합이 생긴다.
 *
 * 처리 순서(각 단계가 왜 그 자리에 있는지는 아래 인라인 주석 참고):
 *   등록 확인 → 이름 정리 → 모듈 로드 → 타입 참조 획득
 *   → update_nr_hwq_lock 읽기 trylock → elevator_change → 락 해제 → 참조 반납
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 write). kernfs의 active 참조를
 * 이미 쥔 상태로 진입하므로 락 순서에 주의해야 한다(아래 trylock 참고).
 *
 * 호출 체인:
 *   sysfs write → blk-sysfs.c의 queue attr store → [elv_iosched_store]
 *     → elv_iosched_load_module → elevator_find_get → elevator_change
 */
ssize_t elv_iosched_store(struct gendisk *disk, const char *buf,
			  size_t count)
{
	/* [한국어] 사용자 입력을 담을 고정 크기 스택 버퍼. ELV_NAME_MAX는
	 * 스케줄러 이름의 상한이라, 이보다 긴 입력은 어차피 유효하지 않다. */
	char elevator_name[ELV_NAME_MAX];
	/* [한국어] 전환 컨텍스트. {}로 전 필드를 0/NULL 초기화해 두어야
	 * ctx.new/ctx.old 판정과 out 라벨의 ctx.type 검사가 안전하다. */
	struct elv_change_ctx ctx = {};
	/* [한국어] 반환값. 성공 시 count로 덮어쓴다. */
	int ret;
	/* [한국어] 실제 조작 대상 큐. */
	struct request_queue *q = disk->queue;
	/* [한국어] update_nr_hwq_lock을 잡기 위한 태그 세트 포인터.
	 * NVMe라면 nvme_ctrl이 소유한 tagset이며, 같은 컨트롤러의 모든
	 * 네임스페이스 큐가 이 하나를 공유한다 — 그래서 이 락이 컨트롤러 단위
	 * 직렬화 지점이 된다. */
	struct blk_mq_tag_set *set = q->tag_set;

	/* Make sure queue is not in the middle of being removed */
	/* [한국어] 큐가 sysfs에 등록된 상태인지 확인한다. 장치 제거(del_gendisk)가
	 * 진행 중이면 이 플래그가 이미 내려가 있어, 사라지는 중인 큐의 스케줄러를
	 * 바꾸려는 무의미하고 위험한 시도를 막는다. NVMe 컨트롤러가 갑자기
	 * 뽑히는(surprise removal) 상황에서 실제로 발생할 수 있는 경쟁이다. */
	if (!blk_queue_registered(q))
		return -ENOENT;

	/*
	 * If the attribute needs to load a module, do it before freezing the
	 * queue to ensure that the module file can be read when the request
	 * queue is the one for the device storing the module file.
	 */
	/* [한국어] 사용자 입력을 고정 크기 스택 버퍼로 복사한다. strscpy는 항상
	 * NUL로 끝맺고 넘치면 잘라 내므로, sysfs로 들어온 임의 길이 입력에
	 * 안전하다(strcpy나 strncpy와 달리). */
	strscpy(elevator_name, buf, sizeof(elevator_name));
	/* [한국어] 앞뒤 공백과 개행을 제거한다. `echo mq-deadline > scheduler`는
	 * 끝에 '\n'을 붙여 쓰므로 이 처리가 없으면 이름 비교가 항상 실패한다.
	 * strstrip은 버퍼 안에서 앞쪽 공백을 건너뛴 포인터를 돌려주고 뒤쪽은
	 * NUL로 잘라 낸다. */
	ctx.name = strstrip(elevator_name);

	/* [한국어] 필요하면 스케줄러 모듈을 먼저 로드한다. 위 영문 주석이 밝히듯
	 * 이것을 큐 freeze "전에" 해야 한다 — 모듈 파일이 바로 이 블록 장치에
	 * 저장되어 있을 수 있는데, freeze된 큐에서 모듈을 읽으려 하면 자기 자신을
	 * 기다리는 데드락이 된다. */
	elv_iosched_load_module(ctx.name);
	/* [한국어] 이름으로 스케줄러 타입을 찾고 모듈 참조를 획득한다.
	 * "none"이면 NULL이 반환되는데 이는 오류가 아니라 정상 경로다. */
	ctx.type = elevator_find_get(ctx.name);

	/*
	 * Use trylock to avoid circular lock dependency with kernfs active
	 * reference during concurrent disk deletion:
	 *   update_nr_hwq_lock -> kn->active (via del_gendisk -> kobject_del)
	 *   kn->active -> update_nr_hwq_lock (via this sysfs write path)
	 */
	/* [한국어] 하드웨어 큐 수 변경(blk_mq_update_nr_hw_queues)과의 동시 실행을
	 * 막는 읽기 락을 잡는다. 스케줄러 자원은 hw 큐 개수만큼 할당되므로,
	 * 할당 도중에 개수가 바뀌면 크기가 어긋난다.
	 *
	 * 일반 down_read가 아니라 trylock을 쓰는 이유는 위 영문 주석의 순환 의존
	 * 때문이다:
	 *   디스크 삭제 경로: update_nr_hwq_lock → kn->active (kobject_del)
	 *   이 sysfs 쓰기 경로: kn->active(이미 보유) → update_nr_hwq_lock
	 * 두 경로가 락을 반대 순서로 잡으므로 서로 기다리면 데드락이다.
	 * trylock으로 즉시 포기하고 -EBUSY를 돌려주면 사용자가 재시도하면 된다.
	 * NVMe 컨트롤러 리셋 중에 스케줄러를 바꾸려 하면 실제로 이 -EBUSY를 본다. */
	if (!down_read_trylock(&set->update_nr_hwq_lock)) {
		/* [한국어] 지금 누군가 hw 큐 수를 바꾸는 중이다. 기다리면 데드락
		 * 위험이 있으므로 즉시 포기하고 사용자에게 "바쁘다"를 알린다. */
		ret = -EBUSY;
		/* [한국어] 락을 잡지 못했으므로 up_read()를 건너뛰고, 이미 획득한
		 * ctx.type 참조만 반납하러 간다. */
		goto out;
	}
	/* [한국어] QUEUE_FLAG_NO_ELV_SWITCH는 "지금은 스케줄러를 바꾸지 말라"는
	 * 일시적 금지 표시로, 디스크 등록 전이나 하드웨어 큐 재구성 중에 설정된다.
	 * 이 플래그가 없을 때만 실제 전환을 수행한다. */
	if (!blk_queue_no_elv_switch(q)) {
		/* [한국어] 실제 전환. ctx.name이 "none"이면 스케줄러를 떼고,
		 * 아니면 ctx.type의 스케줄러를 붙인다. 없는 이름이면 내부의
		 * elevator_switch()가 -EINVAL을 돌려준다. */
		ret = elevator_change(q, &ctx);
		/* [한국어] sysfs write 핸들러의 관례상 "소비한 바이트 수"를 반환해야
		 * 한다. 성공했으면 입력 전체를 소비한 것이므로 count를 돌려준다.
		 * 그렇지 않으면 사용자 공간의 write()가 부분 쓰기로 오해해 무한 재시도한다. */
		if (!ret)
			ret = count;
	} else {
		/* [한국어] 전환이 일시 금지된 상태(디스크 등록 전이거나 hctx 재구성 중).
		 * -ENOENT로 "지금은 그런 요청을 받을 수 없다"를 알린다. */
		ret = -ENOENT;
	}
	/* [한국어] 읽기 락 해제. 이 시점부터 하드웨어 큐 수 변경이 가능해진다. */
	up_read(&set->update_nr_hwq_lock);

out:
	/* [한국어] 공통 정리 지점. "none"을 요청했다면 ctx.type이 NULL이라
	 * 반납할 참조가 없다. */
	if (ctx.type)
		/* [한국어] elevator_find_get()이 잡은 모듈 참조를 돌려준다.
		 * 전환에 성공했다면 elevator_alloc()이 별도 참조를 쥐고 있으므로
		 * 모듈은 계속 살아 있다. */
		elevator_put(ctx.type);
	/* [한국어] count(성공) 또는 음수 errno를 VFS에 반환한다. */
	return ret;
}

/*
 * [한국어]
 * elv_iosched_show - /sys/block/<disk>/queue/scheduler 를 읽었을 때 출력 생성
 *
 * @disk: 대상 디스크. q = disk->queue.
 * @name: 출력 버퍼(PAGE_SIZE). 여기에 문자열을 만들어 넣는다.
 * @return: 쓴 바이트 수.
 *
 * 출력 형식: 선택 가능한 이름들을 공백으로 나열하고, 현재 적용된 것만
 * 대괄호로 감싼다. 멀티큐 NVMe의 전형적인 출력은 다음과 같다.
 *
 *   # cat /sys/block/nvme0n1/queue/scheduler
 *   [none] mq-deadline kyber bfq
 *
 * 대괄호가 none에 있다는 것이 곧 "이 장치에는 I/O 스케줄러가 붙어 있지 않다"
 * 이며, 그 이유는 elevator_set_default()의 nr_hw_queues 조건이다.
 * 단일 큐 장치라면 [mq-deadline] 처럼 나온다.
 * 나열되는 이름은 elv_list에 등록된 것 전부이므로, 커널 빌드 구성과
 * 로드된 모듈에 따라 달라진다("none"은 목록에 없지만 항상 선택 가능해서
 * 아래 코드가 손으로 먼저 찍어 준다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 read). 뮤텍스와 스핀락을
 * 차례로 잡는다 — 순서가 항상 elevator_lock → elv_list_lock이어야
 * 다른 경로와 락 순서가 어긋나지 않는다.
 *
 * 호출 체인:
 *   sysfs read → blk-sysfs.c의 queue attr show → [elv_iosched_show]
 */
ssize_t elv_iosched_show(struct gendisk *disk, char *name)
{
	/* [한국어] 현재 스케줄러를 읽을 대상 큐. */
	struct request_queue *q = disk->queue;
	/* [한국어] cur = 현재 적용된 스케줄러 타입(없으면 NULL 유지),
	 * e = 목록 순회 커서. cur를 NULL로 초기화하는 것이 중요하다 —
	 * "none" 상태에서는 아래 비교(e == cur)가 절대 참이 되지 않아야 한다. */
	struct elevator_type *cur = NULL, *e;
	/* [한국어] 지금까지 버퍼에 쓴 바이트 수. sprintf 반환값을 누적해
	 * 다음 쓰기 위치(name+len)를 계산한다. */
	int len = 0;

	/* [한국어] q->elevator 포인터를 읽는 동안 다른 CPU가 스케줄러를 교체하지
	 * 못하게 막는다. 이 락 없이 읽으면 cur가 방금 해제된 타입을 가리킬 수 있다. */
	mutex_lock(&q->elevator_lock);
	/* [한국어] 스케줄러가 붙어 있지 않은 상태 = "none". */
	if (!q->elevator) {
		/* [한국어] none이 현재 선택이므로 대괄호를 씌워 먼저 찍는다.
		 * 멀티큐 NVMe의 기본 출력이 바로 이 줄에서 만들어진다. */
		len += sprintf(name+len, "[none] ");
	} else {
		/* [한국어] 스케줄러가 있으면 none은 "선택 가능하지만 지금은 아닌"
		 * 항목이므로 대괄호 없이 찍는다. none은 elv_list에 없어서
		 * 아래 순회로는 절대 출력되지 않기 때문에 여기서 손으로 넣어야 한다. */
		len += sprintf(name+len, "none ");
		/* [한국어] 현재 타입을 기억해 두었다가 아래 순회에서 이것과 같은
		 * 항목에만 대괄호를 씌운다. */
		cur = q->elevator->type;
	}

	/* [한국어] 전역 등록 목록을 순회하는 동안 모듈이 등록/해제되지 않도록
	 * 스핀락을 잡는다. 이미 elevator_lock(뮤텍스)을 쥔 상태에서 스핀락을
	 * 잡는 순서이며, 이 구간에서는 잠들 수 없다(sprintf는 잠들지 않는다). */
	spin_lock(&elv_list_lock);
	/* [한국어] 등록된 모든 스케줄러를 등록 순서대로 출력한다. */
	list_for_each_entry(e, &elv_list, list) {
		/* [한국어] 이 항목이 현재 적용된 스케줄러인지 포인터로 비교한다.
		 * 문자열 비교가 아니라 포인터 비교라 별칭 혼동이 없다. */
		if (e == cur)
			/* [한국어] 현재 선택 — 대괄호로 강조(예: [mq-deadline]). */
			len += sprintf(name+len, "[%s] ", e->elevator_name);
		/* [한국어] 나머지는 선택 가능한 후보들이다. */
		else
			/* [한국어] 이름만 출력. 사용자는 이 중 하나를 골라
			 * 같은 파일에 echo 하면 된다(elv_iosched_store). */
			len += sprintf(name+len, "%s ", e->elevator_name);
	}
	/* [한국어] 목록 순회 완료 — 스핀락 해제. */
	spin_unlock(&elv_list_lock);

	/* [한국어] sysfs 출력 관례상 마지막에 개행을 넣는다. cat 결과가
	 * 프롬프트와 붙어 나오지 않게 하는 것이자, 사용자 공간 파서가
	 * 줄 단위로 읽을 수 있게 하는 규약이다. */
	len += sprintf(name+len, "\n");
	/* [한국어] 뮤텍스 해제. 이 시점부터 스케줄러 교체가 가능해진다. */
	mutex_unlock(&q->elevator_lock);

	/* [한국어] 총 출력 길이를 VFS에 반환한다. 버퍼는 PAGE_SIZE라
	 * 스케줄러 개수가 몇 개 안 되는 현실에서는 넘칠 일이 없다. */
	return len;
}

/*
 * [한국어]
 * elv_rb_former_request - LBA 기준 RB-tree에서 @rq 바로 앞의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 작은 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 front-merge 후보를 찾거나 dispatch 순서를
 * 역방향으로 추적할 때 사용한다. rb_prev()는 O(log N)이다.
 * elv_former_request()의 실제 구현체이며 스케줄러가 ops.former_request로
 * 이 함수를 등록한다.
 *
 * 호출 체인:
 *   elv_former_request → 스케줄러 ops.former_request → [elv_rb_former_request]
 */
struct request *elv_rb_former_request(struct request_queue *q,
				      struct request *rq)
{
	/* [한국어] 중위 순회 기준 직전 노드를 찾는다. 트리가 LBA 오름차순으로
	 * 정렬되어 있으므로 "LBA가 바로 작은 request"가 된다. rb_prev는
	 * 왼쪽 서브트리의 최대값 또는 조상 중 첫 "오른쪽 자식이었던" 지점을
	 * 찾는 방식이라 O(log n)이다. */
	struct rb_node *rbprev = rb_prev(&rq->rb_node);

	/* [한국어] 앞 노드가 존재하면 그것을 품은 request로 변환해 반환한다. */
	if (rbprev)
		/* [한국어] rb_entry_rq는 rb_node → struct request 역산 매크로. */
		return rb_entry_rq(rbprev);

	/* [한국어] rq가 트리에서 가장 작은 LBA였다 — 앞선 이웃이 없다.
	 * 호출자는 이 방향의 병합 시도를 포기한다. */
	return NULL;
}
EXPORT_SYMBOL(elv_rb_former_request);

/*
 * [한국어]
 * elv_rb_latter_request - LBA 기준 RB-tree에서 @rq 바로 뒤의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 큰 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 back-merge 후보 및 dispatch 방향 탐색에 사용.
 * elv_latter_request()의 실제 구현체이며 ops.next_request로 등록된다.
 *
 * 호출 체인:
 *   elv_latter_request → 스케줄러 ops.next_request → [elv_rb_latter_request]
 */
struct request *elv_rb_latter_request(struct request_queue *q,
				      struct request *rq)
{
	/* [한국어] 중위 순회 기준 다음 노드 = LBA가 바로 큰 request. O(log n). */
	struct rb_node *rbnext = rb_next(&rq->rb_node);

	/* [한국어] 뒤 노드가 존재하면 그것을 품은 request로 변환해 반환한다. */
	if (rbnext)
		/* [한국어] rb_node → struct request 역산. */
		return rb_entry_rq(rbnext);

	/* [한국어] rq가 트리에서 가장 큰 LBA였다 — 뒤따르는 이웃이 없다. */
	return NULL;
}
EXPORT_SYMBOL(elv_rb_latter_request);

/*
 * [한국어]
 * elevator_setup - 부팅 커널 파라미터 "elevator=" 를 받아 경고만 남긴다
 *
 * @str: 파라미터 값(예: "elevator=deadline"의 "deadline"). 무시된다.
 * @return: 1. __setup 관례상 "이 파라미터를 처리했다"는 뜻으로, init 환경변수
 *          목록에 넘기지 말라는 신호다.
 *
 * 단일 큐 시절에는 이 파라미터로 시스템 전체의 기본 스케줄러를 정할 수
 * 있었다. blk-mq로 전환되면서 장치마다 하드웨어 큐 수와 특성이 크게
 * 달라졌고, 시스템 전역으로 하나를 강제하는 것이 무의미해져 폐기되었다.
 * (예: 같은 시스템에 단일 큐 SATA HDD와 멀티큐 NVMe가 함께 있으면
 *  두 장치에 맞는 선택이 서로 다르다.)
 * 대신 장치마다 sysfs로 지정한다 — 보통 udev 규칙으로 자동화한다.
 *
 * 실행 컨텍스트: 부팅 초기 파라미터 파싱(__init 섹션, 단일 스레드).
 *
 * 호출 체인:
 *   커널 부팅 파라미터 파서 → __setup 테이블 → [elevator_setup]
 */
static int __init elevator_setup(char *str)
{
	/* [한국어] dmesg에 "이 파라미터는 이제 아무 효과가 없다"고 알리고,
	 * sysfs를 쓰라고 안내한다. 조용히 무시하면 사용자가 설정이 적용된 줄
	 * 착각하므로 반드시 경고를 남긴다. */
	pr_warn("Kernel parameter elevator= does not have any effect anymore.\n"
		"Please use sysfs to set IO scheduler for individual devices.\n");
	/* [한국어] 1을 반환해 "처리 완료"를 알린다. 0을 반환하면 커널이 이
	 * 문자열을 init 프로세스의 환경변수로 넘긴다. */
	return 1;
}

/* [한국어] "elevator=" 로 시작하는 부팅 파라미터를 위 함수에 연결한다.
 * __setup 매크로는 .init.setup 섹션에 { 문자열, 핸들러 } 쌍을 등록하고,
 * 부팅 시 파라미터 파서가 그 테이블을 훑으며 일치하는 핸들러를 부른다. */
__setup("elevator=", elevator_setup);

/*
 * [한국어] === NVMe 독자를 위한 이 파일의 핵심 요약 ===
 *
 * 1) 멀티큐 NVMe의 기본값은 스케줄러 없음("none")이다.
 *    근거는 elevator_set_default()의
 *      if (q->nr_hw_queues == 1 || blk_mq_is_shared_tags(...))
 *    조건이다. PCIe NVMe는 하드웨어 큐를 CPU 수만큼 만들고 태그도 공유하지
 *    않으므로 이 조건이 거짓이 되어, 스케줄러를 붙이는 elevator_change()
 *    호출 자체가 실행되지 않는다. q->elevator는 NULL로 남는다.
 *    BLK_MQ_F_NO_SCHED_BY_DEFAULT 때문이 아니다 — 그 플래그를 쓰는 드라이버는
 *    이 트리에서 loop와 null_blk뿐이다.
 *
 * 2) 왜 그것이 옳은가.
 *    스케줄러의 원래 목적인 "탐색 시간을 줄이는 LBA 재정렬"은 SSD에서
 *    이득이 거의 없는데, 스케줄러 인스턴스는 큐당 하나뿐이라 모든 CPU의
 *    삽입/디스패치가 그 하나의 락(mq-deadline이면 dd->lock)으로 직렬화된다.
 *    NVMe처럼 CPU마다 하드웨어 큐가 있는 장치에서는 이 직렬화가 그대로
 *    병목이 된다. 이득은 작고 비용은 큰 거래라 붙이지 않는 것이다.
 *
 * 3) 그래도 붙일 수 있고, 붙이는 이유도 있다.
 *    elv_iosched_store()에는 nr_hw_queues 검사가 없다. 사용자가
 *      echo mq-deadline > /sys/block/nvme0n1/queue/scheduler
 *    를 하면 멀티큐 NVMe에도 스케줄러가 붙는다. 읽기 지연 상한을 보장하거나
 *    (mq-deadline의 read_expire), cgroup 단위 대역폭 비례 배분이 필요할 때
 *    (bfq) 의도적으로 선택한다.
 *
 * 4) 병합이 NVMe에 주는 실제 효과.
 *    elv_merge()/elv_attempt_insert_merge()가 bio 여러 개를 한 request로
 *    묶으면, 그 request는 나중에 NVMe 커맨드 하나가 된다. 즉 SQ 엔트리,
 *    Command ID, 완료 CQ 엔트리가 각각 하나로 줄어든다. 줄어드는 것은
 *    커맨드 "개수"이며, 한 커맨드가 서술하는 데이터가 커지므로 그 커맨드의
 *    PRP/SGL 엔트리 수는 오히려 늘어난다.
 *    discard는 특별하다 — 여러 bio가 한 request에 담기면 NVMe 쪽에서
 *    Dataset Management 커맨드 하나에 여러 range로 들어간다
 *    (drivers/nvme/host/core.c의 nvme_setup_discard).
 *
 * 5) 이 파일과 NVMe 드라이버 사이에 직접 호출은 없다.
 *    elevator.c에는 nvme 식별자가 하나도 없다. 스케줄러가 고른 request는
 *    blk-mq를 거쳐 q->mq_ops->queue_rq() 라는 간접 호출로 드라이버에
 *    전달되며, PCIe NVMe에서는 그 함수 포인터가 nvme_queue_rq다.
 *
 * 6) 스케줄러 태그는 NVMe Command ID가 아니다.
 *    스케줄러가 붙으면 request는 먼저 sched_tags에서 슬롯을 받고, 실제
 *    드라이버로 나갈 때 별도로 드라이버 태그를 받는다. NVMe Command ID로
 *    쓰이는 것은 후자다. 자세한 내용은 block/blk-mq-sched.c 참고.
 */
