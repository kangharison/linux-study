/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 */
/*
 * [한국어 설명] 두 구간 트리의 합집합을 훑는 순회기 (double_span.h)
 *
 * === 파일의 역할 ===
 * 커널의 interval_tree_span_iter 는 구간 트리 하나를 훑으며 "쓰인 구간"과
 * "빈 구멍"을 번갈아 내어 준다. 이 헤더는 그것을 두 트리에 대해 하도록
 * 확장한다.
 *
 * iommufd 가 이것을 필요로 하는 이유가 IOAS 의 구조에 있다. 하나의 IOVA
 * 공간에 대해 두 개의 트리를 유지하는데, 하나는 실제로 매핑된 영역이고
 * 다른 하나는 사용자가 예약해 둔(아직 매핑되지 않은) 영역이다. "이
 * 주소가 비어 있는가"를 물으려면 두 트리를 동시에 봐야 한다.
 *
 * 원 주석이 규칙을 밝힌다: 겹치는 구간은 첫 트리가 우선하고, 결과는
 * 탐욕적이라 같은 종류가 연달아 나오지 않는다 — 즉 인접한 같은 종류
 * 구간은 하나로 합쳐져 나온다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommufd 의 IOVA 공간 관리 계층 안에서만 쓰인다. io_pagetable.c 가 빈
 * IOVA 를 찾거나 겹침을 확인할 때, pages.c 가 어느 부분이 이미 고정되어
 * 있는지 볼 때 이 순회기를 쓴다.
 *
 * 실행 컨텍스트: 호출자가 트리의 락을 쥐고 있어야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommufd/io_pagetable.c, iommufd/pages.c.
 * 아래: <linux/interval_tree.h> 의 한 트리짜리 순회기 두 개를 안고 돈다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct interval_tree_double_span_iter: 두 트리의 순회 상태와 이번에
 *   내어 주는 구간. is_used 가 그 구간이 어느 트리 것인지(또는 구멍인지)를
 *   말한다.
 * interval_tree_for_each_double_span: 그 순회를 도는 반복문.
 * interval_tree_double_span_iter_done: 순회가 끝났는지 — is_used 가 -1 이면
 *   끝이다.
 */
#ifndef __IOMMUFD_DOUBLE_SPAN_H	/* [한국어] 중복 포함 방지 */
#define __IOMMUFD_DOUBLE_SPAN_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/interval_tree.h>	/* [한국어] 한 트리짜리 순회기를 안고 돈다 */

/*
 * This is a variation of the general interval_tree_span_iter that computes the
 * spans over the union of two different interval trees. Used ranges are broken
 * up and reported based on the tree that provides the interval. The first span
 * always takes priority. Like interval_tree_span_iter it is greedy and the same
 * value of is_used will not repeat on two iteration cycles.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 두 트리의 합집합을 훑는 순회 상태.
 *
 * start/last 를 union 으로 둔 것이 이 구조체의 특징이다. 이번 구간이
 * 구멍인지 쓰인 곳인지에 따라 읽을 이름이 달라지는데, 값은 같은 자리에
 * 있다 — 호출자가 is_used 를 먼저 보고 알맞은 이름으로 읽으라는 뜻이다.
 */
struct interval_tree_double_span_iter {
	struct rb_root_cached *itrees[2];
	/* [한국어] 훑을 두 구간 트리.
	 * 설정자: interval_tree_double_span_iter_first.
	 * 읽는 자: 각 트리의 순회기를 다시 세울 때.
	 * 값 범위: 유효한 포인터 둘. 순서가 우선순위다 — [0] 이 이긴다.
	 * 동기화: 호출자가 트리의 락을 쥐고 있어야 한다. */
	struct interval_tree_span_iter spans[2];
	/* [한국어] 각 트리를 도는 한 트리짜리 순회기.
	 * 설정자: first/next 가 두 순회기를 각각 진행시킨다.
	 * 읽는 자: update 가 둘의 현재 위치를 견주어 이번 구간을 정한다.
	 * 값 범위: 커널 공용 순회기의 상태.
	 * 동기화: 이 구조체와 함께 호출 스택에 산다. */
	union {
		unsigned long start_hole;
		/* [한국어] 이번 구간이 구멍일 때의 시작 주소.
		 * 설정자: update.
		 * 읽는 자: is_used 가 0 일 때만 이 이름으로 읽는다.
		 * 값 범위: 순회 범위 안의 색인.
		 * 동기화: 호출 스택 값. */
		unsigned long start_used;
		/* [한국어] 이번 구간이 쓰인 곳일 때의 시작 주소.
		 * 설정자: update. start_hole 과 같은 자리를 공유한다.
		 * 읽는 자: is_used 가 1 이나 2 일 때 이 이름으로 읽는다.
		 * 값 범위: 순회 범위 안의 색인.
		 * 동기화: 호출 스택 값. */
	};
	union {	/* [한국어] 구멍일 때와 쓰인 곳일 때 이름만 달리 읽는다 — 값은 같은 자리에 있다 */
		unsigned long last_hole;
		/* [한국어] 이번 구멍의 마지막 주소(포함).
		 * 설정자: update.
		 * 읽는 자: is_used 가 0 일 때.
		 * 값 범위: start_hole 이상.
		 * 동기화: 호출 스택 값. */
		unsigned long last_used;
		/* [한국어] 이번에 쓰인 구간의 마지막 주소(포함).
		 * 설정자: update. last_hole 과 같은 자리다.
		 * 읽는 자: is_used 가 1 이나 2 일 때.
		 * 값 범위: start_used 이상.
		 * 동기화: 호출 스택 값. */
	};
	/* 0 = hole, 1 = used span[0], 2 = used span[1], -1 done iteration */
	int is_used;
	/* [한국어] (원 주석: 0=구멍, 1=첫 트리의 구간, 2=둘째 트리의 구간, -1=순회 끝)
	 * 설정자: update 가 두 순회기의 상태를 보고 정한다.
	 * 읽는 자: 호출자가 위 union 을 어느 이름으로 읽을지 고를 때, 그리고
	 *   done() 이 종료를 판정할 때.
	 * 값 범위: -1, 0, 1, 2.
	 * 동기화: 호출 스택 값. */
};

void interval_tree_double_span_iter_update(	/* [한국어] 두 순회기의 현재 위치를 견주어 이번 구간을 정한다 */
	struct interval_tree_double_span_iter *iter);
void interval_tree_double_span_iter_first(	/* [한국어] 두 트리에 대해 순회를 시작한다 */
	struct interval_tree_double_span_iter *iter,
	struct rb_root_cached *itree1, struct rb_root_cached *itree2,	/* [한국어] 겹치면 첫 트리가 우선한다 */
	unsigned long first_index, unsigned long last_index);	/* [한국어] 훑을 색인 범위 */
void interval_tree_double_span_iter_next(	/* [한국어] 다음 구간으로 넘어간다 */
	struct interval_tree_double_span_iter *iter);

/*
 * [한국어]
 * interval_tree_double_span_iter_done - 순회가 끝났는지 답한다
 *
 * @state: 순회 상태.
 * @return: 끝났으면 참.
 *
 * is_used 가 -1 이면 두 트리를 모두 훑은 것이다. 구간 종류를 담는 필드를
 * 종료 표시로도 쓰므로 별도의 플래그가 없다.
 */
static inline bool
interval_tree_double_span_iter_done(struct interval_tree_double_span_iter *state)
{
	return state->is_used == -1;	/* [한국어] 구간 종류 필드를 종료 표시로도 쓴다 */
}

/*
 * [한국어] 두 트리의 합집합을 도는 반복문.
 * 매 걸음의 구간이 span 에 담기며, span->is_used 로 그것이 구멍인지
 * 어느 트리의 구간인지 가른다.
 */
#define interval_tree_for_each_double_span(span, itree1, itree2, first_index, \
					   last_index)                        \
	for (interval_tree_double_span_iter_first(span, itree1, itree2,       \
						  first_index, last_index);   \
	     !interval_tree_double_span_iter_done(span);                      \
	     interval_tree_double_span_iter_next(span))	/* [한국어] 다음 구간으로 */

#endif	/* [한국어] 포함 방지 끝 */
