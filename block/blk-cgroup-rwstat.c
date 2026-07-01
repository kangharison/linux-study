/* SPDX-License-Identifier: GPL-2.0
 *
 * Legacy blkg rwstat helpers enabled by CONFIG_BLK_CGROUP_RWSTAT.
 * Do not use in new code.
 */

/*
 * blk-cgroup-rwstat.c: blk-cgroup 기반 READ/WRITE/DISCARD 통계 누적을 위한 레거시 헬퍼
 *
 * NVMe SSD 흐름에서 볼 때, 이 파일은 응용의 READ/WRITE/DISCARD 요청이
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로를 타기 전, blk-cgroup 계층에서 어느 cgroup이
 * 얼마나 많은 I/O를 생성했는지 bytes/요청 단위로 계수(accounting)하는 지점이다.
 * 누적된 값은 NVMe 컨트롤러의 SQ/CQ, CID, PRP/SGL 전송과는 독립적으로
 * cgroup v1/v2 인터페이스(예: blkio.throttle.io_service_bytes)를 통해
 * 사용자 공간에 노출되어 NVMe 장치별 I/O 사용량을 보여준다.
 * blk-cgroup.c, blk-throttle.c 등 상위 정책 코드 뒤에서 rwstat 초기화,
 * 출력, 계층적 집계를 담당한다.
 */
#include "blk-cgroup-rwstat.h"

/*
 * 주요 자료구조와 NVMe 동작 연결 (정의는 blk-cgroup-rwstat.h 참고)
 *
 * struct blkg_rwstat
 *   - cpu_cnt[BLKG_RWSTAT_NR]: per-CPU 카운터 배열. NVMe I/O가 blk-mq
 *     경로로 들어올 때 blkg_rwstat_add()를 통해 READ/WRITE/DISCARD/SYNC/
 *     ASYNC별로 누적된다. percpu_counter를 사용해 nvme_queue_rq 발행
 *     빈도가 높은 환경에서도 계수 오버헤드를 최소화한다.
 *   - aux_cnt[BLKG_RWSTAT_NR]: 하위 cgroup이 제거되거나 offline 될 때
 *     사라지지 않은 통계를 보존하는 보조 원자 카운터. NVMe 입장에서는
 *     과거에 발행되어 아직 완료되지 않은 SQ 엔트리의 잔여 I/O가 여기에
 *     반영될 수 있다(추정).
 *
 * struct blkg_rwstat_sample
 *   - cnt[BLKG_RWSTAT_NR]: blkg_rwstat_read() 등으로 읽어낸 READ/WRITE/
 *     DISCARD/SYNC/ASYNC 스냅샷. NVMe 컨트롤러에 도달하기 직전의
 *     bio/request 상태를 사용자 공간 보고용으로 정리한 값이다.
 *
 * enum blkg_rwstat_type
 *   - BLKG_RWSTAT_READ/WRITE/DISCARD: NVMe 명령의 opcode 방향과 대응한다.
 *   - BLKG_RWSTAT_SYNC/ASYNC: REQ_SYNC 등 플래그에 따라 나뉘며, NVMe
 *     submission 시 SQ 엔트리의 우선순위나 polling 방식과 연결될 수 있다(추정).
 */

/*
 * blkg_rwstat_init - struct blkg_rwstat의 per-CPU 카운터와 보조 카운터를 초기화한다.
 *
 * 호출 경로 (NVMe 연결 예시):
 *   blkcg_policy_register -> throtl_pd_init -> blkg_rwstat_init
 *   (추정) 이후 NVMe READ/WRITE/DISCARD 요청은
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   경로를 타기 전에 blkg_rwstat_add()로 여기에 기록된다.
 *
 * NVMe 연결점:
 *   - cpu_cnt 초기화가 실패하면 NVMe I/O 계수를 할 수 없어 정책 등록이
 *     실패할 수 있다(추정).
 *   - aux_cnt는 0으로 초기화하여 이전 cgroup의 잔여 통계가 누적되지
 *     않도록 한다.
 */
int blkg_rwstat_init(struct blkg_rwstat *rwstat, gfp_t gfp)
/* @rwstat: NVMe I/O가 흐를 blkcg_gq 정책 데이터 내 통계 블록, @gfp: 메모리 할당 플래그(고속 NVMe 경로에서 GFP_ATOMIC 아닌 일반 컨텍스트 사용 가정) */
{
	int i, ret;
	/* i: BLKG_RWSTAT_NR(5개: READ/WRITE/SYNC/ASYNC/DISCARD) 루프 인덱스, ret: percpu_counter 할당 성공/실패(ENOMEM 등) */

	/* per-CPU 카운터 BLKG_RWSTAT_NR개(READ/WRITE/SYNC/ASYNC/DISCARD) 할당 */
	ret = percpu_counter_init_many(rwstat->cpu_cnt, 0, gfp, BLKG_RWSTAT_NR);
	/* cpu_cnt[5]는 이후 nvme_queue_rq 발행 빈도에 맞춰 per-CPU 핫패스에서 blkg_rwstat_add()가 atomic 연산 없이 누적할 수 있는 카운터 배열(추정) */
	if (ret)
		return ret; /* 메모리 부족 시 NVMe I/O 계수 초기화 실패, 상위 blkcg_policy_register 실패로 이어질 수 있음(추정) */

	/* 사라진 하위 cgroup의 잔여 통계를 담을 aux_cnt를 0으로 초기화 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)
	/* BLKG_RWSTAT_READ(0)..BLKG_RWSTAT_DISCARD(4)까지 NVMe opcode 방향/동기성별 원자 카운터 초기화 */
		atomic64_set(&rwstat->aux_cnt[i], 0);
		/* aux_cnt는 하위 cgroup offline/삭제 후에도 완료되지 않은 NVMe SQ/CQ 잔여 I/O를 상위 cgroup 보고로 이전할 때 사용(추정) */
	return 0;
}
EXPORT_SYMBOL_GPL(blkg_rwstat_init);

/*
 * blkg_rwstat_exit - blkg_rwstat_init에서 할당한 percpu_counter 리소스를 해제한다.
 *
 * 호출 경로:
 *   blkg_destroy -> 정책 private data 해제 -> blkg_rwstat_exit
 *   (또는 blkcg_policy_unregister 경로, 추정)
 *
 * NVMe 연결점:
 *   - NVMe 컨트롤러의 CQ 완료와는 별개로, cgroup 소멸 시점에 통계
 *     리소스를 반납한다.
 *   - 완료되지 않은 I/O가 있다면 해당 통계는 상위 cgroup의 aux_cnt로
 *     이전된 후에 해제될 수 있다(추정).
 */
void blkg_rwstat_exit(struct blkg_rwstat *rwstat)
/* @rwstat: 해제할 blkcg 통계 블록, 이후 nvme_queue_rq 경로에서의 blkg_rwstat_add()는 더 이상 유효하지 않음(추정) */
{
	/* per-CPU 카운터 해제: 이후 blkg_rwstat_add() 호출은 불가능해진다(추정) */
	percpu_counter_destroy_many(rwstat->cpu_cnt, BLKG_RWSTAT_NR);
	/* BLKG_RWSTAT_NR(5)개 percpu_counter를 해제, NVMe READ/WRITE/DISCARD/SYNC/ASYNC 누적용 CPU 로컬 카운터 정리 */
}
EXPORT_SYMBOL_GPL(blkg_rwstat_exit);

/**
 * __blkg_prfill_rwstat - prfill helper for a blkg_rwstat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @rwstat: rwstat to print
 *
 * Print @rwstat to @sf for the device assocaited with @pd.
 */
/*
 * __blkg_prfill_rwstat - blkg_rwstat_sample 값을 seq_file에 출력한다.
 *
 * 목적: cgroup v1/v2 인터페이스에서 NVMe 장치별 READ/WRITE/DISCARD/SYNC/
 * ASYNC 통계를 사용자에게 보여주는 핵심 출력 루틴이다.
 *
 * 호출 경로:
 *   cgroup 파일 read -> blkg_prfill_rwstat -> __blkg_prfill_rwstat
 *
 * NVMe 연결점:
 *   - dname은 NVMe 장치 노드(예: nvme0n1)의 이름이다.
 *   - "Total"은 READ+WRITE+DISCARD의 합으로, NVMe SQ에 실제로 발행된
 *     데이터 양(또는 요청 수)의 상위 뷰를 제공한다.
 *   - 본 함수는 NVMe doorbell/CID/SQ/CQ 상태를 직접 읽지 않고,
 *     blk-cgroup이 미리 누적한 소프트웨어 카운터만 출력한다.
 */
u64 __blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
			 const struct blkg_rwstat_sample *rwstat)
/* @sf: 사용자 공간 cgroup 인터페이스 출력 버퍼, @pd: NVMe 장치에 매핑된 blkcg 정책 데이터, @rwstat: 스냅샷된 READ/WRITE/DISCARD/SYNC/ASYNC 카운터 */
{
	static const char *rwstr[] = {
		[BLKG_RWSTAT_READ]	= "Read",
		/* NVMe NVM command set 기준 READ(opcode 0x02/0x01) 방향의 cgroup 누적값 레이블 */
		[BLKG_RWSTAT_WRITE]	= "Write",
		/* NVMe WRITE(opcode 0x01/0x02) 방향의 cgroup 누적값 레이블 */
		[BLKG_RWSTAT_SYNC]	= "Sync",
		/* REQ_SYNC 플래그가 설정된 요청: NVMe submit 시 SQ 엔트리의 우선순위/_latency-sensitive_ 동작과 연관될 수 있음(추정) */
		[BLKG_RWSTAT_ASYNC]	= "Async",
		/* REQ_SYNC 미설정: NVMe poll/irq 결정 시 async 처리 경로와 연결될 수 있음(추정) */
		[BLKG_RWSTAT_DISCARD]	= "Discard",
		/* NVMe Dataset Management/Deallocate/Trim(opcode 0x09) 또는 WRITE ZEROES(opcode 0x08) 방향의 cgroup 누적값 레이블(추정) */
	};
	const char *dname = blkg_dev_name(pd->blkg); /* NVMe 장치 이름 획득: 예) "nvme0n1" */
	u64 v;
	/* v: READ+WRITE+DISCARD 합산값, NVMe SQ에 발행된 총 data-transfer 명령량의 상위 뷰(바이트 또는 요청 수, 정책에 따라 다름) */
	int i;
	/* i: BLKG_RWSTAT_NR(5) 루프 인덱스, NVMe opcode 방향/동기성별 출력 순회 */

	if (!dname)
	/* NVMe 장치 이름이 NULL이면 장치가 아직 probe 중이거나 제거된 상태로 간주(추정) */
		return 0; /* NVMe 장치 이름이 없으면 출력 생략, 에러가 아닌 0 반환으로 상위 seq_printf 흐름 유지 */

	/* "nvme0n1 Read 12345" 형태로 READ/WRITE/SYNC/ASYNC/DISCARD 출력 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)
	/* BLKG_RWSTAT_READ(0)부터 BLKG_RWSTAT_DISCARD(4)까지 NVMe 방향/동기성별 cgroup 카운터 출력 순회 */
		seq_printf(sf, "%s %s %llu\n", dname, rwstr[i],
			   rwstat->cnt[i]);
		/* rwstat->cnt[i]는 nvme_queue_rq 직전 blkg_rwstat_add()로 누적된 percpu_counter+aux_cnt의 스냅샷(추정) */

	/* READ+WRITE+DISCARD 합산: SYNC/ASYNC는 Total에 미포함 */
	v = rwstat->cnt[BLKG_RWSTAT_READ] +
	/* NVMe READ 방향 누적값: PRP/SGL을 통해 호스트->컨트롤러로 전송된 READ 명령의 데이터/요청량 상위 뷰 */
	    rwstat->cnt[BLKG_RWSTAT_WRITE] +
	    /* NVMe WRITE 방향 누적값: doorbell 발행 전까지의 WRITE 생성량 */
	    rwstat->cnt[BLKG_RWSTAT_DISCARD];
	    /* NVMe discard/write-zeroes 누적값: Dataset Management 등으로 매핑된 요청량 */
	seq_printf(sf, "%s Total %llu\n", dname, v);
	/* "Total"은 NVMe data-transfer 계열(READ/WRITE/DISCARD) 명령의 cgroup 누적 총합, CID/tag/SQ 선택과는 무관한 소프트웨어 집계(추정) */
	return v;
	/* 반환값 v는 상위 seq_file 출력 헬퍼에서 추가 처리(예: 콜백 누적)에 사용될 수 있음 */
}
EXPORT_SYMBOL_GPL(__blkg_prfill_rwstat);

/**
 * blkg_prfill_rwstat - prfill callback for blkg_rwstat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @off: offset to the blkg_rwstat in @pd
 *
 * prfill callback for printing a blkg_rwstat.
 */
/*
 * blkg_prfill_rwstat - blkg_policy_data 낸부의 blkg_rwstat를 읽어 출력한다.
 *
 * 목적: cgroup 파일 콜백에서 직접 호출되며, 낸부 rwstat 위치를 계산한 뒤
 * blkg_rwstat_read()로 샘플을 뽑아 __blkg_prfill_rwstat에 넘긴다.
 *
 * 호출 경로:
 *   blkg_print_stat_funcs -> blkg_prfill_rwstat ->
 *   blkg_rwstat_read -> __blkg_prfill_rwstat
 *
 * NVMe 연결점:
 *   - @off는 blkcg_policy_data 안에서 struct blkg_rwstat가 위치한
 *     바이트 오프셋이다. NVMe 관련 정책(예: blk-throttle.c)은 여기에
 *     READ/WRITE/DISCARD 누적값을 저장한다(추정).
 */
u64 blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
		       int off)
/* @off: blkg_policy_data 낸부의 struct blkg_rwstat 바이트 오프셋, NVMe throttle/cost 정책이 READ/WRITE/DISCARD 누적값을 배치한 위치(추정) */
{
	struct blkg_rwstat_sample rwstat = { };
	/* 사용자 공간 출력용 NVMe 방향별 스냅샷 버퍼, blkg_rwstat_read()로 percpu_counter+aux_cnt를 일관되게 복사(추정) */

	/* @off 위치의 blkg_rwstat에서 READ/WRITE/DISCARD/SYNC/ASYNC 스냅샷 읽기 */
	blkg_rwstat_read((void *)pd + off, &rwstat);
	/* (void *)pd + off: NVMe 관련 blkcg 정책 데이터 내 rwstat 포인터 연산, blkg_rwstat_read는 cpu_cnt[]+aux_cnt[]의 원자적/일관적 합산 수행(추정) */
	return __blkg_prfill_rwstat(sf, pd, &rwstat);
	/* 스냅샷을 seq_file에 출력: NVMe 장치 노드별 READ/WRITE/DISCARD/SYNC/ASYNC/Total 라인 생성 */
}
EXPORT_SYMBOL_GPL(blkg_prfill_rwstat);

/**
 * blkg_rwstat_recursive_sum - collect hierarchical blkg_rwstat
 * @blkg: blkg of interest
 * @pol: blkcg_policy which contains the blkg_rwstat
 * @off: offset to the blkg_rwstat in blkg_policy_data or @blkg
 * @sum: blkg_rwstat_sample structure containing the results
 *
 * Collect the blkg_rwstat specified by @blkg, @pol and @off and all its
 * online descendants and their aux counts.  The caller must be holding the
 * queue lock for online tests.
 *
 * If @pol is NULL, blkg_rwstat is at @off bytes into @blkg; otherwise, it
 * is at @off bytes into @blkg's blkg_policy_data of the policy.
 */
/*
 * blkg_rwstat_recursive_sum - 지정한 blkg와 하위 online cgroup의 rwstat를 합산한다.
 *
 * 목적: 상위 cgroup이 NVMe 장치를 공유하는 여러 하위 cgroup의 I/O 사용량을
 * 트리 구조로 집계할 때 사용된다.
 *
 * 호출 경로:
 *   cgroup stat read -> blkg_rwstat_recursive_sum ->
 *   blkg_rwstat_read_counter (percpu_counter + aux_cnt)
 *
 * NVMe 연결점:
 *   - queue_lock을 보유해야 하므로, NVMe I/O가 blk-mq에서
 *     스케줄링/재정렬되는 동안에도 일관된 뷰를 제공한다.
 *   - 하위 cgroup이 offline이면 해당 통계를 제외한다.
 *   - aux_cnt까지 합산하여 이미 제거된 하위 cgroup의 잔여 NVMe I/O까지
 *     상위 보고에 반영한다.
 */
void blkg_rwstat_recursive_sum(struct blkcg_gq *blkg, struct blkcg_policy *pol,
		int off, struct blkg_rwstat_sample *sum)
/* @blkg: NVMe 장치에 매핑된 최상위 blkcg_gq, @pol: rwstat를 포함한 blkcg 정책(예: throttle/cost), @off: 정책 데이터 또는 blkg 낸부 rwstat 오프셋, @sum: 하위 cgroup까지 합산된 NVMe 방향별 결과 */
{
	struct blkcg_gq *pos_blkg;
	/* 트리 순회 중인 현재 하위 blkcg_gq: NVMe 큐 q를 공유하는 cgroup 노드 */
	struct cgroup_subsys_state *pos_css;
	/* cgroup_subsys_state 기반의 트리 순회 커서, cgroup 계층 구조와 NVMe 장치 간 매핑 표현(추정) */
	unsigned int i;
	/* BLKG_RWSTAT_NR(5) 인덱스, 하위 cgroup의 NVMe READ/WRITE/DISCARD/SYNC/ASYNC 값을 상위 sum에 누적 */

	/* queue_lock 보유 확인: NVMe 큐 상태와 통계 뷰의 동기화 가정 */
	lockdep_assert_held(&blkg->q->queue_lock);
	/* NVMe submit/completion 경로에서 queue_lock이 잡힌 상태에서만 online 테스트와 rwstat 합산이 안전하다는 lockdep 검증(추정) */

	memset(sum, 0, sizeof(*sum)); /* 상위 집계값 초기화 */
	/* sum->cnt[5]를 0으로 지워 하위 cgroup의 NVMe 방향별 값을 깨끗하게 누적할 준비 */

	rcu_read_lock();
	/* cgroup 트리 순회 동안 하위 cgroup 제거로 인한 use-after-free 방지, NVMe 장치 공유 구조의 안전한 읽기 임계구간(추정) */
	blkg_for_each_descendant_pre(pos_blkg, pos_css, blkg) {
	/* NVMe 큐 q를 공유하는 모든 하위 cgroup을 선순위(pre-order)로 순회, 상위 cgroup이 하위 NVMe I/O 사용량을 집계 */
		struct blkg_rwstat *rwstat;
		/* 현재 하위 cgroup의 NVMe 방향별 rwstat 포인터 */

		if (!pos_blkg->online)
		/* 하위 cgroup이 offline이면 해당 cgroup에서 NVMe SQ로 발행된 새 I/O는 없으므로 집계에서 제외(추정) */
			continue; /* offline cgroup은 NVMe I/O 미발행으로 간주 */

		/* 정책별 또는 blkg 낸부의 struct blkg_rwstat 위치 계산 */
		if (pol)
		/* @pol이 주어지면 NVMe 관련 blkcg_policy_data(예: throtl_grp) 낸부의 rwstat 위치 사용(추정) */
			rwstat = (void *)blkg_to_pd(pos_blkg, pol) + off;
			/* blkg_to_pd: 정챹 private data 시작 주소, +off로 NVMe READ/WRITE/DISCARD 누적 블록 도달(추정) */
		else
		/* @pol이 NULL이면 blkcg_gq 본체의 지정 오프셋에 rwstat가 내장된 레이아웃(추정) */
			rwstat = (void *)pos_blkg + off;
			/* pos_blkg + off: blkcg_gq 낸부 NVMe 방향별 통계 블록 포인터(추정) */

		/* percpu_counter와 aux_cnt를 합산해 상위 카운터에 누적 */
		for (i = 0; i < BLKG_RWSTAT_NR; i++)
		/* READ/WRITE/SYNC/ASYNC/DISCARD별로 하위 cgroup의 NVMe I/O 누적값을 상위 sum에 합산 */
			sum->cnt[i] += blkg_rwstat_read_counter(rwstat, i);
			/* blkg_rwstat_read_counter: percpu_counter_sum_positive(cpu_cnt[i]) + atomic64_read(aux_cnt[i])로 NVMe 방향별 일관적 값 산출(추정) */
	}
	rcu_read_unlock();
	/* cgroup 트리 순회 종료, NVMe 장치-코어 매핑에 대한 RCU read-side 임계구간 해제(추정) */
}
EXPORT_SYMBOL_GPL(blkg_rwstat_recursive_sum);

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 NVMe I/O가 blk-mq를 통해 전달되기 전에 cgroup별
 *   READ/WRITE/DISCARD/SYNC/ASYNC 계수를 누적하는 계정(accounting) 계층이다.
 * - 실제 NVMe SQ/CQ/doorbell 발행은 별개이며, 이 파일은 소프트웨어
 *   카운터만 다룬다(추정).
 * - percpu_counter 기반 cpu_cnt와 atomic64 기반 aux_cnt로 구성되어
 *   고속 NVMe 경로에서의 통계 수집 오버헤드를 줄인다.
 * - blkg_rwstat_recursive_sum()은 여러 cgroup이 공유하는 NVMe 장치의
 *   계층적 사용량 집계를 지원한다.
 * - 주로 blk-cgroup.c 및 blk-throttle.c 같은 정책 파일과 함께
 *   사용되며, NVMe 드라이버 자체는 직접 호출하지 않는다(추정).
 */
