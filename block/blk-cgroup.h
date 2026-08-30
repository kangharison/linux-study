/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] block cgroup(blkcg) 코어 자료구조 및 정책 등록 인터페이스 (blk-cgroup.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 include/linux/blk-cgroup.h 가 공개하는 blkcg 퍼블릭 인터페이스의
 * 배후에서 실제 자료구조(struct blkcg, struct blkcg_gq, struct blkg_policy_data,
 * struct blkcg_policy 등)를 정의하는 block cgroup 의 "private" 헤더다. cgroup
 * v1(blkio)/v2(io) 컨트롤러가 request_queue 단위로 I/O 를 분류·제한·계층화하기
 * 위한 핵심 연결 구조인 blkcg_gq(blkg)와, blk-throttle/BFQ/blk-ioprio 같은
 * 개별 정책이 (cgroup, request_queue) 쌍마다 사설 상태를 붙일 수 있게 하는
 * blkg_policy_data/blkcg_policy_data 프레임워크, 그리고 정책을 커널에 등록하는
 * blkcg_policy 콜백 테이블을 한곳에 모아 정의한다. block/blk-cgroup.c 가 이
 * 헤더의 자료구조를 실체화(할당/해제/순회)하는 구현체이고, block/blk-mq.c,
 * block/blk-throttle.c, block/bfq-iosched.c, block/blk-ioprio.c 는 이 헤더를
 * include 해 자신의 per-blkg 상태를 pd[] 배열에 매달아 쓰는 소비자다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * I/O 제출 경로에서 이 헤더의 자료구조는 bio 가 실제 NVMe/SCSI 등 하위 장치로
 * 내려가기 직전, "이 bio 가 어느 cgroup 소속이고 그 cgroup 에 대해 어떤 정책
 * 상태를 적용해야 하는가"를 결정하는 지점에 위치한다.
 *   submit_bio() -> bio_associate_blkg()[blk-cgroup.c] -> bio->bi_blkg 설정
 *     -> blk_mq_submit_bio() -> blk_cgroup_bio_start()[본 헤더 선언]
 *     -> blk_throtl_bio()/bfq_bio_merge() 등 정책 콜백이 blkg->pd[plid] 참조
 *     -> blk_mq_get_request() -> nvme_queue_rq() (혹은 scsi_queue_rq 등) -> 장치 doorbell
 * cgroup 계층 변경 경로에서는:
 *   mkdir /sys/fs/cgroup/.../io -> blkcg_css_alloc()[blk-cgroup.c]
 *     -> blkg_create()/blkg_alloc() -> 이 헤더의 struct blkcg_gq 인스턴스 생성
 *     -> pol->pd_alloc_fn()/pd_init_fn() -> 정책별 pd 연결
 * 정책 등록 경로에서는:
 *   blk_throtl_init()/bfq_init() -> blkcg_policy_register()[본 헤더 선언]
 *     -> blkcg_activate_policy() -> 기존 모든 blkg 에 pd_alloc_fn() 호출
 * 실행 컨텍스트: 대부분 태스크(프로세스) 컨텍스트에서 bio 제출 시 호출되고,
 * blkg 참조 카운트 해제(blkg_put)는 인터럽트/소프트IRQ 완료 경로에서도
 * 일어날 수 있어 percpu_ref 와 RCU 로 lock-free 하게 설계되어 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blk-cgroup.h : bio_blkcg_css(), bio_associate_blkg() 등
 *     퍼블릭 API 및 이 헤더가 확장하는 기본 타입 선언
 *   - include/linux/cgroup.h : cgroup_subsys_state, css_for_each_descendant_* -
 *     cgroup 계층 트리 자체는 cgroup 서브시스템이 관리하고 blkcg 는 그 위에
 *     I/O 전용 상태를 얹는다
 *   - block/blk.h : request_queue, QUEUE_FLAG_* - request_queue 의 blkcg_pols
 *     비트마스크와 root_blkg 필드가 여기서 참조된다
 *   - linux/blk-mq.h : blk-mq 의 hctx/tags/request 구조 - request->bio->bi_blkg
 *     체인으로 정책 판단 결과가 실제 request 디스패치까지 전달된다
 * 이 헤더에 의존하는 모듈(구현/소비자):
 *   - block/blk-cgroup.c : 본 헤더가 선언한 모든 함수의 구현체이자
 *     struct blkcg_gq/struct blkcg 라이프사이클 관리 주체
 *   - block/blk-throttle.c, block/bfq-iosched.c, block/blk-ioprio.c :
 *     blkcg_policy 를 등록하고 blkg_policy_data 를 상속해 자신만의
 *     throtl_grp/bfq_group/ioprio_blkg 를 blkg->pd[plid] 에 매단다
 *   - block/blk-cgroup-fc-appid.c : struct blkcg 의 fc_app_id 필드를 읽고 써서
 *     FC-NVMe 상위 계층에 애플리케이션 식별자를 전달한다
 * 데이터 흐름: bio -> bio->bi_blkg(=blkcg_gq*) -> blkg->pd[plid](정책 사설 데이터)
 *   -> 정책이 허용/지연/우선순위 결정 -> request 로 변환되어 장치 드라이버 큐로 전달.
 *   완료(CQ/인터럽트) 시에는 역방향으로 blkg->iostat_cpu(percpu) -> lhead(lockless
 *   list) -> blkg->iostat(전역) 순으로 통계가 집계되어 cgroupfs io.stat 로 노출된다.
 * 공유 핵심 자료구조: struct blkcg_gq(blkg) 는 (blkcg, request_queue) 쌍마다 하나씩
 *   존재하며 blkcg->blkg_tree(radix tree, key=q->id)와 q->blkg_list 양쪽에서
 *   동시에 참조되는 교차점이다. struct blkcg 는 하나의 cgroup 이 여러
 *   request_queue 각각에 대해 갖는 blkg 들의 컨테이너 역할을 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * blkg_lookup()             - (blkcg, request_queue) 쌍으로 blkg 를 RCU 하에서 빠르게 조회 (hint+radix tree)
 * blkg_get()/blkg_tryget()/blkg_put() - blkg 의 percpu_ref 참조 카운트 증감; blkg 생존 기간 관리
 * blkg_to_pd()/pd_to_blkg()/blkcg_to_cpd()/cpd_to_blkcg() - blkg·정책 사설 데이터 간 상호 변환
 * blkcg_use_delay()/blkcg_unuse_delay()/blkcg_set_delay()/blkcg_clear_delay() - cgroup 단위 IO 지연(백프레셔) on/off
 * blk_cgroup_mergeable() - 두 IO(bio/request)가 동일 cgroup·동일 root 발행 여부일 때만 merge 허용
 * blkcg_policy_register()/unregister(), blkcg_activate_policy()/deactivate_policy() - 정책을 커널/특정 디스크에 등록·활성화
 * blkg_conf_prep()/blkg_conf_exit() - "MAJ:MIN 값" 형식의 cgroup 설정 파일 입력을 파싱해 대상 blkg 를 확정
 *
 * 핵심 자료구조:
 *   struct blkcg_gq(blkg) - q(request_queue), blkcg(소속 cgroup), parent(계층상 부모 blkg),
 *     refcnt(percpu_ref), iostat_cpu/iostat(통계), pd[](정책별 사설 데이터),
 *     use_delay/delay_nsec/delay_start(지연 스로틀 상태) 보유
 *   struct blkcg - css(cgroup 서브시스템 상태), blkg_tree/blkg_list(이 cgroup 이 가진
 *     모든 blkg), cpd[](정책별 per-cgroup 데이터), lhead(percpu lockless 통계 큐)
 *   struct blkg_policy_data / struct blkcg_policy_data - 정책이 blkg/blkcg 에 각각
 *     매다는 사설 데이터의 공통 헤더(정책은 이를 embed 한 더 큰 구조체를 할당)
 *   struct blkcg_policy - 정책 등록 시 제공하는 cftype 배열과 cpd/pd 할당·초기화·
 *     온라인/오프라인·해제·통계 콜백 테이블
 */
#ifndef _BLK_CGROUP_PRIVATE_H
#define _BLK_CGROUP_PRIVATE_H	/* [한국어] 인클루드 가드. 바로 위 #ifndef 와 짝을 이뤄, 이 헤더가 한 번역 단위에
				 * 여러 번 포함돼도 내용이 한 번만 펼쳐지게 한다(중복 정의 오류 방지). */
/*
 * block cgroup private header
 *
 * Based on ideas and code from CFQ, CFS and BFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2009 Vivek Goyal <vgoyal@redhat.com>
 * 	              Nauman Rafique <nauman@google.com>
 */

#include <linux/blk-cgroup.h> /* blkcg 정책/헬퍼 인라인 함수 포함 -> bio->request 변환 시 cgroup 결정 */
#include <linux/cgroup.h>     /* cgroup_subsys_state, css_for_each_* 등 cgroup 계층 순회용 */
#include <linux/kthread.h>    /* kthread 관련 (blkcg punt bio work 처리에 간접 사용) */
#include <linux/blk-mq.h>     /* blk-mq hctx/tags/request 구조체 -> NVMe multi-queue 매핑에 직접 연결 */
#include <linux/llist.h>      /* lockless list -> percpu iostat flush 경로에서 cache-friendly 연결 */
#include "blk.h"              /* request_queue, QUEUE_FLAG_* 등 queue 상태/깃발 정의 */

/*
 * 이 헤더는 include/linux/blk-cgroup.h 의 낮은 수준 보조 헤더입니다.
 * block/blk-mq.c, block/blk.h, block/elevator.c, block/blk-throttle.c,
 * block/bfq-iosched.c 등과 함께 동작하며, NVMe I/O가 blk_mq_submit_bio()를
 * 거쳐 request_queue -> blk_mq_hw_ctx -> nvme_queue_rq()로 흐를 때 cgroup
 * 소속/지연/합병 정보를 참조합니다.
 */

struct blkcg_gq;        /* cgroup <-> request_queue 연결 노드 (NVMe namespace 단위) */
struct blkg_policy_data; /* blk-throttle/BFQ 등 policy별 private data 헤더 */


/* percpu_counter batch for blkg_[rw]stats, per-cpu drift doesn't matter */
/*
 * [한국어]
 * BLKG_STAT_CPU_BATCH - percpu_counter_add_batch() 에 넘기는 배치(batch) 임계값.
 * percpu_counter 는 CPU 로컬 delta 가 이 batch 값을 넘어야 비로소 전역(global)
 * count 에 반영(동기화)하는데, 여기서는 INT_MAX/2 라는 사실상 무한대에 가까운
 * 값을 주어 "거의 항상 percpu 로컬 값만 갱신하고 전역 동기화는 생략"하도록
 * 만든다. block/blk-cgroup-rwstat.h 의 blkg_rwstat_add()/block/bfq-cgroup.c 의
 * legacy(cgroup v1) 통계 카운터가 이 배치 값을 percpu_counter_add_batch() 의
 * batch 인자로 사용하며, 이런 통계는 다소 drift(오차)가 있어도 무방하기 때문에
 * 정확도보다 NVMe CQ 인터럽트/softirq 완료 경로에서의 cache-line 경합(false
 * sharing) 최소화를 우선한다는 설계 의도를 담고 있다.
 * 값 범위: INT_MAX/2 (컴파일 타임 상수).
 */
#define BLKG_STAT_CPU_BATCH	(INT_MAX / 2)

#ifdef CONFIG_BLK_CGROUP

/*
 * [한국어]
 * enum blkg_iostat_type - struct blkg_iostat 의 bytes[]/ios[] 배열 인덱스이자
 * IO 유형 분류자. blk_cgroup_bio_start()[block/blk-cgroup.c] 가 bio 하나가
 * 제출될 때 blk_cgroup_io_type(bio) 로 이 enum 값을 계산해 어느 슬롯을
 * 증가시킬지 정하며, cgroupfs 의 io.stat 파일이 이 세 값을 각각
 * "rbytes/wbytes/dbytes", "rios/wios/dios" 키로 노출한다.
 */
enum blkg_iostat_type {
	BLKG_IOSTAT_READ,
	/* [한국어] 읽기 IO. bio_op(bio) == REQ_OP_READ 인 bio 가 여기 집계된다.
	 * NVMe 관점: NVMe Read 커맨드(옵코드 0x02)로 발행되는 요청에 대응. */

	BLKG_IOSTAT_WRITE,
	/* [한국어] 쓰기 IO(REQ_OP_WRITE 및 flush/fua 가 실린 쓰기 포함)가 여기 집계된다.
	 * NVMe 관점: NVMe Write 커맨드(옵코드 0x01)로 발행되는 요청에 대응. */

	BLKG_IOSTAT_DISCARD,
	/* [한국어] 폐기/트림 IO(REQ_OP_DISCARD)가 여기 집계된다.
	 * NVMe 관점: Dataset Management(DSM, 옵코드 0x09)의 Deallocate 속성으로
	 * 매핑되어 SSD 컨트롤러에 해당 LBA 범위가 더 이상 유효하지 않음을 알린다. */

	BLKG_IOSTAT_NR,
	/* [한국어] 위 세 항목의 개수(=3)를 나타내는 관용적 "카운트" 멤버.
	 * 실제 통계 종류로 쓰이지 않고, bytes[BLKG_IOSTAT_NR]/ios[BLKG_IOSTAT_NR]
	 * 처럼 배열 크기를 컴파일 타임에 정의하는 용도로만 사용된다. */
};

/*
 * [한국어]
 * struct blkg_iostat - 한 시점의 read/write/discard 누적 통계 스냅샷.
 * blkg_iostat_set(아래) 안에 cur/last 두 벌로 내장되어 "현재 값"과 "마지막
 * flush 시점 값"을 구분하는 데 쓰이며, 두 스냅샷의 차이(delta)가 상위
 * (전역 blkg 또는 부모 blkg)로 전파된다. 실제 누적은 bio 가 완료되는 시점이
 * 아니라 blk_cgroup_bio_start()[block/blk-cgroup.c] 가 bio 제출을 시작하는
 * 시점에 이루어진다는 점에 유의한다(하드웨어 완료를 기다리지 않고 즉시 반영).
 */
struct blkg_iostat {
	u64				bytes[BLKG_IOSTAT_NR];
	/* [한국어] BLKG_IOSTAT_{READ,WRITE,DISCARD} 별 누적 전송 바이트 수.
	 * 설정자: blk_cgroup_bio_start() 가 bio->bi_iter.bi_size 를 더함(단, 원본
	 * bio 가 분할된 조각이면 BIO_CGROUP_ACCT 플래그로 중복 집계를 막는다).
	 * 읽는 자: __blkcg_rstat_flush()/blkcg_iostat_update()[block/blk-cgroup.c]
	 * 가 cur-last 델타를 계산할 때, blkcg_print_blkgs() 계열 helper 가
	 * cgroupfs io.stat 를 출력할 때.
	 * 값 범위: 0 이상, 프로세스/디스크 수명 동안 단조 증가(overflow 전까지 감소하지 않음).
	 * NVMe 관점: NVMe Read/Write 커맨드가 전송하는 데이터 길이(PRP/SGL 로
	 * 기술되는 바이트 수)가 여기 누적된다고 볼 수 있다.
	 * 동기화: 이 구조체 자신은 락이 없고, 이를 감싸는 blkg_iostat_set.sync
	 * (u64_stats_sync)를 통해 갱신/조회 시 시퀀스 카운터로 일관성을 보장한다. */

	u64				ios[BLKG_IOSTAT_NR];
	/* [한국어] BLKG_IOSTAT_{READ,WRITE,DISCARD} 별 누적 IO 제출 횟수.
	 * 설정자: blk_cgroup_bio_start() 가 매 bio 마다 해당 인덱스를 1 증가.
	 * 읽는 자: bytes[] 와 동일 - rstat flush 경로와 cgroupfs 출력 경로.
	 * 값 범위: 0 이상 단조 증가.
	 * NVMe 관점: 하나의 bio 는 보통 하나의 NVMe 커맨드(CID)로 발행되므로,
	 * 이 카운터는 개념적으로 SQ 에 올라간 커맨드 수에 대응한다(단, 완료가
	 * 아니라 제출 시점에 증가한다는 점은 bytes[] 와 동일하게 유의).
	 * 동기화: bytes[] 와 동일하게 u64_stats_sync 로 보호된다. */
};

/*
 * [한국어]
 * struct blkg_iostat_set - percpu(코어별) 로 유지되는 IO 통계 갱신 단위.
 * 이 구조체는 두 가지 방식으로 존재한다: (1) blkg->iostat_cpu 는 CPU 개수만큼
 * alloc_percpu() 로 할당되어 각 코어가 락 없이(lock-free) 갱신하는 인스턴스이고,
 * (2) blkg->iostat 는 blkg 하나당 단일 인스턴스로 존재하며 percpu 인스턴스들의
 * 값이 최종 합산되는 전역(global) 집계 지점이다. blk_cgroup_bio_start() 가
 * bio 제출 시점에 (1)을 갱신하고, __blkcg_rstat_flush()[block/blk-cgroup.c]
 * 가 cgroup rstat 프레임워크(kernel/cgroup/rstat.c 의 css_rstat_flush)에 의해
 * 주기적으로 호출되어 (1)의 값을 (2)로 flush 한다. 이 2단계 구조 덕분에 매
 * bio 마다 전역 락을 잡을 필요가 없어, NVMe 멀티큐(multi-queue) 환경에서
 * 여러 코어가 동시에 IO 를 제출/완료하더라도 cache-line 경합이 생기지 않는다.
 */
struct blkg_iostat_set {
	struct u64_stats_sync		sync;
	/* [한국어] cur 필드(그리고 percpu 인스턴스의 경우 lqueued 갱신)를 보호하는
	 * 32/64bit 겸용 시퀀스 카운터 기반 동기화 프리미티브(u64_stats_sync).
	 * 설정자: blkg_alloc() 이 u64_stats_init() 으로 초기화.
	 * 갱신자(writer): 자신이 속한 CPU 에서만 blk_cgroup_bio_start() 가
	 * u64_stats_update_begin_irqsave()/end_irqrestore() 로 감싸 갱신(percpu
	 * 인스턴스는 사실상 CPU-local 단일 writer 라 cross-CPU 갱신이 없다).
	 * 읽는 자(reader): __blkcg_rstat_flush() 가 u64_stats_fetch_begin()/
	 * fetch_retry() 루프로 다른 CPU 의 percpu 인스턴스를 안전하게 스냅샷.
	 * 값 범위: 커널 내부 시퀀스 카운터(직접 해석 불필요).
	 * 동기화: irqsave 변형을 쓰는 이유는 blk_cgroup_bio_start() 가 인터럽트
	 * 컨텍스트에서도 호출될 수 있어(NVMe 완료 인터럽트 등) 재진입 갱신을 막기 위함. */

	struct blkcg_gq		       *blkg;
	/* [한국어] 이 iostat_set 이 소속된 blkcg_gq 로의 역참조 포인터.
	 * 설정자: blkg_alloc() 이 percpu 인스턴스마다, 그리고 blkg->iostat.blkg
	 * 자기 자신에 대해서도 blkg 포인터를 채운다.
	 * 읽는 자: __blkcg_rstat_flush() 가 lockless list 를 순회하며 각 노드가
	 * 어느 blkg 소속인지, 나아가 blkg->parent 를 통해 계층 전파 대상을 찾을 때.
	 * 값 범위: 유효한 blkcg_gq 포인터(NULL 불가, blkg 와 생명주기 동일).
	 * 동기화: blkg 자체가 RCU/percpu_ref 로 보호되므로 이 포인터는 immutable
	 * 하게 취급되고 별도 락이 필요 없다. */

	struct llist_node		lnode;
	/* [한국어] blkcg->lhead(percpu llist_head)에 이 노드를 매다는 lock-free
	 * 단일 연결 리스트 노드.
	 * 추가자: blk_cgroup_bio_start() 가 llist_add(&bis->lnode, lhead) 로
	 * "아직 flush 되지 않은 percpu 통계" 목록에 등록.
	 * 제거자: __blkcg_rstat_flush() 가 llist_del_all() 로 CPU 별 리스트
	 * 전체를 한 번에 떼어내 순회(llist_for_each_entry_safe) 후 소비.
	 * 값 범위: 리스트에 있을 때는 next 포인터가 유효, 없을 때는 미사용.
	 * 동기화: llist_* 계열 원자적 연산으로 보호되어 별도 스핀락 없이
	 * 여러 CPU/인터럽트 컨텍스트에서 안전하게 push 가능(단일 소비자 가정 하 pop). */

	int				lqueued;
	/* [한국어] 이 percpu 통계가 현재 lhead 리스트에 큐잉되어 flush 를
	 * 기다리는 중인지 나타내는 불리언 성격의 플래그(0/1).
	 * 설정자: blk_cgroup_bio_start() 가 아직 큐잉되지 않았을 때만
	 * WRITE_ONCE(bis->lqueued, true) 로 설정한 뒤 llist_add() 수행 - 이미
	 * 큐잉된 상태면 중복으로 리스트에 넣지 않는다(리스트 중복 삽입 방지 가드).
	 * 읽는 자: blk_cgroup_bio_start() 가 READ_ONCE(bis->lqueued) 로 확인;
	 * __blkcg_rstat_flush() 가 flush 직후 WRITE_ONCE(false) 로 리셋.
	 * 값 범위: 0(큐잉 안 됨) 또는 true/1(큐잉됨).
	 * 동기화: READ_ONCE/WRITE_ONCE 와 smp_mb() 조합으로 llist 의 next 포인터
	 * 로드와의 순서를 보장한다(배리어가 없으면 flush 도중 추가된 노드를
	 * 잘못 skip 하거나 이중 처리할 위험이 있음). */

	struct blkg_iostat		cur;
	/* [한국어] 현재까지 누적된 read/write/discard bytes/ios (최신 스냅샷).
	 * 설정자: blk_cgroup_bio_start() 가 매 bio 제출 시 bytes[]/ios[] 증가.
	 * 읽는 자: __blkcg_rstat_flush() 가 last 와의 차이(delta)를 계산해
	 * 전역 blkg->iostat.cur 에 반영.
	 * 값 범위: struct blkg_iostat 필드 설명과 동일(0 이상 단조 증가).
	 * 동기화: 이 필드를 감싸는 sync(u64_stats_sync)로 보호. */

	struct blkg_iostat		last;
	/* [한국어] 마지막으로 flush 된 시점의 cur 스냅샷 - "다음 flush 때 얼마나
	 * 늘었는지"를 계산하기 위한 기준점(baseline)이다.
	 * 설정자: __blkcg_rstat_flush()/blkcg_iostat_update() 가 flush 직후
	 * cur 값으로 last 를 갱신.
	 * 읽는 자: 다음 flush 시점의 __blkcg_rstat_flush() 자기 자신.
	 * 값 범위: cur 과 동일한 범위, 항상 cur 이하이거나 같음(단조 증가 누적이므로).
	 * 동기화: flush 는 raw spinlock(blkg_stat_lock)으로 직렬화되어 동시에
	 * 하나의 CPU 만 이 필드를 갱신한다. */
};

/* association between a blk cgroup and a request queue */
/*
 * [한국어]
 * struct blkcg_gq(blkg) - (blkcg, request_queue) 쌍의 연결점이자 cgroup별
 * IO 제어의 실제 단위. 하나의 cgroup 이 여러 블록 장치(request_queue)에 IO 를
 * 낼 수 있으므로 blkcg 하나당 여러 개의 blkg 가 존재할 수 있고, 반대로 하나의
 * request_queue 에도 그 위를 지나간 cgroup 개수만큼 blkg 가 생긴다. NVMe SSD
 * 관점에서는 request_queue(q) 가 보통 하나의 namespace 를 나타내므로, blkg 는
 * "이 cgroup 이 이 namespace 로 낸 IO 전체"를 추적하는 단위로 이해할 수 있다.
 * blkg 는 cgroup 계층을 따라 parent 로 연결되어(root_blkg 까지) throttle 한도
 * 상속이나 통계의 상향 전파를 가능하게 하고, pd[] 배열을 통해 blk-throttle/
 * BFQ/blk-ioprio 같은 실제 정책의 사설 상태를 담아 나른다.
 */
struct blkcg_gq {
	/* Pointer to the associated request_queue */
	struct request_queue		*q;
	/* [한국어] 이 blkg 가 연결된 request_queue(디스크 큐).
	 * 설정자: blkg_alloc() 이 disk->queue 값을 대입, 이후 절대 바뀌지 않음(immutable).
	 * 읽는 자: blkg_lookup(), blkcg_policy_enabled(), blk_cgroup_mergeable() 등
	 * IO 경로 전반이 이 포인터를 통해 큐 상태(q->queue_lock, q->blkcg_pols)를 참조.
	 * 값 범위: 유효한 request_queue 포인터(NULL 불가). blk_get_queue() 로 참조를
	 * 잡아두므로 blkg 가 살아있는 동안 q 자체는 free 되지 않는다.
	 * 동기화: 값 자체는 불변이라 락 불필요; q 의 내부 상태(freeze, dying)는
	 * q->queue_lock/q_usage_counter 로 별도 보호.
	 * NVMe 관점: NVMe namespace 의 mq submit entry point(request_queue)를 가리킨다. */

	struct list_head		q_node;
	/* [한국어] q->blkg_list 에 이 blkg 를 매다는 리스트 노드(일반 list_head, RCU 아님).
	 * 설정자: blkg_create() 가 list_add() 로 등록.
	 * 제거자: blkg_free_workfn() 에서 list_del_init() - blkg_destroy() 시점이
	 * 아니라 정책 pd_free_fn() 이 모두 끝난 뒤(work 컨텍스트)에 지연 제거되는 점에 유의.
	 * 읽는 자: blkcg_deactivate_policy(), 디스크 제거 시 q 에 매달린 모든 blkg 순회.
	 * 값 범위: 등록 상태에서는 유효한 리스트 노드, 미등록 시 list_empty() 로 판별.
	 * 동기화: q->queue_lock 으로 보호. */

	struct hlist_node		blkcg_node;
	/* [한국어] blkcg->blkg_list(hlist) 에 이 blkg 를 매다는 RCU 겸용 노드.
	 * 설정자: blkg_create() 가 hlist_add_head_rcu() 로 등록 - RCU reader(예:
	 * blkg_lookup() 의 폴백 경로)가 즉시 볼 수 있게 배리어와 함께 삽입.
	 * 제거자: blkg_destroy() 가 hlist_del_init_rcu() 로 제거하되, 실제 메모리는
	 * RCU grace period 가 지날 때까지 유지된다(콜백 __blkg_release 까지).
	 * 읽는 자: blkcg_print_blkgs() 등이 hlist_for_each_entry_rcu() 로 순회.
	 * 값 범위: hlist_unhashed() 로 "이미 destroy 되었는지" 판별 가능(blkg_destroy 의
	 * 중복 호출 방지 가드로 사용).
	 * 동기화: 삽입/삭제는 blkcg->lock, 순회(읽기)는 RCU read-side critical section. */

	struct blkcg			*blkcg;
	/* [한국어] 이 blkg 가 대표하는 상위 cgroup(정책 무관 공통 상태).
	 * 설정자: blkg_alloc() 에서 대입, 이후 불변.
	 * 읽는 자: blkg_destroy(), blkg_release() 등이 blkcg->lock/blkg_tree 조작 시.
	 * 값 범위: 유효한 blkcg 포인터(NULL 불가); blkg_create() 의 css_tryget_online()
	 * 이 이 cgroup 의 css 참조를 붙잡고 있으므로 blkg 생존 중에는 blkcg 도
	 * 최소한 offline 상태로나마 존재를 유지한다.
	 * 동기화: 불변 포인터, 별도 락 불필요. */

	/* all non-root blkcg_gq's are guaranteed to have access to parent */
	struct blkcg_gq			*parent;
	/* [한국어] cgroup 계층에서 바로 위 조상(cgroup parent)에 대응하는 blkg.
	 * root cgroup 의 blkg(q->root_blkg)는 parent 가 NULL, 그 외 non-root blkg
	 * 는 반드시 유효한 parent 를 갖는다(원문 주석 "all non-root blkcg_gq's are
	 * guaranteed to have access to parent" - blkg_create() 가 이를 보장).
	 * 설정자: blkg_create() 가 blkg_lookup(부모 blkcg, 같은 q) 결과를 대입하고
	 * blkg_get() 으로 참조 카운트를 증가시킨다.
	 * 읽는 자: __blkcg_rstat_flush() 가 통계를 상위로 전파할 때, blk-throttle
	 * 이 계층적 한도(부모의 남은 대역폭)를 확인할 때.
	 * 값 범위: NULL(root) 또는 유효한 blkg 포인터.
	 * 동기화: blkg_get() 으로 참조를 쥐고 있어 자식이 살아있는 한 부모도
	 * 함부로 해제되지 않는다(blkg_free_workfn 에서 blkg_put(parent) 로 반납). */

	/* reference count */
	struct percpu_ref		refcnt;
	/* [한국어] blkg 의 생명주기를 관리하는 percpu 참조 카운터.
	 * 설정자/초기화: blkg_alloc() 이 percpu_ref_init(..., blkg_release, 0, ...) 로 초기화.
	 * 증가자: blkg_get()/blkg_tryget() - bio_associate_blkg() 등이 bio 가
	 * 이 blkg 를 참조하는 동안 호출.
	 * 감소자: blkg_put() - bio 완료/해제 시. 생성 시 잡은 초기 참조는
	 * blkg_destroy() 의 percpu_ref_kill() 이 반납.
	 * 0 도달 시: blkg_release() 콜백이 call_rcu(&blkg->rcu_head, __blkg_release)
	 * 로 RCU 유예 후 blkg_free() -> schedule_work(&blkg->free_work) 로 최종 kfree.
	 * 값 범위: percpu_ref 내부 표현(직접 정수로 해석하지 않고 percpu_ref_tryget()/
	 * percpu_ref_is_zero() 류 API 로만 다룸).
	 * 동기화: percpu_ref 자체가 lock-free 알고리즘(RCU + percpu 카운터)이라
	 * 별도 스핀락 없이 인터럽트 컨텍스트에서도 get/put 가능. */

	/* is this blkg online? protected by both blkcg and q locks */
	bool				online;
	/* [한국어] 이 blkg 가 현재 IO 분류/정책 적용 대상으로 "살아있는지" 여부.
	 * 설정자: blkg_create() 마지막 단계에서 true, blkg_destroy() 에서 false.
	 * 읽는 자: bio_associate_blkg() 계열이 lookup 결과가 online 인지 확인;
	 * false 인 blkg 는 제거 절차 중이므로 상위(root) blkg 로 spill.
	 * 값 범위: true(사용 가능) / false(제거 중이거나 아직 초기화 전).
	 * 동기화: 원문 주석대로 blkcg->lock 과 q->queue_lock 양쪽 모두를 잡은
	 * 상태에서만 값이 바뀌므로, 둘 중 하나만 들고 있어도 읽기에는 안전하다. */

	struct blkg_iostat_set __percpu	*iostat_cpu;
	/* [한국어] CPU 개수만큼 존재하는 percpu IO 통계 인스턴스 배열(포인터).
	 * 설정자: blkg_alloc() 이 alloc_percpu_gfp() 로 할당, per_cpu_ptr() 로 각
	 * 인스턴스를 순회하며 u64_stats_init()/blkg 역참조를 세팅.
	 * 갱신자: blk_cgroup_bio_start() 가 현재 CPU 의 인스턴스만 락 없이 갱신 -
	 * 다른 CPU 의 인스턴스는 절대 건드리지 않는다.
	 * 읽는 자: __blkcg_rstat_flush() 가 모든 CPU 인스턴스를 순회하며 blkg->iostat 로 flush.
	 * 해제자: blkg_free_workfn() 이 free_percpu() 로 반납.
	 * 값 범위: 유효한 percpu 포인터(할당 실패 시 blkg_alloc() 자체가 실패 처리).
	 * 동기화: CPU 간에는 서로 다른 메모리 영역이라 락이 필요 없고, 같은 CPU
	 * 내에서는 인스턴스의 u64_stats_sync 로 irqsave 하게 보호. */

	struct blkg_iostat_set		iostat;
	/* [한국어] percpu 인스턴스들의 값이 최종 합산되는 전역(global) IO 통계.
	 * 설정자: blkg_alloc() 이 iostat.blkg = blkg(자기 자신)로 역참조 세팅.
	 * 갱신자: __blkcg_rstat_flush() 가 각 percpu 인스턴스의 delta 를 더하고
	 * (blkg_iostat_add), 이 필드 자신도 lhead 에 등록되어 부모 blkg 의
	 * iostat.cur 로 다시 delta 를 전파(계층 합산)한다.
	 * 읽는 자: cgroupfs io.stat 출력 함수들(blkcg_print_blkgs 계열).
	 * 값 범위: percpu 필드들과 동일 범위. blkg 하나당 유일 인스턴스.
	 * 동기화: 전역 인스턴스라 여러 CPU 의 flush 가 경쟁할 수 있어 raw
	 * spinlock(blkg_stat_lock)+u64_stats_sync 조합으로 보호. */

	struct blkg_policy_data		*pd[BLKCG_MAX_POLS];
	/* [한국어] blk-throttle/BFQ/blk-ioprio 등 등록된 각 정책이 이 blkg 에 대해
	 * 소유하는 사설(private) 데이터 포인터 배열. 인덱스는 정책의 plid.
	 * 설정자: blkg_alloc()/blkcg_activate_policy() 가 pol->pd_alloc_fn() 호출 결과를 대입.
	 * 초기화/온라인: blkg_create() 가 pd_init_fn()/pd_online_fn() 을 호출해 pd[i]->online = true.
	 * 읽는 자: blkg_to_pd(), 각 정책의 IO 판단 로직(throtl_charge_bio, bfq
	 * 스케줄러 등)이 blkcg_policy_enabled() 로 활성화 여부를 먼저 확인한 뒤 참조.
	 * 해제자: blkg_destroy() 가 pd_offline_fn(), blkg_free_workfn() 이 pd_free_fn() 호출.
	 * 값 범위: 해당 정책이 이 큐에서 비활성화되어 있으면 NULL, 활성화되어
	 * 있으면 pd_alloc_fn() 이 반환한(embed 된) 구조체의 시작 주소.
	 * 동기화: q->queue_lock 또는 blkcg->lock 을 든 상태에서 blkcg_policy_enabled()
	 * 확인 후 참조하면 안전하다(blkcg_activate_policy() kernel-doc 참고). */
#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	spinlock_t			async_bio_lock;
	/* [한국어] 아래 async_bios 리스트를 보호하는 스핀락.
	 * CONFIG_BLK_CGROUP_PUNT_BIO=y 일 때만 존재 - 공유 kthread 가 우선순위
	 * 역전을 피하려고 bio 제출을 workqueue 로 미룰 때(blkcg_punt_bio_submit)만 쓰인다.
	 * 설정자: blkg_alloc() 이 spin_lock_init().
	 * 잠그는 자: blkcg_punt_bio_submit()(추가), blkg_async_bio_workfn()(소비/이동).
	 * 동기화: bio 추가는 임의 컨텍스트(kthread)에서, 소비는 workqueue 컨텍스트에서
	 * 일어나므로 둘 사이의 상호 배제가 필요해 이 락이 존재한다. */

	struct bio_list			async_bios;
	/* [한국어] priority inversion 회피를 위해 즉시 submit_bio 하지 않고
	 * workqueue 로 미뤄둔 bio 들의 대기열.
	 * 추가자: blkcg_punt_bio_submit() 이 bio_list_add() - root 가 아닌 cgroup 의
	 * bio 에 대해서만 사용(root 는 즉시 submit_bio).
	 * 소비자: blkg_async_bio_workfn() 이 bio_list_merge_init() 으로 통째로
	 * 가져가 순서대로 submit_bio() 재호출.
	 * 값 범위: 비어있음(BIO_EMPTY_LIST) ~ 임의 개수의 대기 bio.
	 * 동기화: async_bio_lock 으로 보호. */
#endif
	union {
		struct work_struct	async_bio_work;
		/* [한국어] async_bios 에 쌓인 bio 를 실제로 submit_bio() 하는
		 * blkg_async_bio_workfn() 을 실행시키는 workqueue 아이템.
		 * blkg 가 살아있는 동안(online) 여러 번 반복 사용될 수 있다.
		 * 설정자: blkg_alloc() 이 INIT_WORK(). 큐잉: blkcg_punt_bio_submit()
		 * 이 queue_work(blkcg_punt_bio_wq, ...). */

		struct work_struct	free_work;
		/* [한국어] refcnt 가 0이 되어 RCU 유예(call_rcu)까지 지난 뒤, 실제
		 * 메모리 회수(blkg_free_workfn, 정책 pd_free_fn 포함)를 프로세스
		 * 컨텍스트에서 수행하기 위한 workqueue 아이템. RCU 콜백은 보통
		 * 인터럽트/소프트IRQ 컨텍스트라 mutex 를 못 잡으므로 별도 work 로 위임.
		 * 설정자/큐잉: blkg_free() 가 INIT_WORK() 후 schedule_work().
		 * async_bio_work 와 union 인 이유: 이 blkg 가 파괴 절차에 들어가는
		 * 시점(free_work 사용)에는 더 이상 async_bios 를 받지 않으므로
		 * (offline 이후) 같은 메모리를 재사용해도 안전하기 때문 - 구조체 크기 절약. */
	};

	atomic_t			use_delay;
	/* [한국어] 이 blkg 에 대해 현재 지연(스로틀)을 요청 중인 "사용자" 수를 세는
	 * 참조 카운터 겸 모드 플래그.
	 * 증가자: blkcg_use_delay() - 첫 증가(0->1) 시 blkcg->congestion_count 도 +1.
	 * 감소자: blkcg_unuse_delay() - CAS 루프로 1 감소, 마지막 감소(1->0) 시
	 * congestion_count -1.
	 * 특수 값: blkcg_set_delay() 가 쓰는 "고정 지연 모드"에서는 이 값이 -1
	 * 로 설정되어 decay(blkcg_scale_delay)를 하지 않는다는 신호로 쓰인다
	 * (blkcg_clear_delay() 로만 해제 가능, [un]use_delay 와 혼용 금지).
	 * 값 범위: -1(고정 지연 모드) 또는 0 이상(사용 중인 지연자 수).
	 * 동기화: atomic_t 연산(atomic_add_return/atomic_try_cmpxchg)만으로 락 없이 관리. */

	atomic64_t			delay_nsec;
	/* [한국어] 이 blkg 소속 태스크가 다음 유저스페이스 복귀 시 sleep 해야 할
	 * 누적 지연 시간(나노초).
	 * 증가자: blkcg_add_delay() 가 blk-throttle/BFQ 등에서 목표 대역폭/IOPS
	 * 초과분(delta)을 더함. blkcg_set_delay() 는 고정값으로 직접 설정.
	 * 감소자: blkcg_scale_delay() 가 1초 지난 예산을 decay(차감), 실제 소비는
	 * blkcg_maybe_throttle_blkg()[block/blk-cgroup.c] 가 이 값을 읽어 usleep 류로 소진.
	 * 값 범위: 0 이상(nsec 단위). 실제 sleep 은 min(값, 1초)로 캡핑됨.
	 * 동기화: atomic64_t 연산으로 락 없이 관리; 여러 코어가 동시에 delta 를
	 * 더해도 안전. */

	atomic64_t			delay_start;
	/* [한국어] 마지막으로 delay_nsec/last_delay 를 decay(스케일링)한 시각(ktime, ns).
	 * 설정자/갱신자: blkcg_scale_delay() 가 atomic64_try_cmpxchg() 로 "1초가
	 * 지났으면 지금 시각으로" 갱신 - CAS 에 성공한 단 하나의 CPU만 decay 로직을 수행.
	 * 읽는 자: blkcg_scale_delay() 자신(다음 호출 때 경과 시간 계산).
	 * 값 범위: 단조 시계(ktime) 값.
	 * 동기화: atomic64_try_cmpxchg 자체가 "동시에 여러 CPU가 add_delay 를
	 * 불러도 decay 계산은 한 CPU만 수행"하도록 만드는 게이트 역할을 한다. */

	u64				last_delay;
	/* [한국어] 마지막 decay 시점에 "아직 차감되지 않고 남아있던" 지연 예산.
	 * 다음 decay 계산 시 "이번에 얼마나 더 깎을지"의 기준(baseline)이 된다.
	 * 설정자: blkcg_scale_delay() 가 delay_start CAS 에 성공했을 때만 갱신
	 * (cur - sub 또는 0). 이 필드는 delay_start CAS 라는 게이트로 보호되므로
	 * 별도 원자적 타입일 필요가 없다(항상 단일 CPU만 접근).
	 * 값 범위: 0 이상, delay_nsec 이하.
	 * 동기화: atomic 타입이 아니지만 delay_start CAS 성공자만 접근하므로 안전. */

	int				last_use;
	/* [한국어] 마지막 decay 시점의 use_delay 스냅샷 - "그 사이 throttle 이
	 * 해제(unthrottle)되었는지"를 판단해 지연 예산을 더 크게 깎을지 결정하는 데 쓰인다.
	 * 설정자: blkcg_scale_delay() 가 delay_start CAS 성공 시 cur_use 로 갱신.
	 * 읽는 자: blkcg_scale_delay() 자기 자신(다음 decay 때 cur_use 와 비교).
	 * 값 범위: use_delay 와 동일한 범위(-1 또는 0 이상).
	 * 동기화: last_delay 와 마찬가지로 delay_start CAS 게이트로 단일 접근 보장. */

	struct rcu_head			rcu_head;
	/* [한국어] blkg 의 RCU 기반 지연 해제를 위한 콜백 헤더.
	 * 사용자: blkg_release()(percpu_ref 의 release 콜백)가
	 * call_rcu(&blkg->rcu_head, __blkg_release) 로 등록 - RCU read-side
	 * critical section(blkg_lookup 등)이 모두 끝난 뒤에야 __blkg_release() 가
	 * 실행되어 blkg_free() -> free_work 스케줄링으로 이어짐을 보장한다.
	 * 값 범위: 커널 RCU 서브시스템 내부 링크 포인터(직접 해석 불필요).
	 * 동기화: RCU 자체 메커니즘(grace period)으로 보호, 별도 락 불필요. */
};

/*
 * [한국어]
 * struct blkcg - 하나의 block cgroup(디렉터리)이 갖는 IO 서브시스템 상태.
 * cgroup 코어가 관리하는 cgroup_subsys_state(css)를 embed 해 cgroup 트리의
 * 노드 역할도 겸하며, 이 cgroup 이 실제로 IO 를 낸 각 request_queue 마다
 * 하나씩 생기는 blkg 들을 blkg_tree(빠른 조회용 radix tree)와 blkg_list(순회용
 * hlist) 두 자료구조로 함께 관리한다. cpd[] 는 특정 request_queue 와 무관하게
 * cgroup 전체에 걸쳐 유지되는 정책 데이터(예: 정책이 지원하면 cgroup 전체
 * 기본 weight/한도)를 담는다. congestion_count/lhead 는 각각 "이 cgroup
 * 예하 어딘가에 IO 지연이 걸려 있는가", "percpu 통계 중 아직 flush 되지 않은
 * 것이 있는가"를 나타내는 집계 상태다.
 */
struct blkcg {
	struct cgroup_subsys_state	css;
	/* [한국어] cgroup 코어가 부여하는 서브시스템 상태 - 이 필드를 통해 blkcg 는
	 * cgroup_subsys_state 로도, container_of(css_to_blkcg)로 다시 blkcg 로도
	 * 취급될 수 있는 관계를 갖는다(첫 번째 필드로 embed 하는 커널 관용구).
	 * 설정자: blkcg_css_alloc()[block/blk-cgroup.c] 이 cgroup 코어 API 로 초기화.
	 * 읽는 자: css_to_blkcg(), bio_blkcg_css(), css_for_each_descendant_* 등
	 * cgroup 계층 순회 코드 전반.
	 * 값 범위: cgroup 서브시스템(io_cgrp_subsys)에 등록된 유효한 css.
	 * 동기화: css 자체 생명주기는 cgroup 코어의 RCU/refcount 규약을 따른다. */

	spinlock_t			lock;
	/* [한국어] 이 blkcg 의 blkg_tree/blkg_list/blkg_hint 갱신을 보호하는 스핀락.
	 * (참고: blkg_lookup() 자체는 이 락 없이 RCU 만으로 동작하는 핫패스이고,
	 * 이 lock 은 blkg 생성/삭제 같은 구조 변경 시에만 관여한다.)
	 * 잠그는 자: blkg_create(), blkg_destroy(), blkcg_destroy_blkgs() 등
	 * blkg 를 신설/제거하는 경로.
	 * 값 범위: 표준 spinlock_t 상태.
	 * 동기화: q->queue_lock 과 함께 잡히는 경우 lockdep 상에서 중첩 순서가
	 * 고정되어 있다(blkg_destroy()의 lockdep_assert_held() 두 개 참고). */

	refcount_t			online_pin;
	/* [한국어] 이 cgroup 이 "online 상태를 계속 유지해야 하는" 이유의 개수를
	 * 세는 참조 카운터 - 0 이 되기 전까지는 cgroup 오프라인 처리(및 그에 따른
	 * blkg 제거)가 시작되지 않는다.
	 * 초기화: blkcg_css_alloc() 이 refcount_set(1).
	 * 증가자: blkcg_css_online() 이 자신의 부모에 대해 +1(자식이 살아있는
	 * 한 부모도 강제로 online 유지).
	 * 감소자: blkcg_css_offline()/blkcg_unpin_online() - 0 이 되는 순간 실제
	 * blkg 제거(destroy) 절차가 시작된다.
	 * 값 범위: 1 이상(생존 중) ~ 0(제거 트리거).
	 * 동기화: refcount_t 원자 연산으로 락 없이 관리, 0 도달은 정확히 한 번만
	 * 감지되도록 refcount_dec_and_test() 류 API 사용. */

	/* If there is block congestion on this cgroup. */
	atomic_t			congestion_count;
	/* [한국어] 이 cgroup 소속 blkg 들 중 최소 하나 이상이 use_delay/set_delay
	 * 로 지연(스로틀) 중임을 나타내는 카운터.
	 * 증가자: blkcg_use_delay()/blkcg_set_delay() 가 "첫 지연자"일 때 +1.
	 * 감소자: blkcg_unuse_delay()/blkcg_clear_delay() 가 "마지막 지연자"일 때 -1.
	 * 읽는 자: blk_cgroup_congested() - writeback 등이 이 값이 0 보다 크면
	 * "이 cgroup 계층에 IO 혼잡이 있다"고 판단해 추가 제출을 자제하는 데 활용.
	 * 값 범위: 0(혼잡 없음) 이상 - 이 cgroup 에 매달린 blkg 개수만큼 이론상 증가 가능.
	 * 동기화: atomic_t 연산만으로 락 없이 관리. */

	struct radix_tree_root		blkg_tree;
	/* [한국어] request_queue->id 를 키로 하는 blkg 색인 - blkg_lookup() 의
	 * "hint 캐시 실패 시" 폴백 경로가 조회하는 자료구조.
	 * 설정자: blkcg_css_alloc() 이 INIT_RADIX_TREE(..., GFP_NOWAIT).
	 * 삽입자: blkg_create() 가 radix_tree_insert(tree, q->id, blkg).
	 * 삭제자: blkg_destroy() 가 radix_tree_delete(tree, q->id).
	 * 읽는 자: blkg_lookup() 이 radix_tree_lookup(tree, q->id).
	 * 값 범위: 키=각 디스크 request_queue 의 id, 값=해당 blkg 포인터.
	 * 동기화: 삽입/삭제는 blkcg->lock 하에서, 조회(radix_tree_lookup)는 RCU
	 * read-side 에서 이루어지는 lock-free 조회 패턴(RCU-safe radix tree). */

	struct blkcg_gq	__rcu		*blkg_hint;
	/* [한국어] 가장 최근에 성공적으로 조회된 blkg 를 캐싱해 반복적인 radix
	 * tree 조회를 건너뛰기 위한 1-entry 캐시(hot path 최적화).
	 * 설정자: blkg_lookup_create()[block/blk-cgroup.c] 성공 경로가
	 * rcu_assign_pointer() 로 갱신.
	 * 클리어: blkg_destroy() 가 제거 대상이 hint 와 같으면 NULL 로 클리어.
	 * 읽는 자: blkg_lookup() 이 rcu_dereference_check() 로 먼저 확인 -
	 * blkg->q 가 원하는 q 와 일치하면 radix tree 조회 없이 바로 반환.
	 * 값 범위: NULL(캐시 없음) 또는 유효한 blkg 포인터(단, q 불일치 가능성이
	 * 있어 사용 전 blkg->q == q 재검증 필수).
	 * 동기화: __rcu 애노테이션대로 RCU 로 보호 - 읽기는 rcu_dereference*,
	 * 쓰기는 rcu_assign_pointer(). */

	struct hlist_head		blkg_list;
	/* [한국어] 이 cgroup 에 속한 모든 blkg 를 매다는 순회용 연결 리스트
	 * (radix tree 가 "특정 q 에 대한 조회"라면, 이쪽은 "이 cgroup 의 모든
	 * blkg 를 훑어야 하는" 작업을 위한 자료구조).
	 * 삽입자: blkg_create() 가 hlist_add_head_rcu().
	 * 삭제자: blkg_destroy() 가 hlist_del_init_rcu().
	 * 읽는 자: blkcg_print_blkgs()(cgroupfs 통계 출력), blkcg_destroy_blkgs()
	 * (cgroup 제거 시 전체 blkg 정리) 등 "cgroup 하나 기준" 순회에 쓰인다.
	 * 값 범위: 비어있음 ~ 이 cgroup 이 IO 를 낸 request_queue 개수만큼의 노드.
	 * 동기화: 변경은 blkcg->lock, 순회는 hlist_for_each_entry_rcu() 로 RCU read-side. */

	struct blkcg_policy_data	*cpd[BLKCG_MAX_POLS];
	/* [한국어] 등록된 각 정책이 "특정 request_queue 와 무관하게" cgroup
	 * 전체에 대해 유지하는 사설 데이터 배열(인덱스는 정책 plid).
	 * 설정자: blkcg_policy_register() 가 새 정책 등록 시 all_blkcgs 의 모든
	 * blkcg 에 대해 pol->cpd_alloc_fn() 호출 결과를 대입; blkcg_css_alloc() 은
	 * 새 cgroup 생성 시 이미 등록된 모든 정책에 대해 마찬가지로 할당.
	 * 읽는 자: blkcg_to_cpd(), 정책이 device 독립적인 기본값(예: cgroup 전체
	 * IO weight 기본치)을 참조할 때.
	 * 해제자: blkcg_policy_unregister()/blkcg_css_free() 가 cpd_free_fn() 호출.
	 * 값 범위: 정책이 cpd_alloc_fn 을 제공하지 않으면 NULL(해당 정책은 cgroup
	 * 레벨 데이터가 없다는 뜻 - blk-ioprio 처럼 device 레벨 pd 없이 cpd 만
	 * 쓰는 정책도 있고, 반대로 cpd 없이 pd 만 쓰는 정책도 있다).
	 * 동기화: blkcg_pol_mutex 로 배열 변경(등록/해제)을 보호. */

	struct list_head		all_blkcgs_node;
	/* [한국어] 시스템에 존재하는 모든 blkcg 를 매다는 전역 리스트(all_blkcgs)의 노드.
	 * 설정자: blkcg_css_alloc() 이 list_add_tail(&all_blkcgs).
	 * 제거자: blkcg_css_free() 가 list_del().
	 * 읽는 자: blkcg_policy_register()/unregister() 가 "이미 존재하는 모든
	 * cgroup" 에 대해 새 정책의 cpd 를 일괄 할당/해제할 때 순회.
	 * 값 범위: 정상 리스트 노드.
	 * 동기화: blkcg_pol_mutex 로 보호(원문 주석 "protected by blkcg_pol_mutex" 참고). */

	/*
	 * List of updated percpu blkg_iostat_set's since the last flush.
	 */
	struct llist_head __percpu	*lhead;
	/* [한국어] CPU 개수만큼 존재하는 percpu lock-free 리스트 헤드 배열 -
	 * "최근 blk_cgroup_bio_start() 로 갱신되었지만 아직 blkg->iostat 전역
	 * 값으로 flush 되지 않은" blkg_iostat_set 들을 CPU 별로 추적한다.
	 * cgroup rstat 프레임워크(kernel/cgroup/rstat.c)는 "어느 CPU 가 갱신
	 * 되었는지"는 알아도 "그 CPU 의 어느 blkg 가 갱신되었는지"는 모르기
	 * 때문에, 이 리스트가 없으면 flush 때마다 이 cgroup 의 모든 blkg 를
	 * 전수 조사해야 해서 request_queue(디스크)가 많을수록 비용이 커진다.
	 * 설정자: blkcg_css_alloc() 이 alloc_percpu(struct llist_head).
	 * 추가자: blk_cgroup_bio_start() 가 llist_add(&bis->lnode, this_cpu_ptr(lhead)).
	 * 소비자: __blkcg_rstat_flush() 가 llist_del_all() 로 CPU 별 전체를 가져와 순회.
	 * 값 범위: 비어있음 ~ 그 CPU 에서 최근 갱신된 blkg 개수만큼의 노드.
	 * 동기화: llist_* 원자적 연산(lock-free), CPU 마다 독립된 헤드라 CPU 간
	 * 경합 자체가 없다. */

#ifdef CONFIG_BLK_CGROUP_FC_APPID
	char                            fc_app_id[FC_APPID_LEN];
	/* [한국어] 이 cgroup(주로 VM/컨테이너 하나에 대응)에 결부된 응용 프로그램
	 * 식별자 문자열. FC-NVMe/NVMe-oF 타겟이 QoS·감사(audit) 목적으로 상위
	 * 애플리케이션을 식별할 수 있도록 전달된다(block/blk-cgroup-fc-appid.c 참고).
	 * 설정자: blkcg_set_fc_appid()[block/blk-cgroup-fc-appid.c, cgroup 파일
	 * write 핸들러 경로]가 strscpy() 로 기록.
	 * 읽는 자: blkcg_get_fc_appid() 가 bio->bi_blkg->blkcg->fc_app_id 순으로
	 * 접근해 FC-NVMe 드라이버에 전달할 문자열을 얻는다.
	 * 값 범위: 최대 FC_APPID_LEN-1 자 + NUL, 비어 있으면(첫 바이트 '\0')
	 * "설정되지 않음"으로 취급.
	 * 동기화: 별도 락 없이 strscpy() 로 기록(경쟁 시에도 문자열 자체는
	 * 원자적으로 교체되지 않지만, 설정 빈도가 낮은 제어 경로라 best-effort). */
#endif
#ifdef CONFIG_CGROUP_WRITEBACK
	struct list_head		cgwb_list;
	/* [한국어] 이 cgroup 에 연결된 cgroup writeback(bdi_writeback) 객체들의 리스트.
	 * 버퍼드 쓰기(예: ext4/f2fs 의 dirty page)가 실제 디스크로 flush 될 때
	 * 어느 cgroup 의 쓰기 대역폭 제한을 적용할지 mm/page-writeback.c 쪽에서
	 * 참조하는 연결점이다.
	 * 설정자: blkcg_css_alloc() 이 INIT_LIST_HEAD().
	 * 읽는 자: blkcg_get_cgwb_list()[block/blk-cgroup.c] 가 포인터를 반환,
	 * mm/backing-dev.c 의 wb_get_create() 등이 이 리스트에 wb 를 추가/탐색.
	 * 값 범위: 비어있음 ~ 이 cgroup 이 buffered write 를 낸 bdi 개수만큼의 노드.
	 * 동기화: cgwb 서브시스템 자체 락(cgwb_lock)으로 보호(이 헤더의 관할 밖). */
#endif
};

/*
 * [한국어]
 * css_to_blkcg - cgroup_subsys_state 포인터로부터 blkcg 를 복원
 *
 * @css: io_cgrp_subsys 에 속하는 cgroup_subsys_state 포인터 (NULL 허용)
 * @return: css 를 embed 한 struct blkcg 의 시작 주소, css 가 NULL 이면 NULL
 *
 * struct blkcg 는 첫 번째 필드로 struct cgroup_subsys_state css 를 embed 하는
 * 관용구를 쓰기 때문에, cgroup 코어가 넘겨주는 범용 css 포인터를
 * container_of() 로 되돌려 blkcg 고유 필드(blkg_tree, lhead, congestion_count 등)에
 * 접근할 수 있게 한다. 락이나 참조 획득 없이 순수 포인터 산술만 수행하므로
 * 어떤 컨텍스트(인터럽트 포함)에서도 호출 가능하다.
 * 호출자: bio_blkcg_css() 결과를 blkcg 로 바꾸는 모든 IO 경로,
 * blkg_for_each_descendant_pre/post 매크로가 css_for_each_descendant_* 로
 * 얻은 pos_css 를 blkcg 로 되돌릴 때, blkcg_parent() 등.
 * 피호출: container_of() 매크로(포인터 산술, 실제 함수 호출 아님).
 * 에러 경로: css 가 NULL 이면 그대로 NULL 반환 - 호출자가 css_to_blkcg() 호출
 * 이전에 NULL 체크를 생략할 수 있게 해주는 방어적 설계.
 *
 * 호출 체인:
 *   bio_blkcg_css() / css_for_each_descendant_* -> [css_to_blkcg] -> blkcg 필드 접근
 */
static inline struct blkcg *css_to_blkcg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct blkcg, css) : NULL;
}

/*
 * A blkcg_gq (blkg) is association between a block cgroup (blkcg) and a
 * request_queue (q).  This is used by blkcg policies which need to track
 * information per blkcg - q pair.
 *
 * There can be multiple active blkcg policies and each blkg:policy pair is
 * represented by a blkg_policy_data which is allocated and freed by each
 * policy's pd_alloc/free_fn() methods.  A policy can allocate private data
 * area by allocating larger data structure which embeds blkg_policy_data
 * at the beginning.
 */
/*
 * [한국어]
 * blkg_policy_data 는 blk-throttle, BFQ, blk-ioprio 같은 정책이
 * "cgroup + request_queue" 단위(즉 blkg 단위)로 상태를 저장하기 위한
 * 공통 헤더다. 정책은 이 구조체를 자신의 사설 데이터 구조 맨 앞에 embed 한
 * 더 큰 구조체(예: throtl_grp, bfq_group)를 pd_alloc_fn() 에서 할당하고,
 * blkg->pd[plid] 에 그 포인터를 저장한다. IO 가 blk_mq_get_request() ->
 * 정책 판단 -> nvme_queue_rq() 로 전달되는 과정에서 이 구조체를 통해
 * cgroup·request_queue 조합별 한도/우선순위 상태가 참조된다.
 */
struct blkg_policy_data {
	/* the blkg and policy id this per-policy data belongs to */
	struct blkcg_gq			*blkg;
	/* [한국어] 이 pd 가 소속된 blkg(즉 (cgroup, request_queue) 쌍)로의 역참조.
	 * 설정자: 각 정책의 pd_alloc_fn() 구현체(예: throtl_pd_alloc, bfq_pd_alloc)가
	 * 반환 직전에 대입.
	 * 읽는 자: pd_to_blkg() - 정책 구현체가 자신의 pd 포인터만 들고 있다가
	 * "이 pd 가 어느 request_queue/어느 cgroup 소속인지" 되짚어야 할 때
	 * (예: pd_offline_fn 구현체가 blkg->q 를 참조).
	 * 값 범위: 유효한 blkcg_gq 포인터(NULL 불가, pd 와 생명주기 동일).
	 * 동기화: blkg 자체의 락 규칙(RCU/refcnt)을 그대로 따르며 이 필드
	 * 자체는 불변이라 별도 보호가 필요 없다. */

	int				plid;
	/* [한국어] 이 pd 를 소유한 정책의 고유 ID(plid, policy id).
	 * blkcg_policy.plid, blkg->pd[plid], blkcg->cpd[plid] 의 인덱스와 동일한
	 * 값을 가리켜 "이 pd 가 blkg->pd[] 배열의 몇 번째 슬롯에 대응하는지"를
	 * 스스로 알 수 있게 한다.
	 * 설정자: blkg_alloc()/blkcg_activate_policy() 가 pd_alloc_fn() 호출 후
	 * pd->plid = pol->plid 로 대입.
	 * 읽는 자: 공통 helper 나 디버깅 코드가 pd 하나만 들고 정책을 역추적할 때.
	 * 값 범위: 0 ~ BLKCG_MAX_POLS-1.
	 * 동기화: 불변 값, 락 불필요. */

	bool				online;
	/* [한국어] 이 pd 가 pd_online_fn() 을 거쳐 "IO 경로에서 참조 가능한
	 * 상태"인지 나타내는 플래그.
	 * 설정자: blkg_create() 가 pd_online_fn() 호출 성공 후 true; blkg_destroy()
	 * 가 pd_offline_fn() 호출 전에 false 로 되돌림(이중 offline 방지).
	 * 읽는 자: blkg_destroy() 자신 - online 인 pd 에 대해서만 pd_offline_fn() 호출.
	 * 값 범위: true(활성, IO 판단에 참여) / false(비활성 또는 아직 초기화 전).
	 * 동기화: blkg->online 과 마찬가지로 blkcg->lock + q->queue_lock 보호 하에서 전이. */
};

/*
 * Policies that need to keep per-blkcg data which is independent from any
 * request_queue associated to it should implement cpd_alloc/free_fn()
 * methods.  A policy can allocate private data area by allocating larger
 * data structure which embeds blkcg_policy_data at the beginning.
 * cpd_init() is invoked to let each policy handle per-blkcg data.
 */
/*
 * [한국어]
 * blkcg_policy_data 는 특정 request_queue 와 무관하게 cgroup 전체에 걸쳐
 * 유지되는 정책 데이터의 공통 헤더다. blkg_policy_data 가 (cgroup,
 * request_queue) 쌍 단위였다면, 이쪽은 cgroup 하나 단위다. 예를 들어
 * blk-ioprio 는 device 별 pd 없이 이 cpd 만으로 cgroup 의 기본 우선순위를
 * 관리한다(block/blk-ioprio.c 의 ioprio_policy 참고).
 */
struct blkcg_policy_data {
	/* the blkcg and policy id this per-policy data belongs to */
	struct blkcg			*blkcg;
	/* [한국어] 이 cpd 가 소속된 cgroup 으로의 역참조.
	 * 설정자: 각 정책의 cpd_alloc_fn() 구현체(예: ioprio_alloc_cpd)가 반환
	 * 직전에 대입.
	 * 읽는 자: cpd_to_blkcg() - 정책이 cpd 포인터만 들고 소속 cgroup 을
	 * 되짚어야 할 때(예: cgroup 파일 write 핸들러가 대상 cgroup 을 확인).
	 * 값 범위: 유효한 blkcg 포인터(NULL 불가, cpd 와 생명주기 동일).
	 * 동기화: blkcg 자체의 락 규칙을 따르며 이 필드 자체는 불변. */

	int				plid;
	/* [한국어] 이 cpd 를 소유한 정책의 고유 ID(plid). blkg_policy_data.plid 와
	 * 동일한 의미로, blkcg->cpd[plid] 배열 인덱스와 일치한다.
	 * 설정자: blkcg_policy_register()/blkcg_css_alloc() 이 cpd_alloc_fn() 호출 후 대입.
	 * 읽는 자: 정책이 cpd 하나만 들고 자신의 plid 를 되짚어야 할 때.
	 * 값 범위: 0 ~ BLKCG_MAX_POLS-1.
	 * 동기화: 불변 값, 락 불필요. */
};

/*
 * [한국어]
 * 아래 typedef 들은 각 blkcg 정책(struct blkcg_policy)이 등록할 수 있는
 * 콜백 함수 포인터의 시그니처를 정의한다. blkcg_policy_register() 를 통해
 * blk-throttle/BFQ/blk-ioprio 등이 자신의 구현 함수를 이 시그니처에 맞춰
 * 등록하면, blkg/blkcg 생성·해제·활성화·비활성화·통계 조회 등 공통 라이프
 * 사이클의 각 단계에서 blk-cgroup 코어(block/blk-cgroup.c)가 이 함수
 * 포인터를 통해 정책별 로직을 호출한다(콜백/훅 패턴). "cpd" 접미사는
 * blkcg_policy_data(cgroup 단위), "pd" 접미사는 blkg_policy_data((cgroup,
 * request_queue) 단위) 대상임을 구분한다.
 */
typedef struct blkcg_policy_data *(blkcg_pol_alloc_cpd_fn)(gfp_t gfp);
/* [한국어] per-cgroup(cpd) 데이터 할당 콜백. 새 cgroup 이 생성될 때
 * (blkcg_css_alloc()) 또는 정책이 새로 등록될 때(blkcg_policy_register()가
 * 기존 모든 cgroup 에 대해) 호출되어 blkcg_policy_data 를 embed 한 더 큰
 * 구조체를 gfp 플래그로 할당해 반환한다. NULL 반환 시 호출자가 -ENOMEM 처리.
 * 사용 예: block/blk-ioprio.c 의 ioprio_alloc_cpd(). */

typedef void (blkcg_pol_init_cpd_fn)(struct blkcg_policy_data *cpd);
/* [한국어] per-cgroup(cpd) 데이터 초기화 콜백. cpd_alloc_fn() 이 성공한
 * 직후 기본값을 채우는 데 쓰도록 설계된 훅이다. 현재 트리(이 저장소 시점)의
 * struct blkcg_policy 에는 이 콜백을 담는 필드가 없어 실제로 어떤 정책도
 * 등록해 쓰지 않는 미사용(vestigial) typedef 로 남아 있다(추정: 과거
 * cpd_alloc_fn 과 분리되어 있던 API 의 흔적이거나 향후 확장을 위한 예약). */

typedef void (blkcg_pol_free_cpd_fn)(struct blkcg_policy_data *cpd);
/* [한국어] per-cgroup(cpd) 데이터 해제 콜백. cgroup 소멸(blkcg_css_free())
 * 또는 정책 등록 해제(blkcg_policy_unregister()) 시 cpd_alloc_fn() 이 만든
 * 구조체를 되돌려 받아 해제한다. cpd_alloc_fn 과 반드시 쌍으로 등록해야
 * 하며(blkcg_policy_register() 가 XOR 검사로 강제), 사용 예는 block/
 * blk-ioprio.c 의 ioprio_free_cpd(). */

typedef void (blkcg_pol_bind_cpd_fn)(struct blkcg_policy_data *cpd);
/* [한국어] cgroup 이 특정 계층(hierarchy)에 바인딩될 때를 위한 훅으로
 * 선언되어 있으나, blkcg_pol_init_cpd_fn 과 마찬가지로 현재 struct
 * blkcg_policy 어떤 필드도 이 타입을 사용하지 않는 미사용 typedef 다. */

typedef struct blkg_policy_data *(blkcg_pol_alloc_pd_fn)(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp);
/* [한국어] per-blkg(pd) 데이터 할당 콜백. blkg_alloc() 이 새 blkg 를 만들 때,
 * 또는 blkcg_activate_policy() 가 기존 모든 blkg 에 대해 정책을 새로
 * 활성화할 때 호출되어 disk(대상 gendisk/request_queue)·blkcg(대상
 * cgroup) 조합에 대한 blkg_policy_data 를 gfp 플래그로 할당한다.
 * NVMe 관점: disk 는 보통 NVMe namespace 의 gendisk 다.
 * 사용 예: block/blk-throttle.c 의 throtl_pd_alloc(). */

typedef void (blkcg_pol_init_pd_fn)(struct blkg_policy_data *pd);
/* [한국어] per-blkg(pd) 데이터 초기화 콜백. pd_alloc_fn() 성공 직후
 * blkg_create() 가 호출해 pd 의 초기값(예: throtl_grp 의 slice 기본값)을 채운다.
 * 사용 예: block/blk-throttle.c 의 throtl_pd_init(). */

typedef void (blkcg_pol_online_pd_fn)(struct blkg_policy_data *pd);
/* [한국어] pd 를 "IO 경로에서 참조 가능한" online 상태로 전환하는 콜백.
 * blkg_create() 가 tree/list 등록 직후 호출하며, 이 호출이 끝나야 실제
 * IO(bio/request)가 이 정책 상태를 참조하기 시작한다.
 * 사용 예: block/blk-throttle.c 의 throtl_pd_online() - 상위 service_queue 와의
 * 연결을 완성한다. */

typedef void (blkcg_pol_offline_pd_fn)(struct blkg_policy_data *pd);
/* [한국어] pd 를 online 에서 되돌리는(offline) 콜백. blkg_destroy() 가
 * 호출하며, cgroup 삭제나 디스크 제거 시 정책이 미완료 상태(예: 아직 처리
 * 중인 throttle 큐)를 강제로 flush/drain 할 기회를 준다.
 * 사용 예: block/blk-throttle.c 의 throtl_pd_offline(). */

typedef void (blkcg_pol_free_pd_fn)(struct blkg_policy_data *pd);
/* [한국어] per-blkg(pd) 데이터 해제 콜백. blkg_free_workfn() 이 blkg 를
 * 최종 회수하기 직전에 호출해 pd_alloc_fn() 이 만든 구조체를 반환한다.
 * cpd 와 마찬가지로 pd_alloc_fn 과 반드시 쌍으로 등록해야 한다.
 * 사용 예: block/blk-throttle.c 의 throtl_pd_free(). */

typedef void (blkcg_pol_reset_pd_stats_fn)(struct blkg_policy_data *pd);
/* [한국어] 이 pd 의 누적 통계(예: 처리한 바이트/IOPS 카운터)를 0 으로
 * 리셋하는 콜백. cgroupfs 의 stat reset 인터페이스(예: blkio.reset_stats)
 * write 핸들러 경로에서 blkcg_reset_stats() 가 각 정책에 대해 호출한다. */

typedef void (blkcg_pol_stat_pd_fn)(struct blkg_policy_data *pd,
				struct seq_file *s);
/* [한국어] 이 pd 가 보유한 정책별 통계를 seq_file(cgroupfs 텍스트 파일
 * 출력 인터페이스)에 출력하는 콜백. blkcg_print_blkgs() 가 각 blkg 순회
 * 중 정책이 이 콜백을 등록했으면 호출해 "Total"/장치별 라인에 이어 붙일
 * 추가 통계 문자열을 만들게 한다. */

/*
 * [한국어]
 * struct blkcg_policy 는 하나의 cgroup IO 정책(예: blk-throttle, BFQ,
 * blk-ioprio)을 커널에 등록할 때 채워 blkcg_policy_register() 에 넘기는
 * 구조체다. cftype 배열로 cgroupfs 파일을, 각 콜백 함수 포인터로 cpd/pd
 * 라이프사이클(할당·초기화·온라인·오프라인·해제·통계)을 정의한다. NVMe
 * SSD 라면 이 정책들이 request 발행 전/후에 개입해 대역폭/IOPS 를 제어하는
 * 진입점이 바로 이 구조체가 가리키는 함수들이다.
 */
struct blkcg_policy {
	int				plid;
	/* [한국어] 이 정책의 고유 ID. blkcg_policy_register() 가 blkcg_policy[]
	 * 전역 배열에서 빈 슬롯을 찾아 부여하며, 이후 이 값이 blkg->pd[plid],
	 * blkcg->cpd[plid], q->blkcg_pols 비트마스크의 인덱스로 두루 쓰인다.
	 * 설정자: blkcg_policy_register() 내부에서 for 루프로 빈 슬롯 탐색 후 대입.
	 * 읽는 자: blkcg_policy_enabled() 가 test_bit(pol->plid, q->blkcg_pols).
	 * 값 범위: 0 ~ BLKCG_MAX_POLS-1.
	 * 동기화: blkcg_pol_mutex 로 등록/해제 시 보호. */

	/* cgroup files for the policy */
	struct cftype			*dfl_cftypes;
	/* [한국어] cgroup v2(unified hierarchy) 에 노출할 cgroupfs 파일 배열
	 * (예: blk-throttle 의 io.max, blk-ioprio 의 io.prio.class).
	 * 설정자: 정책 모듈이 정적으로 초기화(예: blk-throttle.c 의 throtl_files).
	 * 등록/해제: blkcg_policy_register()/unregister() 가 cgroup_add_dfl_cftypes()/
	 * cgroup_rm_cftypes() 로 io_cgrp_subsys 에 등록·제거.
	 * 값 범위: NULL(v2 인터페이스 없음) 또는 NULL 종료(sentinel) cftype 배열.
	 * 동기화: cgroup 코어의 cftype 등록 규약(내부적으로 cgroup_mutex 사용). */

	struct cftype			*legacy_cftypes;
	/* [한국어] cgroup v1(legacy, "blkio" 이름의 계층) 에 노출할 파일 배열
	 * (예: blk-throttle 의 blkio.throttle.read_bps_device 등).
	 * 설정자/등록/해제: dfl_cftypes 와 동일한 방식이나 cgroup_add_legacy_cftypes() 사용.
	 * 값 범위: NULL(v1 인터페이스 미제공) 또는 cftype 배열.
	 * 동기화: dfl_cftypes 와 동일. */

	/* operations */
	blkcg_pol_alloc_cpd_fn		*cpd_alloc_fn;
	/* [한국어] per-cgroup(cpd) 데이터 할당 함수. NULL 이면 이 정책은
	 * cgroup 단위 사설 데이터가 없다는 뜻(예: blk-throttle 은 device 단위
	 * pd 만 쓰고 cpd 는 쓰지 않는다).
	 * cpd_free_fn 과 반드시 쌍으로 존재해야 함(blkcg_policy_register() 가
	 * "(!cpd_alloc_fn ^ !cpd_free_fn)" 로 XOR 검사해 어긋나면 -EINVAL). */

	blkcg_pol_free_cpd_fn		*cpd_free_fn;
	/* [한국어] cpd_alloc_fn() 이 만든 cpd 를 해제하는 함수. cpd_alloc_fn 과
	 * 쌍을 이루며, 정책 해제(blkcg_policy_unregister())와 cgroup 소멸
	 * (blkcg_css_free()) 양쪽에서 호출된다. */

	blkcg_pol_alloc_pd_fn		*pd_alloc_fn;
	/* [한국어] per-blkg(pd) 데이터 할당 함수. NULL 이면 이 정책은 device
	 * (request_queue) 단위 사설 데이터가 없다는 뜻(예: blk-ioprio 는 cpd 만
	 * 쓰고 pd_alloc_fn 은 등록하지 않는다 - device 독립적으로 동작).
	 * pd_free_fn 과 반드시 쌍으로 존재해야 하며, 이 콜백이 있는 정책만
	 * blkcg_activate_policy() 로 특정 gendisk(NVMe namespace 등)에 활성화될 수 있다. */

	blkcg_pol_init_pd_fn		*pd_init_fn;
	/* [한국어] pd_alloc_fn() 직후 pd 의 초기값을 채우는 함수(선택적, NULL 가능). */

	blkcg_pol_online_pd_fn		*pd_online_fn;
	/* [한국어] pd 를 IO 경로에서 참조 가능한 상태로 전환하는 함수(선택적).
	 * blkg_create() 가 tree/list 등록 직후 호출 -> 이 시점부터 request_queue
	 * 를 지나는 실제 IO 가 이 정책의 판단을 받기 시작한다. */

	blkcg_pol_offline_pd_fn		*pd_offline_fn;
	/* [한국어] pd 를 online 에서 되돌리는 함수(선택적). blkg_destroy() 가
	 * 호출하며, 큐가 dying(QUEUE_FLAG_DYING) 상태로 전환되거나 cgroup 이
	 * 삭제될 때 진행 중이던 정책 상태(예: 대기 중인 throttle 항목)를 정리한다. */

	blkcg_pol_free_pd_fn		*pd_free_fn;
	/* [한국어] pd_alloc_fn() 이 만든 pd 를 최종 해제하는 함수. pd_alloc_fn 이
	 * 설정되어 있다면 이 함수도 반드시 있어야 한다(위 XOR 검사 대상). */

	blkcg_pol_reset_pd_stats_fn	*pd_reset_stats_fn;
	/* [한국어] 이 pd 의 누적 통계를 리셋하는 함수(선택적, cgroupfs 의
	 * stat reset 파일 write 핸들러가 모든 활성 정책에 대해 호출). */

	blkcg_pol_stat_pd_fn		*pd_stat_fn;
	/* [한국어] 이 pd 의 정책별 통계를 seq_file 에 출력하는 함수(선택적).
	 * blkcg_print_blkgs() 가 각 blkg 에 대해 정책이 이 콜백을 등록했으면 호출. */
};

extern struct blkcg blkcg_root;
/* [한국어] 시스템 전체의 root cgroup 인스턴스(정적 전역, block/blk-cgroup.c
 * 에 실체가 있다). 모든 request_queue 는 blkcg_init_disk() 시점에 이
 * blkcg_root 에 대응하는 q->root_blkg 를 미리 만들어 두므로, cgroup 을
 * 별도로 쓰지 않는 IO 나 아직 분류되지 않은 IO 는 항상 이 root cgroup 으로
 * 귀속된다. blkg_lookup() 이 blkcg == &blkcg_root 인 경우 radix tree 조회
 * 없이 q->root_blkg 를 즉시 반환하는 것도 이 때문(hot path 최적화). */

extern bool blkcg_debug_stats;
/* [한국어] "디버그 통계"(module_param 로 노출되는 blkcg_debug_stats)를
 * cgroupfs 출력에 포함할지 여부. true 면 blkcg_print_blkgs() 계열 출력에
 * use_delay/delay_nsec 같은 내부 스로틀 상태까지 노출해 디버깅을 돕는다.
 * 기본값 false(모듈 파라미터로 부팅 시 또는 sysfs 로 변경 가능). */

/*
 * [한국어]
 * blkg_init_queue - request_queue 의 blkcg 관련 필드(blkg_list, blkcg_mutex) 초기화
 *
 * @q: 초기화할 request_queue
 * @return: 없음(void)
 *
 * 새 request_queue 가 만들어질 때 이 큐에 매달릴 blkg 들을 담을 리스트와
 * 그 리스트를 보호할 mutex 를 준비해 두는 최초 1회성 초기화 함수다. 아직
 * 어떤 blkg 도 생성되지 않은 시점에 호출되므로 락 없이 단순 초기화만 수행.
 * 호출자: blk_alloc_queue() - request_queue 자체가 만들어지는 극초기 단계.
 * 피호출: INIT_LIST_HEAD(), mutex_init() (둘 다 자료구조 초기화 헬퍼, 실제
 * 커널 서비스 호출 아님).
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   blk_alloc_queue() -> [blkg_init_queue] -> (이후) blkcg_init_disk() -> blkg_create()
 */
void blkg_init_queue(struct request_queue *q);

/*
 * [한국어]
 * blkcg_init_disk - gendisk 에 대한 root blkg 생성 및 blkcg 관련 초기화
 *
 * @disk: 새로 등록되는 gendisk(디스크 전체를 나타내는 상위 객체)
 * @return: 0 성공, 음수 -errno 실패(예: -ENOMEM). 실패 시 disk 등록 자체가 중단됨.
 *
 * gendisk(예: NVMe 라면 namespace 에 대응)가 시스템에 처음 등록될 때
 * blkcg_root(root cgroup)에 대한 blkg 를 만들어 disk->queue->root_blkg 에
 * 연결한다. 이후 bio 가 어떤 cgroup 에도 명시적으로 속하지 않거나 그
 * cgroup 의 blkg 조회가 실패하면 이 root_blkg 로 spill 된다.
 * 호출자: 디스크 등록 경로(add_disk() 계열)가 gendisk 초기화 단계에서 호출.
 * 피호출: blkg_alloc(), blkg_create(), radix_tree_preload() 등.
 * 에러 경로: 메모리 부족 시 -ENOMEM 을 반환해 디스크 등록을 실패시킨다.
 *
 * 호출 체인:
 *   add_disk() 계열 -> [blkcg_init_disk] -> blkg_alloc() -> blkg_create()
 */
int blkcg_init_disk(struct gendisk *disk);

/*
 * [한국어]
 * blkcg_exit_disk - gendisk 제거 시 blk-cgroup 리소스 정리
 *
 * @disk: 제거되는 gendisk
 * @return: 없음(void)
 *
 * blkcg_init_disk() 의 역순 정리 함수. 이 disk 의 request_queue 에 매달린
 * 모든 blkg 를 destroy 하고 root_blkg 를 비워, 이후 blkg_lookup() 이 이
 * queue 에 대해 더 이상 유효한 결과를 내지 않게 한다.
 * 호출자: 디스크 제거 경로(del_gendisk() 계열).
 * 피호출: blkg_destroy_all() 류(block/blk-cgroup.c), q->queue_lock 잠금 헬퍼.
 * 에러 경로: 없음(정리 함수는 실패하지 않도록 설계).
 *
 * 호출 체인:
 *   del_gendisk() 계열 -> [blkcg_exit_disk] -> blkg_destroy_all() -> blkg_destroy()
 */
void blkcg_exit_disk(struct gendisk *disk);

/* Blkio controller policy registration */
/*
 * [한국어]
 * blkcg_policy_register - 새 blkcg 정책(blk-throttle/BFQ/blk-ioprio 등)을 커널에 전역 등록
 *
 * @pol: 등록할 blkcg_policy(cftype·콜백 테이블이 채워진 정적/전역 구조체)
 * @return: 0 성공(빈 슬롯 발견 및 cftype 등록 성공), 음수 -errno 실패
 *   (-EINVAL: cpd/pd alloc·free 콜백 쌍이 맞지 않음, -ENOSPC: BLKCG_MAX_POLS 초과)
 *
 * 정책 모듈이 자신을 처음 초기화할 때(예: throttle 모듈 init) 호출하는
 * 최상위 등록 함수. blkcg_policy[] 전역 테이블에서 빈 slot 을 찾아 pol->plid
 * 를 부여하고, 이미 존재하는 모든 cgroup(all_blkcgs)에 대해 cpd_alloc_fn() 을
 * 호출해 cpd 를 채운 뒤, cftype 배열을 cgroupfs 에 추가한다. 이 시점에는
 * 아직 어떤 request_queue 에도 활성화되지 않은 "등록"일 뿐이며, 실제
 * 디스크에 적용하려면 blkcg_activate_policy() 를 별도로 호출해야 한다.
 * 호출자: blk_throtl_init(), bfq 모듈 init 등 정책 서브시스템 초기화 코드.
 * 피호출: mutex_lock(blkcg_pol_register_mutex/blkcg_pol_mutex), cpd_alloc_fn(),
 * cgroup_add_dfl_cftypes()/cgroup_add_legacy_cftypes().
 * 에러 경로: alloc/free 콜백 쌍이 어긋나면 시작 전에 -EINVAL; 빈 슬롯이
 * 없으면 -ENOSPC; cpd_alloc_fn() 실패 시 이미 할당한 cpd 들을 롤백.
 *
 * 호출 체인:
 *   blk_throtl_init() / bfq_init() -> [blkcg_policy_register] -> cpd_alloc_fn() (모든 cgroup)
 */
int blkcg_policy_register(struct blkcg_policy *pol);

/*
 * [한국어]
 * blkcg_policy_unregister - 등록된 blkcg 정책을 커널에서 전역 해제
 *
 * @pol: blkcg_policy_register() 로 등록했던 동일 구조체
 * @return: 없음(void)
 *
 * cftype 파일을 cgroupfs 에서 먼저 제거한 뒤, blkcg_policy[pol->plid] 슬롯을
 * 비우고 모든 cgroup 의 cpd 를 cpd_free_fn() 으로 해제한다. 호출 전에
 * 이 정책이 활성화되어 있던 모든 request_queue 에 대해 blkcg_deactivate_policy()
 * 가 먼저 호출되어 있어야 한다(그렇지 않으면 활성 blkg->pd[] 가 매달린 채
 * 정책 콜백만 사라지는 위험한 상태가 될 수 있음).
 * 호출자: 정책 모듈 종료(module_exit) 경로.
 * 피호출: cgroup_rm_cftypes(), cpd_free_fn().
 * 에러 경로: WARN_ON 으로 슬롯 불일치(이미 다른 정책이거나 미등록)를 방어적으로 검출.
 *
 * 호출 체인:
 *   정책 모듈 module_exit -> [blkcg_policy_unregister] -> cpd_free_fn() (모든 cgroup)
 */
void blkcg_policy_unregister(struct blkcg_policy *pol);

/*
 * [한국어]
 * blkcg_activate_policy - 등록된 정책을 특정 gendisk(request_queue)에 실제로 활성화
 *
 * @disk: 정책을 적용할 대상 gendisk
 * @pol: blkcg_policy_register() 로 이미 전역 등록된 정책
 * @return: 0 성공(이미 활성화되어 있었다면 no-op 으로 0), 음수 -errno 실패
 *
 * 이 disk 의 request_queue 에 매달린 기존 모든 blkg 를 순회하며 pd_alloc_fn()
 * 으로 정책 사설 데이터를 새로 붙이고, 성공하면 q->blkcg_pols 비트마스크에
 * pol->plid 비트를 세팅해 이후 blkcg_policy_enabled() 가 true 를 반환하게
 * 한다. GFP_KERNEL 컨텍스트가 필요해(잠재적으로 sleep) 큐를 freeze 한
 * 상태에서 진행되며, 이는 활성화 도중 IO 경로가 절반만 초기화된 pd 를
 * 관측하지 않도록 하기 위함이다.
 * 호출자: 정책이 특정 디스크에 켜질 때(elevator 전환, throttle cgroup 파일
 * write 등으로 트리거).
 * 피호출: blk_mq_freeze_queue()/unfreeze_queue(), pol->pd_alloc_fn()/pd_init_fn().
 * 에러 경로: pd_alloc_fn() 실패 시 그때까지 할당한 pd 들을 롤백하고 -ENOMEM 반환;
 * pd_alloc_fn/pd_free_fn 이 정책에 없으면(cgroup 레벨 전용 정책) WARN_ON_ONCE.
 *
 * 호출 체인:
 *   cgroup 파일 write / elevator 전환 -> [blkcg_activate_policy] -> pd_alloc_fn() (모든 blkg)
 */
int blkcg_activate_policy(struct gendisk *disk, const struct blkcg_policy *pol);

/*
 * [한국어]
 * blkcg_deactivate_policy - 특정 gendisk 에서 정책을 비활성화
 *
 * @disk: 대상 gendisk
 * @pol: 비활성화할 정책
 * @return: 없음(void)
 *
 * blkcg_activate_policy() 의 역과정. q->blkcg_pols 비트를 지워 이후 IO 가
 * 이 정책을 더 이상 참조하지 않게 한 뒤, 모든 blkg 에 대해 pd_offline_fn()
 * 과 pd_free_fn() 을 호출해 pd 를 회수한다.
 * 호출자: 정책을 특정 디스크에서 끌 때(elevator 전환 등).
 * 피호출: blk_mq_freeze_queue(), pol->pd_offline_fn()/pd_free_fn().
 * 에러 경로: 없음(비활성화는 실패하지 않도록 설계, 이미 비활성 상태면 no-op).
 *
 * 호출 체인:
 *   elevator 전환 등 -> [blkcg_deactivate_policy] -> pd_offline_fn()/pd_free_fn() (모든 blkg)
 */
void blkcg_deactivate_policy(struct gendisk *disk,
			     const struct blkcg_policy *pol);

/*
 * [한국어]
 * blkg_dev_name - blkg 가 속한 블록 장치의 표시 이름을 반환
 *
 * @blkg: 이름을 얻을 대상 blkg
 * @return: "8:0" 형태가 아닌 사람이 읽는 bdi 이름 문자열(예: "nvme0n1"), disk 가
 *   없으면 NULL
 *
 * cgroupfs 통계 출력 시 "장치 이름 값" 형태의 각 라인 맨 앞에 붙일 장치
 * 이름을 얻는다.
 * 호출자: __blkg_prfill_u64(), 각 정책의 pd_stat_fn() 구현체.
 * 피호출: bdi_dev_name().
 * 에러 경로: blkg->q->disk 가 아직 없으면(초기화 중이거나 이미 제거됨) NULL 반환,
 * 호출자는 이를 "출력하지 않음"으로 처리.
 *
 * 호출 체인:
 *   __blkg_prfill_u64() / pd_stat_fn() -> [blkg_dev_name] -> bdi_dev_name()
 */
const char *blkg_dev_name(struct blkcg_gq *blkg);

/*
 * [한국어]
 * blkcg_print_blkgs - 한 cgroup 에 속한 모든 blkg 에 대해 prfill 콜백을 호출해 통계 출력
 *
 * @sf: 출력 대상 seq_file(cgroupfs read 핸들러가 제공)
 * @blkcg: 통계를 출력할 대상 cgroup
 * @prfill: blkg_policy_data 하나를 문자열로 변환해 @sf 에 쓰는 콜백(예: __blkg_prfill_u64)
 * @pol: 대상 정책(이 정책이 활성화된 blkg 만 골라 출력)
 * @data: prfill 에 그대로 전달할 부가 데이터(정책마다 의미 다름)
 * @show_total: true 면 모든 prfill 반환값의 합을 "Total" 로 마지막에 출력
 * @return: 없음(void)
 *
 * cftype->seq_show 구현체들이 "장치별 한 줄씩 + 필요하면 합계 한 줄"
 * 형태의 표준적인 cgroupfs 출력 포맷을 만들 때 재사용하는 공통 helper.
 * blkcg->blkg_list 를 RCU 로 순회하며, 정책이 활성화된 blkg 에 대해서만
 * q->queue_lock 을 잡고 prfill() 을 호출한다.
 * 호출자: 각 정책의 cftype->seq_show 콜백(예: throtl 의 tg_print_conf_u64 류).
 * 피호출: prfill(사용자 제공), blkcg_policy_enabled().
 * 에러 경로: 없음(정책이 비활성화된 blkg 는 그냥 건너뜀).
 *
 * 호출 체인:
 *   cftype->seq_show -> [blkcg_print_blkgs] -> prfill() (활성화된 각 blkg 마다)
 */
void blkcg_print_blkgs(struct seq_file *sf, struct blkcg *blkcg,
		       u64 (*prfill)(struct seq_file *,
				     struct blkg_policy_data *, int),
		       const struct blkcg_policy *pol, int data,
		       bool show_total);

/*
 * [한국어]
 * __blkg_prfill_u64 - "장치이름 값" 한 줄을 출력하는 표준 prfill 콜백
 *
 * @sf: 출력 대상 seq_file
 * @pd: 값을 낼 blkg_policy_data(여기서 pd->blkg 로 장치 이름을 얻는다)
 * @v: 출력할 u64 값
 * @return: 호출자(blkcg_print_blkgs)가 합계를 낼 때 더할 값 - 성공 시 @v 그대로,
 *   장치 이름이 없으면 0(합계에 영향 없도록)
 *
 * blkcg_print_blkgs() 의 @prfill 인자로 그대로 꽂아 쓸 수 있는 범용 구현체.
 * 단순 u64 카운터 하나만 출력하면 되는 정책(간단한 IOPS/바이트 카운터 등)이
 * 자체 prfill 함수를 새로 작성하지 않고 재사용하도록 제공된다.
 * 호출자: blkcg_print_blkgs() 가 각 blkg 마다 호출.
 * 피호출: blkg_dev_name(), seq_printf().
 * 에러 경로: blkg_dev_name() 이 NULL 이면(장치 미연결) 아무것도 출력하지 않고 0 반환.
 *
 * 호출 체인:
 *   blkcg_print_blkgs() -> [__blkg_prfill_u64] -> blkg_dev_name() / seq_printf()
 */
u64 __blkg_prfill_u64(struct seq_file *sf, struct blkg_policy_data *pd, u64 v);

/*
 * [한국어]
 * struct blkg_conf_ctx - "MAJ:MIN 정책설정값" 형식의 cgroup 설정 파일 입력을
 * 파싱하는 동안 상태를 나르는 컨텍스트. blkg_conf_init() 으로 초기화하고,
 * blkg_conf_open_bdev()(또는 그 frozen 버전)와 blkg_conf_prep() 을 거쳐
 * @blkg 를 확정한 뒤, 반드시 blkg_conf_exit()(또는 frozen 버전)로 정리해야
 * 한다(그렇지 않으면 bdev 참조·queue_lock·rq_qos_mutex 가 누수/미해제된다).
 * 사용 예: "echo '8:0 rbps=1048576' > io.max" 같은 cgroupfs write 핸들러가
 * 이 컨텍스트를 스택에 두고 파싱-검증-적용-정리 순으로 사용한다.
 */
struct blkg_conf_ctx {
	char				*input;
	/* [한국어] 파싱 중인 입력 문자열의 현재 위치를 가리키는 포인터.
	 * 설정자: blkg_conf_init() 이 사용자가 넘긴 input 을 그대로 저장.
	 * 갱신자: blkg_conf_open_bdev() 가 "MAJ:MIN" 부분을 소비한 뒤 그
	 * 다음(공백 스킵 이후) 위치로 포인터를 전진시키는 데 이 필드를 임시로
	 * 사용(최종적으로는 body 필드가 그 결과를 저장).
	 * 읽는 자: blkg_conf_open_bdev() 가 sscanf("%u:%u%n", ...) 로 major:minor 파싱.
	 * 값 범위: 호출자가 소유한 버퍼를 가리키는 non-owning 포인터(이 구조체가
	 * 메모리를 복사/소유하지 않음 - 원본 버퍼가 파싱 동안 유효해야 함).
	 * 동기화: 이 ctx 를 스택에 둔 단일 호출 스레드만 접근하므로 락 불필요. */

	char				*body;
	/* [한국어] "MAJ:MIN" 부분을 건너뛴 뒤, 정책별 나머지 설정값이 시작되는 위치.
	 * 설정자: blkg_conf_open_bdev() 가 input 파싱 완료 후 대입.
	 * 읽는 자: blkg_conf_prep() 호출자(각 정책의 cftype->write 핸들러)가 이
	 * 지점부터 자신의 정책 전용 문법(예: "rbps=1048576 wiops=200")을 파싱.
	 * 값 범위: input 버퍼 내부를 가리키는 포인터, bdev 파싱 실패 시 NULL.
	 * 동기화: input 과 동일. */

	struct block_device		*bdev;
	/* [한국어] "MAJ:MIN" 으로 지목된 블록 장치를 blkdev_get_no_open() 으로 연
	 * (open 은 아니고 참조만 획득한) block_device 포인터.
	 * 설정자: blkg_conf_open_bdev()/blkg_conf_open_bdev_frozen().
	 * 해제자: blkg_conf_exit()/blkg_conf_exit_frozen() 이 blkdev_put_no_open().
	 * 값 범위: 유효한 block_device 포인터, 아직 열리지 않았으면 NULL(NULL 이면
	 * "아직 open 안 됨"의 신호로도 쓰여 blkg_conf_open_bdev() 의 재호출을 막는다).
	 * 동기화: 이 bdev 의 큐에 대한 rq_qos_mutex/queue_lock 을 함께 다루므로
	 * lock 순서(먼저 rq_qos_mutex, freeze 필요 시 그 사이에 unlock/relock)에 주의. */

	struct blkcg_gq			*blkg;
	/* [한국어] 최종적으로 설정을 적용할 대상 blkg(파싱된 cgroup 과 bdev 의 조합).
	 * 설정자: blkg_conf_prep() 이 blkg_lookup() 또는 blkg_create() 로 확정.
	 * 읽는 자: 호출자(cftype->write 핸들러)가 정책별 필드(예: throtl_grp)에
	 * 실제 설정값을 반영할 때 blkg_to_pd(ctx->blkg, pol) 로 pd 를 얻는다.
	 * 값 범위: blkg_conf_prep() 성공 후에는 유효한 포인터, blkg_conf_exit() 가
	 * queue_lock 을 해제하면서 NULL 로 리셋.
	 * 동기화: blkg_conf_prep() 은 성공 시 q->queue_lock 을 잡은 채로 반환하며,
	 * 이 필드가 유효한 동안은 그 lock 이 blkg 의 생존을 보장한다(반드시
	 * blkg_conf_exit() 로 짝을 맞춰 해제해야 함, __acquires/__releases 애노테이션 참고). */
};

/*
 * [한국어]
 * blkg_conf_init - blkg_conf_ctx 를 입력 문자열로 초기화
 *
 * @ctx: 초기화할 blkg_conf_ctx(보통 호출자의 스택 변수)
 * @input: 파싱할 입력 문자열(예: cgroupfs write 핸들러가 받은 사용자 버퍼)
 * @return: 없음(void)
 *
 * *ctx = (struct blkg_conf_ctx){ .input = input } 형태로 나머지 필드를 모두
 * 0/NULL 로 초기화하면서 input 만 채운다. 이후 blkg_conf_open_bdev()/
 * blkg_conf_prep() 과 함께 쓰고, 반드시 blkg_conf_exit() 로 정리해야 한다.
 * 호출자: 각 정책 cftype->write 핸들러의 진입부.
 * 피호출: 없음(단순 구조체 대입).
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   cftype->write 핸들러 -> [blkg_conf_init] -> blkg_conf_open_bdev()/blkg_conf_prep()
 */
void blkg_conf_init(struct blkg_conf_ctx *ctx, char *input);

/*
 * [한국어]
 * blkg_conf_open_bdev - 입력 문자열에서 "MAJ:MIN" 을 파싱해 block_device 를 연다
 *
 * @ctx: blkg_conf_init() 으로 초기화된 컨텍스트
 * @return: 0 성공, 음수 -errno 실패(-EINVAL: 형식 오류, -ENODEV: 장치 없음/
 *   partition/live 아님)
 *
 * ctx->input 앞부분의 "MAJ:MIN"(예: "259:0")을 sscanf 로 파싱해 major/minor
 * 번호를 얻고, blkdev_get_no_open() 으로 block_device 참조를 획득한다.
 * partition 장치나 아직 live 하지 않은(초기화 중/제거 중) 디스크는 거부한다.
 * 성공 시 q->rq_qos_mutex 를 잡은 채로 반환하며, ctx->body/ctx->bdev 를 채운다.
 * blkg_conf_prep() 이 내부적으로 이 함수를 호출하므로, cgroup/정책 부분까지
 * 해석하지 않고 bdev 접근만 필요할 때 직접 호출해도 된다(이미 bdev 가
 * 열려 있으면 아무 일도 하지 않는 NOOP).
 * 호출자: blkg_conf_prep(), 또는 bdev 만 필요한 다른 write 핸들러.
 * 피호출: sscanf(), blkdev_get_no_open(), disk_live(), mutex_lock(rq_qos_mutex).
 * 에러 경로: 파싱 실패/장치 없음/partition/미live 상태 각각에서 즉시 -errno 반환.
 *
 * 호출 체인:
 *   blkg_conf_prep() / cftype->write 핸들러 -> [blkg_conf_open_bdev] -> blkdev_get_no_open()
 */
int blkg_conf_open_bdev(struct blkg_conf_ctx *ctx);

/*
 * [한국어]
 * blkg_conf_open_bdev_frozen - blkg_conf_open_bdev() 와 동일하되 큐를 freeze 까지 수행
 *
 * @ctx: blkg_conf_init() 으로 초기화된 컨텍스트
 * @return: 음수면 -errno 실패, 0 이상이면 memflags(추후 blkg_conf_exit_frozen()
 *   에 그대로 되돌려줘야 하는 memalloc scope 값)
 *
 * freeze 와 q->rq_qos_mutex 사이의 lock 순서(freeze 를 먼저, QoS mutex 를
 * 나중에 잡아야 함)를 올바르게 지키기 위해, blkg_conf_open_bdev() 가 잡은
 * rq_qos_mutex 를 일단 풀고 blk_mq_freeze_queue() 를 호출한 뒤 다시 mutex 를
 * 잡는 절차를 캡슐화한다. 정책 설정 변경이 in-flight IO 와 경쟁하지 않도록
 * 큐 전체를 잠시 멈춰야 하는 호출자(예: 큐 depth 에 영향을 주는 설정)가 사용.
 * 호출자: freeze 가 필요한 정책의 cftype->write 핸들러.
 * 피호출: blkg_conf_open_bdev(), blk_mq_freeze_queue().
 * 에러 경로: 이미 ctx->bdev 가 열려 있으면 -EINVAL; open_bdev 자체 실패 시 그 -errno 전파.
 *
 * 호출 체인:
 *   cftype->write 핸들러 -> [blkg_conf_open_bdev_frozen] -> blk_mq_freeze_queue()
 */
unsigned long blkg_conf_open_bdev_frozen(struct blkg_conf_ctx *ctx);

/*
 * [한국어]
 * blkg_conf_prep - cgroup 설정 입력을 파싱하고 대상 blkg 를 확정
 *
 * @blkcg: 설정을 적용할 대상 cgroup
 * @pol: 이 설정이 속한 정책(비활성화 상태면 -EOPNOTSUPP)
 * @ctx: blkg_conf_init() 으로 초기화된 컨텍스트(필요하면 blkg_conf_open_bdev()
 *   가 이미 호출되어 있어도 됨)
 * @return: 0 성공(ctx->body/bdev/blkg 모두 확정), 음수 -errno 실패
 *
 * blkg_conf_open_bdev() 로 bdev 를 연 뒤, @blkcg 에 대해 아직 blkg 가 없으면
 * blkcg_root 부터 @blkcg 까지 내려오며 부모 blkg 들을 순서대로 생성해
 * (모든 non-root blkg 가 parent 를 갖는다는 불변식 유지) 최종적으로
 * ctx->blkg 를 채운다. 성공 시 q->queue_lock 을 잡은 채로 반환하므로,
 * 호출자는 설정을 반영한 뒤 반드시 blkg_conf_exit() 를 호출해야 한다.
 * 호출자: 각 정책의 cftype->write 핸들러(예: io.max, blkio.weight write).
 * 피호출: blkg_conf_open_bdev(), blkg_lookup(), blkg_alloc(), blkg_create(),
 * radix_tree_preload().
 * 에러 경로: 정책 비활성화 시 -EOPNOTSUPP; 메모리 부족 -ENOMEM; queue 가
 * bypass 중이면 -EBUSY 를 받아 잠시 대기 후 restart_syscall() 로 시스템
 * 콜 재시작을 호출자에게 요청.
 *
 * 호출 체인:
 *   cftype->write 핸들러 -> [blkg_conf_prep] -> blkg_create() (부모 체인 순회)
 */
int blkg_conf_prep(struct blkcg *blkcg, const struct blkcg_policy *pol,
		   struct blkg_conf_ctx *ctx);

/*
 * [한국어]
 * blkg_conf_exit - blkg_conf_prep()/open_bdev() 이후의 정리(락 해제·참조 반납)
 *
 * @ctx: 정리할 컨텍스트
 * @return: 없음(void)
 *
 * ctx->blkg 가 설정되어 있으면(blkg_conf_prep 성공) q->queue_lock 을 해제하고,
 * ctx->bdev 가 있으면 rq_qos_mutex 해제 후 blkdev_put_no_open() 으로 참조를
 * 반납한다. blkg_conf_init() 으로 초기화된 모든 ctx 는 경로(성공/실패 무관)에
 * 관계없이 반드시 이 함수로 정리해야 한다.
 * 호출자: cftype->write 핸들러의 정리(cleanup) 구간.
 * 피호출: spin_unlock_irq(queue_lock), mutex_unlock(rq_qos_mutex), blkdev_put_no_open().
 * 에러 경로: 없음(정리 함수는 실패하지 않음, 이미 정리된 ctx 에 대해서도 안전).
 *
 * 호출 체인:
 *   cftype->write 핸들러 -> [blkg_conf_exit] -> blkdev_put_no_open()
 */
void blkg_conf_exit(struct blkg_conf_ctx *ctx);

/*
 * [한국어]
 * blkg_conf_exit_frozen - blkg_conf_exit() 과 동일하되 freeze 된 큐를 unfreeze 까지 수행
 *
 * @ctx: 정리할 컨텍스트(blkg_conf_open_bdev_frozen() 으로 열었던 것)
 * @memflags: blkg_conf_open_bdev_frozen() 이 반환했던 memalloc scope 값
 * @return: 없음(void)
 *
 * blkg_conf_exit() 을 호출해 일반 정리를 마친 뒤, blk_mq_unfreeze_queue() 로
 * 큐를 다시 활성화한다. blkg_conf_open_bdev_frozen() 과 반드시 짝을 이뤄야 한다.
 * 호출자: freeze 를 사용했던 cftype->write 핸들러.
 * 피호출: blkg_conf_exit(), blk_mq_unfreeze_queue().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   cftype->write 핸들러 -> [blkg_conf_exit_frozen] -> blk_mq_unfreeze_queue()
 */
void blkg_conf_exit_frozen(struct blkg_conf_ctx *ctx, unsigned long memflags);

/**
 * bio_issue_as_root_blkg - see if this bio needs to be issued as root blkg
 * @bio: the target &bio
 *
 * Return: true if this bio needs to be submitted with the root blkg context.
 *
 * In order to avoid priority inversions we sometimes need to issue a bio as if
 * it were attached to the root blkg, and then backcharge to the actual owning
 * blkg.  The idea is we do bio_blkcg_css() to look up the actual context for
 * the bio and attach the appropriate blkg to the bio.  Then we call this helper
 * and if it is true run with the root blkg for that queue and then do any
 * backcharging to the originating cgroup once the io is complete.
 */
/*
 * [한국어]
 * bio_issue_as_root_blkg - 이 bio 를 root blkg 컨텍스트로 발행해야 하는지 판단
 *
 * @bio: 판단 대상 bio
 * @return: true = root blkg 로 발행해야 함, false = bio->bi_blkg 가 가리키는
 *   실제 소속 cgroup 으로 그대로 발행
 *
 * 메타데이터 갱신(REQ_META, 예: 파일시스템 저널/inode 쓰기)이나 스왑 IO
 * (REQ_SWAP)는 특정 사용자 cgroup 의 IO 로 취급해 스로틀되면, 그 cgroup 과
 * 무관한 시스템 전체의 무결성/성능에 악영향을 줄 수 있다(우선순위 역전).
 * 이를 피하기 위해 이런 bio 는 일단 root blkg 로 발행하고, 완료 후 실제
 * 소유 cgroup 에 비용을 역청구(backcharge)하는 2단계 전략을 쓰는데, 이
 * 함수는 그 전략을 적용해야 하는 bio 인지를 비트 검사 한 줄로 판단한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서도 호출 가능(순수 비트 연산, side-effect 없음).
 * 호출자: bio_associate_blkg()/blk_cgroup_mergeable() 등 bio 의 cgroup 컨텍스트를
 * 다루는 코드.
 * 피호출: 없음(비트 마스크 연산).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   bio_associate_blkg() / blk_cgroup_mergeable() -> [bio_issue_as_root_blkg]
 */
static inline bool bio_issue_as_root_blkg(struct bio *bio)
{
	return (bio->bi_opf & (REQ_META | REQ_SWAP)) != 0;
	/* [한국어] bi_opf 에 REQ_META(메타데이터) 또는 REQ_SWAP(스왑) 비트가 하나라도
	 * 서 있으면 true. 두 플래그 중 하나만 서 있어도 root 발행 대상이 되도록
	 * OR 비트마스크로 검사(AND 가 아님에 유의 - REQ_META | REQ_SWAP 로 마스킹한
	 * 뒤 0 이 아닌지만 확인하므로 "둘 중 하나라도"의 의미). */
}

/**
 * blkg_lookup - lookup blkg for the specified blkcg - q pair
 * @blkcg: blkcg of interest
 * @q: request_queue of interest
 *
 * Lookup blkg for the @blkcg - @q pair.
 *
 * Must be called in a RCU critical section.
 */
/*
 * [한국어]
 * blkg_lookup - (blkcg, request_queue) 쌍에 대한 blkg 를 RCU 하에서 조회
 *
 * @blkcg: 조회할 cgroup
 * @q: 조회할 request_queue
 * @return: 해당 쌍에 대한 blkg 포인터, 존재하지 않으면 NULL(호출자가 보통
 *   root_blkg 로 fallback 하거나 blkg_conf_prep() 처럼 새로 생성)
 *
 * hint 캐시(blkcg->blkg_hint) -> radix tree(blkcg->blkg_tree) 순으로
 * 2단계 조회를 수행하는 hot path 함수. bio 하나가 제출될 때마다 호출될 수
 * 있으므로 락을 거의 쓰지 않고 RCU 와 hint 캐시로 비용을 최소화한다.
 * 실행 컨텍스트: RCU read-side critical section 안에서 호출해야 한다(원문
 * 주석 "Must be called in a RCU critical section" - blkg 가 이 함수 도중
 * 해제되지 않도록 보장하는 최소한의 전제 조건).
 * 호출자: bio_associate_blkg()/blkg_lookup_create() 등 IO 제출 경로,
 * blkg_for_each_descendant_pre/post 매크로.
 * 피호출: rcu_dereference_check(), radix_tree_lookup().
 * 에러 경로: 해당 (blkcg, q) 조합의 blkg 가 아직 생성되지 않았으면 NULL -
 * 호출자가 필요하면 blkg_create() 로 새로 만들거나 root_blkg 를 대신 사용.
 *
 * 호출 체인:
 *   bio_associate_blkg() -> [blkg_lookup] -> (없으면) blkg_create()
 */
static inline struct blkcg_gq *blkg_lookup(struct blkcg *blkcg,
					   struct request_queue *q)
{
	struct blkcg_gq *blkg;
	/* [한국어] 조회 결과를 담을 지역 변수 - 각 단계마다 갱신되며 최종 반환값이 된다. */

	if (blkcg == &blkcg_root)
		/* [한국어] root cgroup 은 항상 큐 생성 시 미리 만들어 둔 q->root_blkg 를
		 * 가지므로, radix tree/hint 조회를 거칠 필요 없이 즉시 반환 - 가장 흔한
		 * "cgroup 미분류 IO" 경로에 대한 hot path 최적화. */
		return q->root_blkg;

	/* hint 캐시를 먼저 확인: lockdep 검증 하에 RCU read로 접근 */
	blkg = rcu_dereference_check(blkcg->blkg_hint,
				lockdep_is_held(&q->queue_lock));
	/* [한국어] blkg_hint 를 RCU 로 읽는다 - lockdep_is_held(q->queue_lock) 를
	 * 넘겨 "queue_lock 을 들고 있다면 그것도 유효한 접근 근거로 인정"하도록
	 * rcu_dereference_check() 에 알려준다(RCU read-side 밖에서 queue_lock 을
	 * 든 채 호출하는 경로도 정당함을 lockdep 오탐 없이 표현). */
	if (blkg && blkg->q == q)
		/* [한국어] hint 가 가리키는 blkg 가 실제로 이 q 소속인지 재검증한 뒤
		 * 일치하면 캐시 히트로 즉시 반환 - radix tree 조회를 건너뛰어 조회
		 * 비용을 O(1) 로 낮춘다. */
		return blkg;

	/* hint 실패 시 radix tree에서 request_queue id로 정확히 검색 */
	blkg = radix_tree_lookup(&blkcg->blkg_tree, q->id);
	/* [한국어] hint 미스 시 q->id(request_queue 고유 ID)를 키로 radix tree 를
	 * 조회 - RCU-safe 자료구조이므로 RCU read-side 안에서 락 없이 안전. */
	if (blkg && blkg->q != q)
		/* [한국어] radix tree 는 q->id 를 키로 쓰는데, id 는 큐가 사라지면
		 * 재사용될 수 있어 이론상 "다른 큐가 같은 id 를 재사용했지만 이
		 * blkg 는 옛 큐의 것" 같은 불일치가 있을 수 있다(방어적 재검증). */
		blkg = NULL;
	return blkg;
	/* [한국어] 여기서 NULL 이 반환되면 호출자는 보통 "아직 이 cgroup 에
	 * 대한 blkg 가 없다"로 해석해 root_blkg 로 spill 하거나 blkg_create() 로
	 * 새로 만든다. */
}

/**
 * blkg_to_pd - get policy private data
 * @blkg: blkg of interest
 * @pol: policy of interest
 *
 * Return pointer to private data associated with the @blkg-@pol pair.
 */
/*
 * [한국어]
 * blkg_to_pd - blkg 에서 특정 정책의 사설(private) 데이터를 얻는다
 *
 * @blkg: 조회 대상 blkg(NULL 허용)
 * @pol: 조회할 정책(pol->plid 가 blkg->pd[] 의 인덱스로 쓰임)
 * @return: blkg->pd[pol->plid], blkg 가 NULL 이면 NULL
 *
 * 정책 구현체(throtl_grp 등은 blkg_policy_data 를 embed 하고 있음)가 자신의
 * 상태에 접근하는 표준 진입점. blkg 가 NULL 인 경우를 허용해 호출자가 매번
 * NULL 체크를 반복하지 않고 이 함수 하나로 위임할 수 있게 한다.
 * 호출자: 각 정책의 IO 판단 로직(예: throtl_charge_bio 가 blkg_to_pd(blkg,
 * &blkcg_policy_throtl) 로 throtl_grp 를 얻음), blkcg_activate_policy() 등.
 * 피호출: 없음(배열 인덱싱).
 * 에러 경로: blkg 가 NULL 이면 NULL 반환 - 호출자는 이를 "정책 미적용"으로 처리.
 *
 * 호출 체인:
 *   정책의 bio 처리 콜백 -> [blkg_to_pd] -> pd_to_blkg()(역방향 필요 시)
 */
static inline struct blkg_policy_data *blkg_to_pd(struct blkcg_gq *blkg,
						  struct blkcg_policy *pol)
{
	return blkg ? blkg->pd[pol->plid] : NULL;
	/* [한국어] blkg 가 NULL 이 아니면 pd[] 배열에서 이 정책의 슬롯을 반환;
	 * NULL 이면 호출자가 정책 적용 없이 진행하도록 NULL 전달. */
}

/*
 * [한국어]
 * blkcg_to_cpd - cgroup 전용(device 독립적) 정책 데이터를 조회
 *
 * @blkcg: 조회 대상 cgroup(NULL 허용)
 * @pol: 조회할 정책
 * @return: blkcg->cpd[pol->plid], blkcg 가 NULL 이면 NULL
 *
 * blkg_to_pd() 의 cgroup 레벨 버전. blk-ioprio 처럼 device(request_queue)
 * 단위 상태 없이 cgroup 전체 기본값만 갖는 정책이 이 함수로 자신의 cpd 를 얻는다.
 * 호출자: 각 정책의 cgroup 레벨 로직(예: ioprio_get_prio_policy()).
 * 피호출: 없음(배열 인덱싱).
 * 에러 경로: blkcg 가 NULL 이면 NULL - 호출자가 root/미지정으로 처리.
 *
 * 호출 체인:
 *   정책의 cgroup 레벨 콜백 -> [blkcg_to_cpd] -> cpd_to_blkcg()(역방향 필요 시)
 */
static inline struct blkcg_policy_data *blkcg_to_cpd(struct blkcg *blkcg,
						     struct blkcg_policy *pol)
{
	return blkcg ? blkcg->cpd[pol->plid] : NULL;
	/* [한국어] blkcg 가 NULL 이 아니면 cpd[] 배열에서 이 정책의 슬롯 반환. */
}

/**
 * pd_to_blkg - get blkg associated with policy private data
 * @pd: policy private data of interest
 *
 * @pd is policy private data.  Determine the blkg it's associated with.
 */
/*
 * [한국어]
 * pd_to_blkg - 정책 사설 데이터로부터 역으로 소속 blkg 를 얻는다
 *
 * @pd: 역참조할 blkg_policy_data(NULL 허용)
 * @return: pd->blkg, pd 가 NULL 이면 NULL
 *
 * blkg_to_pd() 의 반대 방향 변환. 정책 구현체가 pd 포인터만 들고 있다가
 * "이 pd 가 어느 (cgroup, request_queue) 소속인지" 알아야 할 때 사용한다
 * (예: pd_offline_fn 이 blkg->q 를 참조해 큐 관련 자원을 정리).
 * 호출자: 각 정책의 pd_offline_fn/pd_stat_fn 등 pd 만 인자로 받는 콜백들.
 * 피호출: 없음(구조체 필드 접근).
 * 에러 경로: pd 가 NULL 이면 NULL 반환.
 *
 * 호출 체인:
 *   정책의 pd_*_fn 콜백 -> [pd_to_blkg] -> blkg->q / blkg->blkcg 등 접근
 */
static inline struct blkcg_gq *pd_to_blkg(struct blkg_policy_data *pd)
{
	return pd ? pd->blkg : NULL;
	/* [한국어] pd 가 NULL 이 아니면 소속 blkg 반환. */
}

/*
 * [한국어]
 * cpd_to_blkcg - per-cgroup 정책 데이터로부터 역으로 소속 blkcg 를 얻는다
 *
 * @cpd: 역참조할 blkcg_policy_data(NULL 허용)
 * @return: cpd->blkcg, cpd 가 NULL 이면 NULL
 *
 * blkcg_to_cpd() 의 반대 방향 변환. cpd_to_blkg 와 대칭을 이루는 cgroup
 * 레벨 helper.
 * 호출자: cpd 만 인자로 받는 콜백(cpd_free_fn 구현체 등)이 소속 cgroup 을
 * 되짚어야 할 때.
 * 피호출: 없음.
 * 에러 경로: cpd 가 NULL 이면 NULL.
 *
 * 호출 체인:
 *   정책의 cpd_*_fn 콜백 -> [cpd_to_blkcg] -> blkcg 필드 접근
 */
static inline struct blkcg *cpd_to_blkcg(struct blkcg_policy_data *cpd)
{
	return cpd ? cpd->blkcg : NULL;
}

/**
 * blkg_get - get a blkg reference
 * @blkg: blkg to get
 *
 * The caller should be holding an existing reference.
 */
/*
 * [한국어]
 * blkg_get - blkg 의 percpu_ref 참조 카운트를 증가
 *
 * @blkg: 참조를 늘릴 대상(호출자가 이미 유효한 참조를 하나 들고 있어야 함
 *   - 원문 주석 "The caller should be holding an existing reference" 참고,
 *   즉 이 함수는 "0 에서 1 로" 만드는 최초 획득용이 아니다)
 * @return: 없음(void)
 *
 * bio 가 blkg 를 참조하는 동안 그 blkg 가 먼저 해제되지 않도록 보장하는
 * 가장 기본적인 참조 증가 연산. percpu_ref 를 쓰므로 대부분의 경우 CPU
 * 로컬 카운터만 건드리는 매우 빠른 fastpath 로 동작한다.
 * 호출자: bio_associate_blkg()(bio 가 blkg 를 참조로 붙잡을 때), blkg_create()
 * (자식이 부모 blkg 를 참조할 때).
 * 피호출: percpu_ref_get().
 * 에러 경로: 없음(항상 성공 - 이미 유효한 참조가 있다는 전제 하에 실패할
 * 수 없는 연산).
 *
 * 호출 체인:
 *   bio_associate_blkg() -> [blkg_get] -> (짝) blkg_put()
 */
static inline void blkg_get(struct blkcg_gq *blkg)
{
	percpu_ref_get(&blkg->refcnt);
	/* [한국어] percpu_ref_get() 은 현재 CPU 의 로컬 카운터만 증가시키는
	 * lock-free 연산 - 여러 CPU 가 동시에 같은 blkg 를 get 해도 cache-line
	 * 경합 없이 각자의 percpu 슬롯만 건드린다. */
}

/**
 * blkg_tryget - try and get a blkg reference
 * @blkg: blkg to get
 *
 * This is for use when doing an RCU lookup of the blkg.  We may be in the midst
 * of freeing this blkg, so we can only use it if the refcnt is not zero.
 */
/*
 * [한국어]
 * blkg_tryget - RCU lookup 결과에 대해 "죽어가는 중이 아닐 때만" 참조를 획득
 *
 * @blkg: 참조를 시도할 대상(NULL 허용)
 * @return: true = 참조 획득 성공(사용 가능), false = blkg 가 NULL 이거나
 *   이미 percpu_ref_kill() 이 호출되어 해제 절차가 시작된 상태
 *
 * blkg_get() 과 달리 "이미 유효한 참조를 갖고 있다"는 전제가 없는 상황 -
 * 즉 RCU read-side 에서 방금 lookup 한 blkg 가 바로 그 순간 다른 스레드에
 * 의해 destroy 절차(percpu_ref_kill)를 밟고 있을 수 있는 경우에 사용한다.
 * percpu_ref_tryget() 이 원자적으로 "0이 아니면 증가, 0이면 실패"를 보장하므로
 * TOCTOU(검사-사용 사이 경쟁) 없이 안전하게 판단할 수 있다.
 * 호출자: RCU read-side 에서 blkg_lookup() 결과를 쓰려는 코드(예: NVMe
 * 완료 인터럽트 경로에서 이미 삭제 중일 수 있는 blkg 에 안전하게 접근).
 * 피호출: percpu_ref_tryget().
 * 에러 경로: false 반환 시 호출자는 보통 상위(부모) blkg 나 root_blkg 로 대체.
 *
 * 호출 체인:
 *   RCU read-side lookup 코드 -> [blkg_tryget] -> (성공 시) 사용 후 blkg_put()
 */
static inline bool blkg_tryget(struct blkcg_gq *blkg)
{
	return blkg && percpu_ref_tryget(&blkg->refcnt);
	/* [한국어] blkg 자체가 NULL 이면 즉시 false(단락 평가로 percpu_ref_tryget
	 * 호출을 피함); NULL 이 아니면 percpu_ref_tryget() 의 원자적 판정 결과를 그대로 반환. */
}

/**
 * blkg_put - put a blkg reference
 * @blkg: blkg to put
 */
/*
 * [한국어]
 * blkg_put - blkg 참조를 하나 반납
 *
 * @blkg: 참조를 반납할 대상
 * @return: 없음(void)
 *
 * blkg_get()/blkg_tryget() 으로 얻은 참조를 반납한다. 마지막 참조였다면
 * percpu_ref 의 release 콜백(blkg_release())이 실행되어 call_rcu() 로
 * RCU 유예 후 최종적으로 blkg_free_workfn() 이 메모리를 회수한다.
 * 실행 컨텍스트: 인터럽트/소프트IRQ 컨텍스트를 포함해 어디서든 호출
 * 가능(percpu_ref_put() 자체가 그렇게 설계됨). 실제 회수(kfree)는 release
 * 콜백이 예약하는 workqueue 컨텍스트로 미뤄지므로 이 함수 자체는 sleep 하지 않는다.
 * 호출자: bio 완료 처리 경로, blkg_destroy()(생성 시 잡은 초기 참조 반납은
 * percpu_ref_kill() 로 별도 처리).
 * 피호출: percpu_ref_put().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   bio 완료 처리 -> [blkg_put] -> (마지막 참조라면) blkg_release() -> call_rcu()
 */
static inline void blkg_put(struct blkcg_gq *blkg)
{
	percpu_ref_put(&blkg->refcnt);
	/* [한국어] percpu_ref_put() 이 CPU 로컬 카운터를 감소시키고, 전역 합이
	 * 0 에 도달했다고 판단되면 등록된 release 콜백(blkg_release)을 실행한다. */
}

/**
 * blkg_for_each_descendant_pre - pre-order walk of a blkg's descendants
 * @d_blkg: loop cursor pointing to the current descendant
 * @pos_css: used for iteration
 * @p_blkg: target blkg to walk descendants of
 *
 * Walk @c_blkg through the descendants of @p_blkg.  Must be used with RCU
 * read locked.  If called under either blkcg or queue lock, the iteration
 * is guaranteed to include all and only online blkgs.  The caller may
 * update @pos_css by calling css_rightmost_descendant() to skip subtree.
 * @p_blkg is included in the iteration and the first node to be visited.
 */
/*
 * [한국어]
 * blkg_for_each_descendant_pre(d_blkg, pos_css, p_blkg) - pre-order(부모 먼저)
 * 로 @p_blkg 의 모든 후손 blkg 를 순회하는 for-문 스타일 매크로.
 *
 * @d_blkg: (출력) 매 반복마다 현재 후손 blkg 를 가리키는 루프 커서 변수명
 * @pos_css: (내부용) css_for_each_descendant_pre() 가 순회 상태를 유지하는
 *   cgroup_subsys_state* 커서 변수명 - 호출자가 미리 선언해 둬야 한다
 * @p_blkg: 순회를 시작할 조상(자기 자신 포함) blkg
 *
 * cgroup css 트리를 css_for_each_descendant_pre() 로 pre-order 순회하면서,
 * 각 css 를 blkg_lookup(css_to_blkcg(pos_css), p_blkg->q) 로 "같은
 * request_queue 에 대한 blkg" 로 변환한다. 아직 그 하위 cgroup 에 대한
 * blkg 가 생성되지 않았으면(lookup 실패) if 문이 거짓이 되어 그 반복은
 * 본문을 건너뛴다(순회는 계속됨). @p_blkg 자기 자신도 이터레이션에 포함되며
 * 가장 먼저 방문된다.
 * 실행 컨텍스트: RCU read-side critical section 안에서 사용해야 한다.
 * blkcg->lock 또는 q->queue_lock 을 추가로 들고 있으면 "online 인 blkg 만
 * 정확히 전부" 순회함이 보장된다(원문 주석).
 * 사용 예: blk-throttle 이 부모 cgroup 의 설정 변경을 하위 cgroup들에
 * 전파(inherit)할 때 부모부터 자식 순으로 처리해야 하므로 pre-order 를 쓴다.
 * 부분 트리를 건너뛰려면 호출자가 루프 본문에서 pos_css 를
 * css_rightmost_descendant() 로 갱신할 수 있다.
 */
#define blkg_for_each_descendant_pre(d_blkg, pos_css, p_blkg)		\
	css_for_each_descendant_pre((pos_css), &(p_blkg)->blkcg->css)	\
		if (((d_blkg) = blkg_lookup(css_to_blkcg(pos_css),	\
					    (p_blkg)->q)))

/**
 * blkg_for_each_descendant_post - post-order walk of a blkg's descendants
 * @d_blkg: loop cursor pointing to the current descendant
 * @pos_css: used for iteration
 * @p_blkg: target blkg to walk descendants of
 *
 * Similar to blkg_for_each_descendant_pre() but performs post-order
 * traversal instead.  Synchronization rules are the same.  @p_blkg is
 * included in the iteration and the last node to be visited.
 */
/*
 * [한국어]
 * blkg_for_each_descendant_post(d_blkg, pos_css, p_blkg) - post-order(자식
 * 먼저) 로 @p_blkg 의 모든 후손 blkg 를 순회하는 매크로.
 *
 * @d_blkg / @pos_css / @p_blkg: blkg_for_each_descendant_pre() 와 동일한 의미.
 *
 * 순회 순서만 css_for_each_descendant_post() 로 바뀌어 자식이 부모보다
 * 먼저 방문되고, @p_blkg 자신은 마지막에 방문된다. 동기화 규칙은 pre-order
 * 버전과 동일(RCU read-side 필수, blkcg/queue lock 추가 시 online blkg 만
 * 정확히 포함).
 * 사용 예: __blkcg_rstat_flush() 류가 자식의 통계를 먼저 확정한 뒤 그 합을
 * 부모에 반영해야 하는 "상향 집계(bottom-up aggregation)" 로직에 적합하다.
 */
#define blkg_for_each_descendant_post(d_blkg, pos_css, p_blkg)		\
	css_for_each_descendant_post((pos_css), &(p_blkg)->blkcg->css)	\
		if (((d_blkg) = blkg_lookup(css_to_blkcg(pos_css),	\
					    (p_blkg)->q)))

/*
 * [한국어]
 * blkcg_use_delay - 이 blkg 에 대한 지연(delay) 사용자 수를 하나 증가
 *
 * @blkg: 대상 blkg
 * @return: 없음(void)
 *
 * blk-throttle/BFQ 같은 정책이 "이 blkg 에 지금부터 지연을 적용한다"고
 * 선언할 때 호출한다. 여러 정책/경로가 동시에 지연을 걸 수 있으므로 단순
 * on/off 가 아니라 참조 카운트로 관리하며, 0->1 로 바뀌는 순간(첫 지연자)
 * 에만 cgroup 전체의 congestion_count 를 +1 해 "이 cgroup 계층에 혼잡이
 * 있다"는 신호를 blk_cgroup_congested() 가 관측할 수 있게 한다.
 * 실행 컨텍스트: 여러 CPU 에서 동시 호출 가능(atomic 연산으로 보호).
 * 호출자: blkcg_add_delay()(추정 - IO 지연 누적 시작 시점), 정책의 스로틀 판단 로직.
 * 피호출: atomic_read(), atomic_add_return(), atomic_inc().
 * 에러 경로: use_delay 가 이미 음수(blkcg_set_delay() 의 고정 지연 모드와
 * 혼용된 버그 상황)면 WARN_ON_ONCE 로 경고만 하고 아무 것도 하지 않는다
 * ([un]use_delay 와 set_delay/clear_delay 는 서로 배타적으로 설계됨).
 *
 * 호출 체인:
 *   blkcg_add_delay() -> [blkcg_use_delay] -> (짝) blkcg_unuse_delay()
 */
static inline void blkcg_use_delay(struct blkcg_gq *blkg)
{
	if (WARN_ON_ONCE(atomic_read(&blkg->use_delay) < 0))
		/* [한국어] use_delay 가 음수라는 것은 blkcg_set_delay() 의 "고정 지연
		 * 모드"(-1)와 뒤섞여 호출되었다는 뜻 - 두 메커니즘은 혼용 금지이므로
		 * 버그로 간주해 경고만 남기고 즉시 반환(상태를 더 어지럽히지 않음). */
		return;
	/* 첫 번째 지연자가 congestion_count를 증가시킴 */
	if (atomic_add_return(1, &blkg->use_delay) == 1)
		/* [한국어] atomic_add_return() 은 "증가시킨 후의 값"을 반환하므로,
		 * 반환값이 1 이라는 것은 0->1 전이, 즉 내가 첫 번째로 지연을 건
		 * 주체라는 뜻 - 이때만 congestion_count 를 올려 중복 계상을 막는다. */
		atomic_inc(&blkg->blkcg->congestion_count);
}

/*
 * [한국어]
 * blkcg_unuse_delay - blkg 지연 사용자 수를 하나 감소
 *
 * @blkg: 대상 blkg
 * @return: 1 = 실제로 감소를 수행함(사용자였음), 0 = 원래 지연 사용자가
 *   아니었거나(use_delay==0) 버그 상태(use_delay<0)라 아무 것도 안 함
 *
 * blkcg_use_delay() 의 짝 함수. 여러 CPU 가 동시에 use/unuse 를 호출할 수
 * 있는 경쟁 상황에서 안전하게 "마지막 사용자였는지"를 판별하기 위해 단순
 * atomic_dec 대신 CAS(compare-and-swap) 루프를 사용한다.
 * 실행 컨텍스트: 여러 CPU 동시 호출 가능.
 * 호출자: 정책이 스로틀을 해제할 때, blkcg_scale_delay() 가 간접적으로
 * 참조하는 로직(추정), cgroup 오프라인 정리 경로.
 * 피호출: atomic_read(), atomic_try_cmpxchg(), atomic_dec().
 * 에러 경로: use_delay 가 이미 음수(고정 지연 모드)면 WARN_ON_ONCE 후 0 반환;
 * 이미 0 이면 아무 것도 안 하고 0 반환(중복 unuse 방어).
 *
 * 호출 체인:
 *   (blkcg_use_delay() 의 짝) -> [blkcg_unuse_delay]
 */
static inline int blkcg_unuse_delay(struct blkcg_gq *blkg)
{
	int old = atomic_read(&blkg->use_delay);
	/* [한국어] CAS 루프의 시작점이 될 현재 값 스냅샷 - 이후 다른 CPU 가
	 * 동시에 값을 바꿀 수 있으므로 "낙관적 재시도"의 기준값일 뿐이다. */

	if (WARN_ON_ONCE(old < 0))
		return 0;
		/* [한국어] blkcg_use_delay() 와 동일하게, 고정 지연 모드(-1)와의
		 * 혼용을 버그로 간주해 경고 후 무시. */
	if (old == 0)
		return 0;
		/* [한국어] 애초에 지연 사용자가 없었다면(0) 감소시킬 것도 없으므로
		 * 그대로 0 을 반환해 "아무 일도 하지 않았음"을 호출자에게 알린다. */

	/*
	 * We do this song and dance because we can race with somebody else
	 * adding or removing delay.  If we just did an atomic_dec we'd end up
	 * negative and we'd already be in trouble.  We need to subtract 1 and
	 * then check to see if we were the last delay so we can drop the
	 * congestion count on the cgroup.
	 */
	/* atomic_dec 음수 방지를 위해 CAS 루프로 1 감소 */
	while (old && !atomic_try_cmpxchg(&blkg->use_delay, &old, old - 1))
		;
		/* [한국어] atomic_try_cmpxchg() 는 실패 시 old 를 "실제 현재 값"으로
		 * 자동 갱신해 주므로, 다른 CPU 가 그 사이 use_delay 를 바꿨더라도
		 * 다음 반복에서 최신 값 기준으로 다시 시도한다(락 없는 낙관적 재시도
		 * - 단순 atomic_dec() 을 썼다면 동시 호출 시 음수로 내려갈 위험이 있음). */

	if (old == 0)
		return 0;
		/* [한국어] 루프를 도는 동안 다른 CPU 가 먼저 0 으로 만들어 버렸다면
		 * (while 조건의 old 검사로 루프가 빠져나온 경우) 나는 더 이상 할
		 * 일이 없으므로 0 반환. */
	/* 마지막 지연자가 congestion_count 감소 -> NVMe I/O 발행 재개 신호 */
	if (old == 1)
		/* [한국어] CAS 직전 값(old)이 1 이었다는 것은 "내가 감소시켜 0 으로
		 * 만든 마지막 사용자"라는 뜻 - 이때만 congestion_count 를 내려
		 * blk_cgroup_congested() 가 더 이상 혼잡으로 보지 않게 한다. */
		atomic_dec(&blkg->blkcg->congestion_count);
	return 1;
	/* [한국어] 실제로 카운트를 감소시켰음을 호출자에게 알림(반환값 자체를
	 * 적극적으로 활용하는 호출자는 현재 없을 수 있으나 API 계약상 의미 있는 값). */
}

/**
 * blkcg_set_delay - Enable allocator delay mechanism with the specified delay amount
 * @blkg: target blkg
 * @delay: delay duration in nsecs
 *
 * When enabled with this function, the delay is not decayed and must be
 * explicitly cleared with blkcg_clear_delay(). Must not be mixed with
 * blkcg_[un]use_delay() and blkcg_add_delay() usages.
 */
/*
 * [한국어]
 * blkcg_set_delay - blkg 에 decay 되지 않는 고정 지연 시간을 설정
 *
 * @blkg: 대상 blkg
 * @delay: 설정할 지연 시간(나노초)
 * @return: 없음(void)
 *
 * blkcg_use_delay()/blkcg_add_delay() 가 만드는 "점진적으로 decay 되는"
 * 지연과 달리, 이 함수는 use_delay 를 특수값 -1 로 고정해 blkcg_scale_delay()
 * 가 절대 이 값을 깎지 않게 만든다. 명시적으로 blkcg_clear_delay() 를
 * 호출해야만 해제되며, [un]use_delay()/add_delay() 와 혼용하면 안 된다
 * (원문 kernel-doc 의 "Must not be mixed with..." 경고 참고).
 * 실행 컨텍스트: 여러 CPU 동시 호출 가능하나 congestion_count 증가는
 * CAS 로 단 한 번만 일어나도록 보호.
 * 호출자: 진단/디버그 목적이나 관리자가 강제로 IO 를 멈춰야 하는 특수
 * 상황(추정 - 예: cgroup OOM kill 전 유예 등).
 * 피호출: atomic_read(), atomic_try_cmpxchg(), atomic_inc(), atomic64_set().
 * 에러 경로: 없음(멱등적으로 동작 - 이미 -1 이면 congestion_count 재증가 없이
 * delay_nsec 값만 갱신).
 *
 * 호출 체인:
 *   (호출자) -> [blkcg_set_delay] -> (짝) blkcg_clear_delay()
 */
static inline void blkcg_set_delay(struct blkcg_gq *blkg, u64 delay)
{
	int old = atomic_read(&blkg->use_delay);
	/* [한국어] 현재 use_delay 값을 읽어 "아직 고정 지연 모드가 아닌지"(0)
	 * 확인하는 CAS 의 기대값(expected)으로 사용. */

	/* We only want 1 person setting the congestion count for this blkg. */
	/* 한 명만 congestion_count를 설정하도록 CAS */
	if (!old && atomic_try_cmpxchg(&blkg->use_delay, &old, -1))
		/* [한국어] old 가 0 일 때만(아직 아무도 지연을 걸지 않았을 때만)
		 * CAS 로 -1 을 기록 시도 - 성공한 단 하나의 호출자만 congestion_count
		 * 를 올려 중복 증가를 막는다(여러 CPU 가 동시에 set_delay 를 불러도
		 * 카운트가 한 번만 오르도록). */
		atomic_inc(&blkg->blkcg->congestion_count);

	atomic64_set(&blkg->delay_nsec, delay);
	/* [한국어] 실제 지연 시간(나노초)을 무조건 최신 값으로 덮어쓴다 - CAS
	 * 성공 여부와 무관하게 항상 실행되므로, 이미 고정 지연 모드였던 blkg 에
	 * 대해 지연 시간만 갱신(재설정)하는 것도 이 함수 하나로 가능하다. */
}

/**
 * blkcg_clear_delay - Disable allocator delay mechanism
 * @blkg: target blkg
 *
 * Disable use_delay mechanism. See blkcg_set_delay().
 */
/*
 * [한국어]
 * blkcg_clear_delay - blkcg_set_delay() 로 설정된 고정 지연 모드를 해제
 *
 * @blkg: 대상 blkg
 * @return: 없음(void)
 *
 * use_delay 를 0 으로 되돌려 blkcg_set_delay() 이전 상태로 복원한다. 이
 * 함수는 오직 set_delay 로 진입한 고정 지연 모드(-1)를 벗어나기 위한
 * 용도이며, [un]use_delay() 의 참조 카운트 감소와는 별개의 경로다.
 * 실행 컨텍스트: 여러 CPU 동시 호출 가능.
 * 호출자: blkcg_set_delay() 를 호출했던 것과 동일한 상위 로직이 지연을
 * 풀어야 할 때.
 * 피호출: atomic_read(), atomic_try_cmpxchg(), atomic_dec().
 * 에러 경로: 이미 use_delay 가 0(고정 지연 모드가 아님)이면 아무 것도 하지
 * 않음(멱등적).
 *
 * 호출 체인:
 *   (호출자) -> [blkcg_clear_delay] (blkcg_set_delay() 의 짝)
 */
static inline void blkcg_clear_delay(struct blkcg_gq *blkg)
{
	int old = atomic_read(&blkg->use_delay);
	/* [한국어] 현재 값이 -1(고정지연 모드)인지 확인하기 위한 CAS 기대값. */

	/* We only want 1 person clearing the congestion count for this blkg. */
	/* 한 명만 congestion_count를 해제하도록 CAS */
	if (old && atomic_try_cmpxchg(&blkg->use_delay, &old, 0))
		/* [한국어] old 가 0 이 아닐 때(즉 -1 고정 지연 모드일 때)만 CAS 로
		 * 0 으로 리셋 시도 - 성공한 호출자만 congestion_count 를 내려
		 * blkcg_set_delay() 가 올렸던 것과 정확히 짝을 맞춘다. */
		atomic_dec(&blkg->blkcg->congestion_count);
}

/**
 * blk_cgroup_mergeable - Determine whether to allow or disallow merges
 * @rq: request to merge into
 * @bio: bio to merge
 *
 * @bio and @rq should belong to the same cgroup and their issue_as_root should
 * match. The latter is necessary as we don't want to throttle e.g. a metadata
 * update because it happens to be next to a regular IO.
 */
/**
 * blk_cgroup_mergeable - Determine whether to allow or disallow merges
 * @rq: request to merge into
 * @bio: bio to merge
 *
 * @bio and @rq should belong to the same cgroup and their issue_as_root should
 * match. The latter is necessary as we don't want to throttle e.g. a metadata
 * update because it happens to be next to a regular IO.
 */
/*
 * [한국어]
 * blk_cgroup_mergeable - 두 IO(기존 request 와 새 bio)를 merge 해도 되는지 판단
 *
 * @rq: 병합 대상이 되는 기존 request
 * @bio: 새로 병합하려는 bio
 * @return: true = 병합 허용, false = 병합 금지
 *
 * blk-mq 의 merge 로직(예: bio 를 인접한 기존 request 뒤에 붙이는 back-merge)
 * 은 원래 물리적으로 인접한 LBA 범위만 확인하지만, cgroup 관점에서는 두
 * 가지가 더 맞아야 한다: (1) 같은 cgroup 소속이어야 통계/스로틀 회계가
 * 꼬이지 않고, (2) bio_issue_as_root_blkg() 판정이 같아야 한다 - 그렇지
 * 않으면 예컨대 메타데이터 bio(root 로 발행)가 일반 사용자 IO 뒤에
 * 병합되어 그 사용자 cgroup 의 스로틀 한도에 함께 걸려버리는 문제(원문
 * kernel-doc 의 예시)가 생긴다.
 * 실행 컨텍스트: blk-mq merge 판단 경로(보통 IRQ 비활성 스핀락 보호 하 또는
 * 태스크 컨텍스트).
 * 호출자: blk-mq 의 bio-to-request merge 판단 로직(elevator/블록 계층의 병합 시도).
 * 피호출: bio_issue_as_root_blkg().
 * 에러 경로: 없음(단순 판정 함수, false 반환 시 호출자는 새 request 를 생성).
 *
 * 호출 체인:
 *   blk-mq merge 시도 경로 -> [blk_cgroup_mergeable] -> (true 면) 기존 rq 에 병합
 */
static inline bool blk_cgroup_mergeable(struct request *rq, struct bio *bio)
{
	return rq->bio->bi_blkg == bio->bi_blkg &&
		/* [한국어] rq 에 실린 첫 bio 와 새 bio 가 같은 blkg(cgroup) 소속인지
		 * 비교 - 다르면 서로 다른 cgroup 의 IO 가 하나의 request 로 섞여
		 * 회계/스로틀이 잘못될 수 있으므로 병합 금지. */
		bio_issue_as_root_blkg(rq->bio) == bio_issue_as_root_blkg(bio);
		/* [한국어] 두 bio 의 "root 로 발행되어야 하는지" 판정이 같아야
		 * 병합 허용 - 메타데이터/스왑 bio 가 일반 bio 와 섞여 우선순위
		 * 역전 방지 로직이 무력화되는 것을 막는다. */
}

/*
 * [한국어]
 * blkcg_policy_enabled - 주어진 request_queue 에서 특정 정책이 활성화됐는지 확인
 *
 * @q: 확인할 request_queue
 * @pol: 확인할 정책(NULL 허용)
 * @return: true = 이 큐에서 정책이 활성화됨(blkg->pd[pol->plid] 참조 가능),
 *   false = pol 이 NULL 이거나 이 큐에서 비활성화 상태
 *
 * blkcg_activate_policy() 가 세팅하는 q->blkcg_pols 비트마스크를 검사하는
 * 단순 bit test. 정책 구현체가 IO 처리 중 pd[] 를 참조하기 전에 반드시
 * 이 함수로 활성화 여부를 먼저 확인해야 한다(비활성 상태에서 pd[] 를
 * 참조하면 NULL 또는 stale 포인터를 만질 위험이 있음).
 * 실행 컨텍스트: 어디서든 호출 가능(단순 비트 테스트, 락 불필요 - 단
 * blkcg_activate_policy() kernel-doc 의 "큐/blkcg 락을 든 채 확인 후
 * 참조하면 안전" 규약을 따르는 것이 정석).
 * 호출자: blkg_lookup() 계열 헬퍼가 아니라 정책 자신의 IO 판단 로직,
 * blkg_conf_prep(), blkcg_print_blkgs() 등 pd 를 참조하기 전 모든 지점.
 * 피호출: test_bit().
 * 에러 경로: 없음(단순 판정).
 *
 * 호출 체인:
 *   정책의 bio/merge 판단 로직 -> [blkcg_policy_enabled] -> (true 면) blkg_to_pd() 로 pd 참조
 */
static inline bool blkcg_policy_enabled(struct request_queue *q,
				const struct blkcg_policy *pol)
{
	return pol && test_bit(pol->plid, q->blkcg_pols);
	/* [한국어] pol 이 NULL 이면 단락 평가로 즉시 false; 아니면 q->blkcg_pols
	 * 비트마스크에서 pol->plid 번째 비트를 검사. */
}

/*
 * [한국어]
 * blk_cgroup_bio_start - bio 제출 시작 시 cgroup IO 통계를 누적
 *
 * @bio: 제출되는 bio(bio->bi_blkg 가 이미 설정되어 있어야 함)
 * @return: 없음(void)
 *
 * 실제 구현은 block/blk-cgroup.c 에 있으며(이 헤더에는 선언만), bio 가
 * 실제로 하위 장치로 내려가기 직전(submit_bio_noacct_nocheck() 시점)에
 * bio->bi_blkg 의 percpu iostat_cpu(read/write/discard bytes/ios)를
 * 갱신하고, 아직 flush 대기열(lhead)에 없다면 등록해 나중에
 * __blkcg_rstat_flush() 가 전역 통계로 합산할 수 있게 한다.
 * 실행 컨텍스트: 태스크 컨텍스트(submit_bio 호출자) - 다만 u64_stats_*_irqsave
 * 를 쓰므로 인터럽트 컨텍스트에서의 재진입도 안전하게 설계되어 있다.
 * 호출자: submit_bio_noacct_nocheck()[block/blk-core.c].
 * 피호출(구현 내부): blk_cgroup_io_type(), per_cpu_ptr(), u64_stats_update_*(),
 * llist_add(), css_rstat_updated().
 * 에러 경로: cgroup v1(legacy) 이거나 root cgroup 이면 아무 것도 하지 않고 반환
 * (v1 은 이 경로로 통계를 내지 않고, root 통계는 시스템 전체 disk_stats 로 대체).
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck() -> [blk_cgroup_bio_start] -> css_rstat_updated()
 */
void blk_cgroup_bio_start(struct bio *bio);

/*
 * [한국어]
 * blkcg_add_delay - blkg 에 delta 만큼의 IO 지연 예산을 누적
 *
 * @blkg: 대상 blkg
 * @now: 현재 시각(ktime, ns) - 호출자가 미리 얻어 넘김(중복 clock 조회 방지)
 * @delta: 이번에 추가로 누적할 지연 시간(나노초)
 * @return: 없음(void)
 *
 * blk-throttle/BFQ 등이 "이 cgroup 이 목표 대역폭/IOPS 를 초과했다"고
 * 판단했을 때 초과분을 지연 예산(delay_nsec)에 더하는 함수. 먼저
 * blkcg_scale_delay() 로 1초 넘게 지난 예산을 decay 시킨 뒤 delta 를
 * atomic 하게 더하므로, 지연 예산이 무한정 누적되지 않고 "최근 초과분"
 * 위주로 유지된다. 실제 소비(태스크를 재우는 것)는 이 함수가 아니라
 * blkcg_maybe_throttle_current()/blkcg_maybe_throttle_blkg()[block/
 * blk-cgroup.c] 가 유저스페이스 복귀 시점에 수행한다.
 * 실행 컨텍스트: 여러 CPU 동시 호출 가능(atomic64_add 로 보호).
 * 호출자: blk_cgroup_bio_start() 가 IO 시작 시 위임하는 스로틀 계산 경로(추정),
 * 각 정책의 한도 초과 판정 로직.
 * 피호출: blkcg_scale_delay(), atomic64_add().
 * 에러 경로: use_delay 가 이미 음수(set_delay 고정 모드)면 WARN_ON_ONCE 로
 * 경고 후 아무 것도 하지 않고 반환(혼용 방지).
 *
 * 호출 체인:
 *   정책의 한도 초과 판정 -> [blkcg_add_delay] -> blkcg_scale_delay()
 */
void blkcg_add_delay(struct blkcg_gq *blkg, u64 now, u64 delta);
#else	/* CONFIG_BLK_CGROUP */
/*
 * [한국어]
 * 이하는 CONFIG_BLK_CGROUP 이 꺼져 있을 때(cgroup IO 컨트롤러 자체를
 * 빌드하지 않는 커널 구성) 컴파일이 되도록 제공하는 "무장(no-op) 폴백"
 * 구현이다. 상단(#ifdef CONFIG_BLK_CGROUP 블록)의 실제 구조체/함수와 이름을
 * 맞춰, 이 헤더를 include 하는 다른 block layer 코드(blk-mq.c 등)가
 * #ifdef 분기 없이 동일한 심볼을 그대로 호출할 수 있게 한다. 모든 구조체는
 * 필드 없는 빈 구조체(크기만 존재)로, 모든 함수는 아무 일도 하지 않거나
 * 가장 무해한 기본값(성공/true/NULL)을 반환하도록 구현되어 "cgroup 이
 * 전혀 없는 것처럼" 동작한다.
 */

struct blkg_policy_data {
};
/* [한국어] CONFIG_BLK_CGROUP=n 일 때의 blkg_policy_data - 필드가 전혀 없는
 * 빈 구조체. 정책이 이 타입을 embed 하더라도 실질적인 per-blkg 상태를
 * 가질 수 없다(애초에 cgroup 자체가 없으므로 필요 없음). */

struct blkcg_policy_data {
};
/* [한국어] CONFIG_BLK_CGROUP=n 일 때의 blkcg_policy_data - 위와 동일한 이유로 빈 구조체. */

struct blkcg_policy {
};
/* [한국어] CONFIG_BLK_CGROUP=n 일 때의 blkcg_policy - 콜백 테이블 자체가
 * 의미를 갖지 못하므로(등록할 정책 프레임워크가 없음) 빈 구조체로 남긴다. */

struct blkcg {
};
/* [한국어] CONFIG_BLK_CGROUP=n 일 때의 blkcg - cgroup 컨트롤러가 없으므로
 * css/blkg_tree 등 실제 상태를 가질 필요가 없는 빈 구조체. */

static inline struct blkcg_gq *blkg_lookup(struct blkcg *blkcg, void *key) { return NULL; }
/* [한국어]
 * blkg_lookup (no-op 버전) - @blkcg: 사용 안 함, @key: 사용 안 함
 * @return: 항상 NULL(대응하는 blkg 가 없다는 뜻).
 * cgroup 자체가 없으므로 조회할 blkg 도 없다는 것을 표현. 호출자는 이
 * 결과를 받아 항상 "장치 전체" 기준(cgroup 미분류)으로 동작하게 된다.
 * 실제(CONFIG_BLK_CGROUP=y) 버전과 시그니처를 맞추기 위해 두 번째 인자
 * 타입만 void* 로 완화되어 있다(request_queue* 대신). */

static inline void blkg_init_queue(struct request_queue *q) { }
/* [한국어] blkg_init_queue (no-op) - 초기화할 blkg 관련 큐 필드 자체가
 * 없으므로 아무 것도 하지 않는다. */

static inline int blkcg_init_disk(struct gendisk *disk) { return 0; }
/* [한국어] blkcg_init_disk (no-op) - root blkg 를 만들 필요가 없으므로
 * 항상 성공(0)만 반환해 디스크 등록이 계속 진행되게 한다. */

static inline void blkcg_exit_disk(struct gendisk *disk) { }
/* [한국어] blkcg_exit_disk (no-op) - 정리할 blkg 상태가 없으므로 아무 것도 하지 않는다. */

static inline int blkcg_policy_register(struct blkcg_policy *pol) { return 0; }
/* [한국어] blkcg_policy_register (no-op) - 정책 프레임워크가 없으므로 등록
 * 자체가 무의미하지만, 정책 모듈의 init 경로가 실패로 취급되지 않도록
 * 항상 성공(0)을 반환한다. */

static inline void blkcg_policy_unregister(struct blkcg_policy *pol) { }
/* [한국어] blkcg_policy_unregister (no-op) - 등록된 것이 없으므로 해제할 것도 없다. */

static inline int blkcg_activate_policy(struct gendisk *disk,
					const struct blkcg_policy *pol) { return 0; }
/* [한국어] blkcg_activate_policy (no-op) - 활성화할 blkg->pd[] 자체가 없으므로
 * 항상 성공(0)만 반환해 디스크/정책 초기화 흐름이 계속 진행되게 한다. */

static inline void blkcg_deactivate_policy(struct gendisk *disk,
					   const struct blkcg_policy *pol) { }
/* [한국어] blkcg_deactivate_policy (no-op) - 정리할 pd 가 없으므로 아무 것도 하지 않는다. */

static inline struct blkg_policy_data *blkg_to_pd(struct blkcg_gq *blkg,
						  struct blkcg_policy *pol) { return NULL; }
/* [한국어] blkg_to_pd (no-op) - pd 자체가 존재할 수 없는 구성이므로 항상 NULL.
 * 호출자는 이를 "정책 미적용"으로 해석해 그냥 진행한다. */

static inline struct blkcg_gq *pd_to_blkg(struct blkg_policy_data *pd) { return NULL; }
/* [한국어] pd_to_blkg (no-op) - 대응 관계 자체가 없으므로 항상 NULL. */

static inline void blkg_get(struct blkcg_gq *blkg) { }
/* [한국어] blkg_get (no-op) - 참조 카운트를 관리할 blkg 실체가 없으므로 아무 것도 하지 않는다. */

static inline void blkg_put(struct blkcg_gq *blkg) { }
/* [한국어] blkg_put (no-op) - blkg_get() 과 짝을 맞추는 no-op. */

static inline void blk_cgroup_bio_start(struct bio *bio) { }
/* [한국어] blk_cgroup_bio_start (no-op) - 누적할 cgroup IO 통계 자체가
 * 없으므로 아무 것도 하지 않는다(시스템 전체 통계는 별도 경로에서 계속 집계됨). */

static inline bool blk_cgroup_mergeable(struct request *rq, struct bio *bio) { return true; }
/* [한국어] blk_cgroup_mergeable (no-op) - cgroup 이 없으니 "다른 cgroup 이라
 * 병합 금지" 같은 제약 자체가 성립하지 않아 항상 true(병합 허용)를 반환 -
 * 순수 물리적 인접성 기준의 일반 merge 로직만 남기고 cgroup 제약을 제거한다. */

#define blk_queue_for_each_rl(rl, q)	\
	for ((rl) = &(q)->root_rl; (rl); (rl) = NULL)
/*
 * [한국어]
 * blk_queue_for_each_rl(rl, q) - request_queue 의 request_list 를 순회하는
 * for-문 스타일 매크로(레거시 request-based(비 blk-mq) 경로에서 cgroup 별
 * request_list 를 순회하기 위해 원래 존재하던 매크로의 CONFIG_BLK_CGROUP=n
 * 폴백 버전).
 * @rl: (출력) 루프 커서 - request_list* 변수명
 * @q: 순회 대상 request_queue
 * cgroup 이 없으므로 q->root_rl(디폴트 request_list) 하나만 존재한다고
 * 가정해, 이 매크로는 사실상 "딱 한 번만 도는 for 문"으로 축약된다(두
 * 번째 반복에서 (rl) = NULL 로 종료 조건 충족). CONFIG_BLK_CGROUP=y 인
 * 실제 버전은 q 에 매달린 모든 cgroup 별 request_list 를 순회하지만,
 * 여기서는 그런 리스트 자체가 없기 때문에 단순화되어 있다. */

#endif	/* CONFIG_BLK_CGROUP */

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 blkcg <-> request_queue 연결 구조(blkcg_gq)와 정책 콜백을 정의하며,
 *   NVMe namespace로 향하는 I/O가 blk-mq를 거쳐 nvme_queue_rq()로 전달되기 전
 *   cgroup 소속/지연/합병 정보를 결정하는 기반이 됩니다.
 *
 * - blkcg_gq 는 per-cpu iostat, 지연 메커니즘(use_delay/delay_nsec),
 *   그리고 blk-throttle/BFQ 같은 정책 데이터(pd[])를 담아 NVMe I/O를
 *   계층적으로 제어합니다.
 *
 * - blkcg_use_delay()/blkcg_unuse_delay()/blkcg_set_delay()/blkcg_clear_delay()는
 *   atomic reference/counter를 이용해 cgroup 단위 congestion/backpressure를
 *   표현하며(congestion_count), blkcg_add_delay()/blkcg_scale_delay()[block/
 *   blk-cgroup.c]와 함께 IOPS/대역폭 초과 시 delay_nsec 을 누적·decay 시켜
 *   유저스페이스 복귀 시점에 태스크를 재우는 방식으로 request 발행을 늦춥니다.
 *
 * - blkg_lookup()은 hint + radix tree를 통해 IO 경로에서 빠른
 *   cgroup lookup을 제공하고, blkg_to_pd()/pd_to_blkg()는 정책별 상태를
 *   request와 연결합니다.
 *
 * - 상위 인터페이스인 include/linux/blk-cgroup.h, 구현체인 block/blk-cgroup.c,
 *   그리고 block/blk-mq.c, block/blk-throttle.c, block/bfq-iosched.c 등과
 *   함께 NVMe SSD의 end-to-end I/O 제어 스택을 구성합니다.
 */

#endif /* _BLK_CGROUP_PRIVATE_H */
