// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024 Google LLC.
 */

/*
 * [한국어 설명] STE/CD 갱신 규약과 무효화 배열의 단위 시험 (arm-smmu-v3-test.c)
 *
 * === 파일의 역할 ===
 * 이 드라이버에서 가장 다루기 어려운 두 가지를 하드웨어 없이 검증하는
 * KUnit 시험이다. 첫째는 "끊김 없는 항목 갱신"이다. 스트림 표 항목(STE)은
 * 8워드, 문맥 서술자(CD)도 8워드라 한 번의 원자적 쓰기로 바꿀 수 없다.
 * 그런데 하드웨어는 언제든 그 항목을 읽어 갈 수 있으므로, 바꾸는 도중의
 * 어중간한 상태를 하드웨어가 보면 엉뚱한 페이지 테이블을 걷거나 없는 표를
 * 가리키는 사고가 난다. arm_smmu_write_entry() 가 "지금 설정이 실제로
 * 쓰는 비트(used bits)"를 계산해 안전한 순서로 나눠 쓰는데, 그 순서가
 * 정말 안전한지를 이 파일이 확인한다.
 * 둘째는 무효화 배열(arm_smmu_invs)의 병합·해제·정리 연산이다. 도메인마다
 * "무엇을 무효화해야 하는가"를 모아 둔 배열인데, 장치가 붙고 떨어질 때마다
 * 항목이 늘고 줄며 참조 계수가 오르내린다. 그 계산이 틀리면 무효화가 빠지거나
 * 남아, 실제 하드웨어에서는 아주 드물게만 드러나는 버그가 된다.
 * 두 시험 모두 하드웨어를 전혀 건드리지 않는다 — 가짜 SMMU 구조체와 가짜
 * 주소 값만으로 순수한 계산을 검증한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 시험의 뼈대는 이렇게 돈다:
 *
 *   KUnit 실행기
 *     → arm_smmu_v3_test_suite_init()  (우회·중단 STE 를 미리 지어 둔다)
 *     → 각 KUNIT_CASE
 *       → arm_smmu_test_make_*_ste/cd()  (가짜 장치·도메인으로 항목을 짓는다)
 *       → arm_smmu_v3_test_*_expect_transition()
 *         → arm_smmu_write_entry()   ← 검증 대상 (arm-smmu-v3.c 의 실제 코드)
 *           → ops->sync()  = arm_smmu_test_writer_record_syncs()  ← 이 파일
 *
 * 핵심은 sync 갈고리를 가로챈다는 점이다. 실제 드라이버에서는 sync 가
 * 명령 큐에 무효화를 넣고 하드웨어를 기다리지만, 여기서는 그 자리에서
 * "지금 항목이 안전한 상태인가"를 검사한다. 하드웨어가 항목을 볼 수 있는
 * 시점이 곧 sync 시점이므로, 그 순간마다 검사하면 규약 위반을 잡아낼 수 있다.
 * 실행 컨텍스트는 KUnit 모듈의 프로세스 문맥이다. 인터럽트도, 하드웨어도,
 * 실제 DMA 도 없다.
 *
 * === 타 모듈과의 연결 ===
 * 검증 대상은 arm-smmu-v3.c 의 arm_smmu_write_entry(), arm_smmu_get_ste_used(),
 * arm_smmu_get_cd_used(), 항목을 짓는 arm_smmu_make_*_ste/cd(), 그리고
 * 무효화 배열 연산 arm_smmu_invs_merge/unref/purge() 다. 이들 중 일부는
 * 평소에는 static 이지만, VISIBLE_IF_KUNIT 과 EXPORT_SYMBOL_IF_KUNIT 으로
 * 시험 빌드에서만 열려 이 파일이 부를 수 있게 된다 — 파일 끝의
 * MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING") 이 그 통로다.
 * arm-smmu-v3-sva.c 의 arm_smmu_make_sva_cd() 도 검증 대상이다. SVA 서술자와
 * 그 "모두 폴트" 변형 사이의 전환이 끊김 없어야 프로세스가 죽는 순간에도
 * 장치가 안전하게 멈추기 때문이다.
 * io-pgtable 은 실제로 쓰지 않고, 페이지 테이블 설정 구조체만 가짜 값으로
 * 채워 넘긴다 — 항목을 짓는 계산만 검증하면 되기 때문이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct arm_smmu_test_writer: 실제 writer 를 감싸, sync 가 불릴 때마다
 *   횟수를 세고 항목의 안전성을 검사하는 시험용 껍데기.
 * - arm_smmu_test_writer_record_syncs(): 시험의 심장. 매 sync 마다 지금 항목이
 *   "출발 항목" 또는 "도착 항목" 중 하나와 사용 비트 기준으로 같은지 확인한다.
 *   둘 다와 다르면 하드웨어가 보면 안 될 중간 상태라는 뜻이다.
 * - arm_smmu_entry_differs_in_used_bits(): 그 비교를 실제로 수행한다.
 * - arm_smmu_v3_test_ste_expect_hitless_transition(): 한 전환이 끊김 없이,
 *   그리고 정확히 몇 번의 sync 로 끝나야 하는지를 못 박는다.
 * - arm_smmu_v3_invs_test(): 무효화 배열을 11단계로 병합·해제·정리하며
 *   매 단계의 id·참조 수·PASID 를 표와 대조한다.
 */

#include <kunit/test.h>	/* [한국어] KUNIT_CASE, KUNIT_EXPECT_* 같은 시험 뼈대. */
#include <linux/io-pgtable.h>	/* [한국어] 페이지 테이블 설정 구조체 — 실제로 테이블을 만들지는 않고 값만 채워 쓴다. */

#include "arm-smmu-v3.h"	/* [한국어] 검증 대상인 STE/CD 규격과 헬퍼들. */

/* [한국어] 실제 항목 기록기(arm_smmu_entry_writer)를 감싼 시험용 껍데기.
 *
 * 실제 드라이버는 sync 갈고리에서 명령 큐에 무효화를 넣지만, 시험에서는
 * 그 자리에서 "지금 항목이 안전한가"를 검사한다. 그 검사를 하려면 출발
 * 항목과 도착 항목, 그리고 지금 값이 모두 필요해서 이 구조가 생겼다. */
struct arm_smmu_test_writer {
	/* [한국어] 검증 대상 코드가 아는 기록기 몸통 — 반드시 첫 필드여야 한다.
	 * 설정자: 각 전환 시험이 ops 만 채워 초기화한다.
	 * 읽는 자: arm_smmu_write_entry() 가 이 포인터를 받아 ops->sync 를 부른다.
	 * 값 범위: ops 는 test_ste_ops 또는 test_cd_ops.
	 * 동기화: 시험 하나가 자기 스택 변수를 쓰므로 공유되지 않는다. */
	struct arm_smmu_entry_writer writer;
	/* [한국어] 지금 돌고 있는 시험 문맥. 실패를 보고할 대상이다.
	 * 설정자: 각 전환 시험이 자기 kunit 포인터를 넣는다.
	 * 읽는 자: KUNIT_EXPECT_* 매크로와 kunit_kzalloc().
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 없음. */
	struct kunit *test;
	/* [한국어] 전환을 시작할 때의 항목 값 (원본, 바뀌지 않는다).
	 * 설정자: 전환 시험이 cur->data 를 가리키게 한다.
	 * 읽는 자: sync 마다 "지금 값이 출발점과 같은가"를 볼 때.
	 * 값 범위: 8워드짜리 배열.
	 * 동기화: 없음 — 읽기만 한다. */
	const __le64 *init_entry;
	/* [한국어] 전환이 끝났을 때 도달해야 할 항목 값.
	 * 설정자: 전환 시험이 target->data 를 가리키게 한다.
	 * 읽는 자: sync 마다 "지금 값이 도착점과 같은가"를 볼 때.
	 * 값 범위: 8워드짜리 배열.
	 * 동기화: 없음 — 읽기만 한다. */
	const __le64 *target_entry;
	/* [한국어] 실제로 고쳐지는 작업용 항목 — 하드웨어가 본다고 가정하는 자리.
	 * 설정자: 전환 시험이 출발 항목의 복사본을 가리키게 하고,
	 *         arm_smmu_write_entry() 가 단계마다 이 값을 고친다.
	 * 읽는 자: sync 마다 이 값을 검사한다.
	 * 값 범위: 전환이 끝나면 target_entry 와 정확히 같아야 한다.
	 * 동기화: 없음. */
	__le64 *entry;

	/* [한국어] 전환 도중 항목이 무효 상태(첫 워드가 0)로 떨어진 적이 있는가.
	 * 설정자: sync 검사가 첫 워드가 0 인 것을 보면 참으로 놓는다.
	 * 읽는 자: 전환 시험이 "끊김 없음"을 기대했는지와 대조한다 —
	 *         끊김 없는 전환이라면 이 값이 거짓이어야 한다.
	 * 값 범위: true/false.
	 * 동기화: 없음. */
	bool invalid_entry_written;
	/* [한국어] 전환이 끝날 때까지 sync 가 몇 번 불렸는가.
	 * 설정자: sync 검사가 한 번 불릴 때마다 1 씩 올린다.
	 * 읽는 자: 전환 시험이 기대한 횟수와 정확히 같은지 확인한다.
	 * 값 범위: 보통 1~3. 이 횟수가 곧 하드웨어 무효화 비용이므로
	 *         숫자를 못 박아 두면 최적화가 무너질 때 바로 드러난다.
	 * 동기화: 없음. */
	unsigned int num_syncs;
};

#define NUM_ENTRY_QWORDS 8	/* [한국어] STE 도 CD 도 8워드(64바이트)다. 두 시험이 같은 크기를 쓰므로 하나로 묶었다. */
#define NUM_EXPECTED_SYNCS(x) x	/* [한국어] 값은 그대로지만, 호출부에서 "이 숫자는 기대 sync 횟수"임을 눈으로 알리는 이름표 역할을 한다. */

static struct arm_smmu_ste bypass_ste;	/* [한국어] 우회 STE 의 표준 값. 시험 시작 때 한 번 지어 두고 여러 시험이 함께 쓴다. */
static struct arm_smmu_ste abort_ste;	/* [한국어] 중단 STE 의 표준 값. 마찬가지로 미리 지어 둔다. */
/* [한국어] 가짜 SMMU 하나. 실제 하드웨어 없이 항목을 지으려면 능력 비트가 필요하다.
 *
 * 멈춤(STALLS)과 메모리 속성 덮어쓰기(ATTR_TYPES_OVR)를 켜 둔 이유는, 그
 * 두 기능이 켜졌을 때 STE 의 사용 비트가 늘어나 전환이 더 까다로워지기
 * 때문이다. 어려운 쪽을 기본으로 시험한다. */
static struct arm_smmu_device smmu = {
	.features = ARM_SMMU_FEAT_STALLS | ARM_SMMU_FEAT_ATTR_TYPES_OVR	/* [한국어] 이 두 기능만 있으면 시험에 필요한 항목을 모두 지을 수 있다. */
};
/* [한국어] 가짜 프로세스 주소 공간. SVA 서술자를 지을 때만 쓴다. */
static struct mm_struct sva_mm = {
	.pgd = (void *)0xdaedbeefdeadbeefULL,	/* [한국어] 알아보기 쉬운 가짜 주소 — 서술자에 이 값이 그대로 실렸는지 눈으로 확인하기 좋다. 실제로 접근하지는 않는다. */
};

/* [한국어] 시험용 장치가 어떤 성질을 갖는지 고르는 비트 플래그.
 *
 * 같은 항목이라도 ATS 를 켰는지, 멈춤을 쓰는지, 중첩인지에 따라 사용 비트가
 * 달라져 전환 단계 수가 바뀐다. 그래서 조합을 골라 시험할 수 있게 했다. */
enum arm_smmu_test_master_feat {
	/* [한국어] 장치 쪽 변환 캐시(ATS)를 쓰는 장치로 만든다.
	 * 설정자: 각 시험이 필요에 따라 넘긴다.
	 * 읽는 자: 항목을 짓는 헬퍼가 master.ats_enabled 로 옮겨 담는다.
	 * 값 범위: 비트 0.
	 * 동기화: 없음 — 시험 인자다. */
	ARM_SMMU_MASTER_TEST_ATS = BIT(0),
	/* [한국어] 폴트 때 트랜잭션을 멈춰 세우는 장치로 만든다.
	 * 설정자: 위와 같다.
	 * 읽는 자: master.stall_enabled 로 옮겨 담긴다.
	 * 값 범위: 비트 1.
	 * 동기화: 없음. */
	ARM_SMMU_MASTER_TEST_STALL = BIT(1),
	/* [한국어] 중첩 변환(게스트 1단계 + 호스트 2단계) 항목으로 만든다.
	 * 설정자: 위와 같다.
	 * 읽는 자: 항목을 지은 뒤 설정 갈래를 NESTED 로 바꾸고 2단계 워드를 덧댄다.
	 * 값 범위: 비트 2.
	 * 동기화: 없음. */
	ARM_SMMU_MASTER_TEST_NESTED = BIT(2),
};

/* [한국어] 중첩 항목을 지을 때 2단계 부분이 필요해 미리 알려 둔다 —
 * 정의는 아래에 있고, 그보다 위에 있는 cdtable 헬퍼가 이것을 부른다. */
static void arm_smmu_test_make_s2_ste(struct arm_smmu_ste *ste,
				      enum arm_smmu_test_master_feat feat);

/*
 * [한국어]
 * arm_smmu_entry_differs_in_used_bits - 두 항목이 "쓰이는 비트"에서 다른가
 *
 * @entry: 지금 항목 값.
 * @used_bits: 지금 설정이 실제로 쓰는 비트 마스크.
 * @target: 견줄 대상 항목.
 * @safe: 달라도 괜찮다고 표시된 비트 마스크.
 * @length: 워드 수.
 * @return: 다르면 참.
 *
 * 이 시험의 판정 기준을 구현한 함수다. 항목의 모든 비트가 같을 필요는 없다 —
 * 지금 설정이 읽지 않는 비트는 무엇이 들어 있든 하드웨어 동작에 영향이 없다.
 * 그래서 "지금 쓰이는 비트"만 골라 견주고, 그중에서도 갱신 도중 달라져도
 * 안전하다고 표시된 비트(safe)는 제외한다.
 *
 * 실행 컨텍스트: 시험 프로세스 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_test_writer_record_syncs() → [이 함수]
 */
static bool arm_smmu_entry_differs_in_used_bits(const __le64 *entry,
						const __le64 *used_bits,
						const __le64 *target,
						const __le64 *safe,
						unsigned int length)
{
	bool differs = false;	/* [한국어] 하나라도 다르면 참이 된다. */
	unsigned int i;	/* [한국어] 워드 반복자. */

	for (i = 0; i < length; i++) {	/* [한국어] 항목의 워드를 하나씩 본다. */
		__le64 used = used_bits[i] & ~safe[i];	/* [한국어] 쓰이는 비트에서 "달라도 안전한" 비트를 뺀다 — 남은 것이 반드시 일치해야 할 비트다. */

		if ((entry[i] & used) != (target[i] & used))	/* [한국어] 그 비트만 골라 견준다. */
			differs = true;	/* [한국어] 다르면 표시해 둔다. 끝까지 도는 이유는 없지만, 조기 탈출보다 코드가 단순하다. */
	}
	return differs;	/* [한국어] 하나도 다르지 않았으면 거짓. */
}

/*
 * [한국어]
 * arm_smmu_test_writer_record_syncs - sync 시점마다 항목이 안전한지 검사한다
 *
 * @writer: 검증 대상 코드가 넘겨준 기록기 (시험용 껍데기를 품고 있다).
 *
 * 이 파일의 심장이다. 실제 드라이버에서 sync 는 "지금까지 쓴 내용을 하드웨어에
 * 반영하고 설정 캐시를 씻는" 자리이고, 곧 하드웨어가 새 항목 값을 볼 수 있게
 * 되는 시점이다. 따라서 그 순간의 항목은 반드시 안전한 상태여야 한다.
 *
 * "안전한 상태"의 정의가 이 함수의 요점이다 — 지금 항목이 출발 항목과 같거나,
 * 도착 항목과 같아야 한다. 단, 전체 비트가 아니라 "지금 항목의 설정이 실제로
 * 읽는 비트"만 견준다. 둘 다와 다르다면 하드웨어가 어느 쪽으로도 해석할 수
 * 없는 중간 상태라는 뜻이고, 그것이 곧 규약 위반이다.
 *
 * 항목의 첫 워드가 0 이면 무효 항목이라 이 검사를 건너뛰고, 대신 "무효 상태를
 * 거쳤다"고 기록한다. 무효 상태를 거치는 전환은 끊김이 있는 전환이며, 그것을
 * 기대한 시험만 통과해야 한다.
 *
 * 실행 컨텍스트: arm_smmu_write_entry() 안에서 갈고리로 불린다. 시험 문맥이라
 * 잠들 수 있고 메모리도 잡을 수 있다 — 실제 드라이버의 sync 와 다른 점이다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->sync = [이 함수]
 *     → get_used()/get_update_safe() → arm_smmu_entry_differs_in_used_bits()
 */
static void
arm_smmu_test_writer_record_syncs(struct arm_smmu_entry_writer *writer)
{
	struct arm_smmu_test_writer *test_writer =	/* [한국어] 기록기 몸통에서 시험용 껍데기로 되짚는다. */
		container_of(writer, struct arm_smmu_test_writer, writer);
	__le64 *entry_used_bits;	/* [한국어] 지금 항목의 설정이 실제로 읽는 비트 마스크. */
	__le64 *safe_target;	/* [한국어] 도착 항목과 견줄 때 달라도 안전한 비트. */
	__le64 *safe_init;	/* [한국어] 출발 항목과 견줄 때 달라도 안전한 비트. */

	entry_used_bits = kunit_kzalloc(	/* [한국어] 시험이 끝나면 KUnit 이 알아서 놓아 주는 메모리로 잡는다 — 해제를 잊어도 새지 않는다. */
		test_writer->test, sizeof(*entry_used_bits) * NUM_ENTRY_QWORDS,
		GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test_writer->test, entry_used_bits);	/* [한국어] 못 잡으면 이 시험은 더 진행할 수 없다 — ASSERT 는 즉시 중단시킨다. */

	safe_target = kunit_kzalloc(test_writer->test,	/* [한국어] 같은 방식으로 도착 쪽 안전 비트 버퍼를 잡는다. */
				    sizeof(*safe_target) * NUM_ENTRY_QWORDS,
				    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test_writer->test, safe_target);	/* [한국어] 실패하면 중단. */

	safe_init = kunit_kzalloc(test_writer->test,	/* [한국어] 출발 쪽 안전 비트 버퍼. */
				  sizeof(*safe_init) * NUM_ENTRY_QWORDS,
				  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test_writer->test, safe_init);	/* [한국어] 실패하면 중단. */

	pr_debug("STE value is now set to: ");	/* [한국어] 디버그 빌드에서만 찍힌다 — 시험이 실패했을 때 어느 단계에서 무너졌는지 보기 위한 흔적이다. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8,	/* [한국어] 8바이트 단위로 항목을 16진수로 찍는다. */
			     test_writer->entry,
			     NUM_ENTRY_QWORDS * sizeof(*test_writer->entry),
			     false);

	test_writer->num_syncs += 1;	/* [한국어] 이 전환이 몇 단계로 나뉘었는지 센다 — 단계 수가 곧 하드웨어 무효화 비용이다. */
	if (!test_writer->entry[0]) {	/* [한국어] 첫 워드가 0 이면 유효 비트도 0 이라 항목 전체가 무효다. */
		test_writer->invalid_entry_written = true;	/* [한국어] 무효 상태를 거쳤다고 기록한다 — 이 전환은 끊김이 있다. */
	} else {	/* [한국어] 유효한 항목이라면 하드웨어가 이 값을 해석할 수 있으므로 안전성을 따져야 한다. */
		/*
		 * At any stage in a hitless transition, the entry must be
		 * equivalent to either the initial entry or the target entry
		 * when only considering the bits used by the current
		 * configuration.
		 */
		/* [한국어] (위 영어 주석 참고) 끊김 없는 전환이라면, 어느 단계에서
		 * 멈춰 보아도 지금 항목은 출발 항목이거나 도착 항목이어야 한다.
		 * "이어야 한다"의 기준은 지금 설정이 실제로 읽는 비트뿐이다 —
		 * 읽지 않는 비트는 무엇이 들어 있어도 동작에 영향이 없다. */
		writer->ops->get_used(test_writer->entry, entry_used_bits);	/* [한국어] 지금 항목의 설정 갈래를 보고, 그 갈래가 읽는 비트를 계산한다. */
		if (writer->ops->get_update_safe)	/* [한국어] CD 기록기에는 이 갈고리가 없다 — 있는 경우에만 부른다. */
			writer->ops->get_update_safe(test_writer->entry,	/* [한국어] 출발 항목으로 가는 길에서 달라도 안전한 비트를 계산한다. */
						     test_writer->init_entry,
						     safe_init);
		if (writer->ops->get_update_safe)	/* [한국어] 같은 검사를 도착 쪽에도 한다. 두 번 나눠 부른 것은 두 마스크가 서로 다르기 때문이다. */
			writer->ops->get_update_safe(test_writer->entry,
						     test_writer->target_entry,
						     safe_target);
		KUNIT_EXPECT_FALSE(	/* [한국어] "출발과도 다르고 도착과도 다르다"가 거짓이어야 한다 — 곧 둘 중 하나와는 같아야 한다. */
			test_writer->test,
			arm_smmu_entry_differs_in_used_bits(	/* [한국어] 출발 항목과 견준다. */
				test_writer->entry, entry_used_bits,
				test_writer->init_entry, safe_init,
				NUM_ENTRY_QWORDS) &&
				arm_smmu_entry_differs_in_used_bits(	/* [한국어] 그리고 도착 항목과도 견준다. AND 이므로 둘 다 다를 때만 실패한다. */
					test_writer->entry, entry_used_bits,
					test_writer->target_entry, safe_target,
					NUM_ENTRY_QWORDS));
	}
}

/*
 * [한국어]
 * arm_smmu_v3_test_debug_print_used_bits - 항목의 사용 비트를 로그에 찍는다
 *
 * @writer: 기록기 (여기서는 쓰지 않는다 — 호출 형태를 맞추기 위한 인자다).
 * @ste: 사용 비트를 계산할 항목.
 *
 * 시험이 실패했을 때 "왜 이 비트가 문제였는지" 알아보려면 사용 비트 마스크를
 * 눈으로 봐야 한다. 디버그 로그를 켰을 때만 찍히므로 평소에는 비용이 없다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_v3_test_ste/cd_expect_transition() → [이 함수]
 *     → arm_smmu_get_ste_used()
 */
static void
arm_smmu_v3_test_debug_print_used_bits(struct arm_smmu_entry_writer *writer,
				       const __le64 *ste)
{
	__le64 used_bits[NUM_ENTRY_QWORDS] = {};	/* [한국어] 계산 결과를 담을 스택 버퍼. 0 으로 채워 시작한다. */

	arm_smmu_get_ste_used(ste, used_bits);	/* [한국어] 이 항목의 설정 갈래가 읽는 비트를 계산한다. */
	pr_debug("STE used bits: ");	/* [한국어] 이어서 찍힐 값이 무엇인지 알리는 머리말. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8, used_bits,	/* [한국어] 마스크를 16진수로 찍는다. */
			     sizeof(used_bits), false);
}

/* [한국어] STE 시험용 기록기 연산표.
 *
 * sync 만 시험용으로 바꿔 끼우고, 사용 비트 계산은 실제 드라이버 함수를
 * 그대로 쓴다 — 그 계산이 맞는지가 시험의 주제이기 때문이다. */
static const struct arm_smmu_entry_writer_ops test_ste_ops = {
	.sync = arm_smmu_test_writer_record_syncs,	/* [한국어] 하드웨어 무효화 대신 안전성 검사를 끼워 넣는다. */
	.get_used = arm_smmu_get_ste_used,	/* [한국어] 실제 드라이버의 STE 사용 비트 계산. */
	.get_update_safe = arm_smmu_get_ste_update_safe,	/* [한국어] 갱신 도중 달라져도 안전한 비트 계산 — STE 에만 있다. */
};

/* [한국어] CD 시험용 기록기 연산표.
 *
 * get_update_safe 가 없다는 점이 STE 와 다르다. 문맥 서술자에는 "달라도
 * 안전한 비트"라는 개념이 필요 없어서, 그 갈고리가 NULL 로 남는다. */
static const struct arm_smmu_entry_writer_ops test_cd_ops = {
	.sync = arm_smmu_test_writer_record_syncs,	/* [한국어] STE 와 같은 검사 함수를 재사용한다. */
	.get_used = arm_smmu_get_cd_used,	/* [한국어] 실제 드라이버의 CD 사용 비트 계산. */
};

/*
 * [한국어]
 * arm_smmu_v3_test_ste_expect_transition - 한 STE 전환이 규약을 지키는지 시험한다
 *
 * @test: 시험 문맥.
 * @cur: 출발 항목.
 * @target: 도착 항목.
 * @num_syncs_expected: 이 전환이 정확히 몇 단계로 끝나야 하는가.
 * @hitless: 끊김 없이 끝나야 하는가 (무효 상태를 거치면 안 되는가).
 *
 * 모든 STE 시험이 이 함수를 거친다. 출발 항목을 복사해 두고 실제
 * arm_smmu_write_entry() 를 돌린 뒤, 세 가지를 확인한다 — 무효 상태를 거쳤는지,
 * 몇 번의 sync 로 끝났는지, 그리고 끝난 값이 도착 항목과 정확히 같은지.
 *
 * sync 횟수를 못 박는 것이 중요하다. 그 횟수가 곧 하드웨어 무효화 명령의
 * 개수이자 장치가 멈춰 서는 시간이므로, 최적화가 무너지면 여기서 바로 드러난다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   각 KUNIT_CASE → expect_hitless/non_hitless_transition() → [이 함수]
 *     → arm_smmu_write_entry()
 */
static void arm_smmu_v3_test_ste_expect_transition(
	struct kunit *test, const struct arm_smmu_ste *cur,
	const struct arm_smmu_ste *target, unsigned int num_syncs_expected,
	bool hitless)
{
	struct arm_smmu_ste cur_copy = *cur;	/* [한국어] 원본을 건드리지 않도록 복사해 둔다 — 같은 출발 항목을 여러 시험이 함께 쓴다. */
	struct arm_smmu_test_writer test_writer = {	/* [한국어] 시험용 기록기를 스택에 짓는다. */
		.writer = {	/* [한국어] 기록기 몸통을 시험용 연산표로 초기화한다 — 검증 대상 코드는 이 부분만 본다. */
			.ops = &test_ste_ops,	/* [한국어] sync 를 가로채는 연산표를 건다. */
		},
		.test = test,	/* [한국어] 실패를 보고할 대상. */
		.init_entry = cur->data,	/* [한국어] 출발점 (원본을 가리킨다 — 바뀌지 않는다). */
		.target_entry = target->data,	/* [한국어] 도착점. */
		.entry = cur_copy.data,	/* [한국어] 실제로 고쳐질 복사본 — 하드웨어가 본다고 가정하는 자리다. */
		.num_syncs = 0,	/* [한국어] 아직 한 번도 안 불렸다. */
		.invalid_entry_written = false,	/* [한국어] 아직 무효 상태를 거치지 않았다. */

	};

	pr_debug("STE initial value: ");	/* [한국어] 아래 로그들은 모두 실패를 추적하기 위한 흔적이다. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8, cur_copy.data,	/* [한국어] 출발 항목을 16진수로 찍는다 — 실패했을 때 값을 눈으로 확인하기 위해서다. */
			     sizeof(cur_copy), false);
	arm_smmu_v3_test_debug_print_used_bits(&test_writer.writer, cur->data);	/* [한국어] 출발 항목이 읽는 비트도 함께 찍는다. */
	pr_debug("STE target value: ");	/* [한국어] 이어서 도착 항목을 찍는다는 머리말. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8, target->data,	/* [한국어] 도착 항목을 16진수로 찍는다. */
			     sizeof(cur_copy), false);
	arm_smmu_v3_test_debug_print_used_bits(&test_writer.writer,	/* [한국어] 도착 항목이 읽는 비트. 두 마스크의 차이가 전환 단계 수를 결정한다. */
					       target->data);

	arm_smmu_write_entry(&test_writer.writer, cur_copy.data, target->data);	/* [한국어] 검증 대상 — 실제 드라이버 코드를 그대로 돌린다. 그 안에서 sync 가 불릴 때마다 안전성이 검사된다. */

	KUNIT_EXPECT_EQ(test, test_writer.invalid_entry_written, !hitless);	/* [한국어] 끊김 없기를 기대했다면 무효 상태를 거치지 않았어야 한다. 반대로 끊김을 기대했다면 반드시 거쳤어야 한다. */
	KUNIT_EXPECT_EQ(test, test_writer.num_syncs, num_syncs_expected);	/* [한국어] 단계 수가 정확히 기대와 같아야 한다 — 적어도, 많아도 안 된다. */
	KUNIT_EXPECT_MEMEQ(test, target->data, cur_copy.data, sizeof(cur_copy));	/* [한국어] 마지막에는 모든 비트가 도착 항목과 같아야 한다 — 사용 비트뿐 아니라 전부다. */
}

/*
 * [한국어]
 * arm_smmu_v3_test_ste_expect_non_hitless_transition - 끊김이 있어야 하는 전환
 *
 * @test: 시험 문맥.
 * @cur: 출발 항목.
 * @target: 도착 항목.
 * @num_syncs_expected: 기대 단계 수.
 *
 * hitless=false 를 넘기는 얇은 껍데기다. 이름으로 의도를 드러내 시험 코드를
 * 읽기 쉽게 만든다 — 어떤 전환은 원리상 끊김 없이 할 수 없고, 그 사실 자체를
 * 시험으로 못 박아 두는 것이다.
 *
 * 호출 체인:
 *   arm_smmu_v3_write_ste_test_non_hitless() → [이 함수]
 *     → arm_smmu_v3_test_ste_expect_transition()
 */
static void arm_smmu_v3_test_ste_expect_non_hitless_transition(
	struct kunit *test, const struct arm_smmu_ste *cur,
	const struct arm_smmu_ste *target, unsigned int num_syncs_expected)
{
	arm_smmu_v3_test_ste_expect_transition(test, cur, target,	/* [한국어] 끊김을 허용(기대)하는 전환으로 넘긴다. */
					       num_syncs_expected, false);
}

/*
 * [한국어]
 * arm_smmu_v3_test_ste_expect_hitless_transition - 끊김 없어야 하는 전환
 *
 * @test: 시험 문맥.
 * @cur: 출발 항목.
 * @target: 도착 항목.
 * @num_syncs_expected: 기대 단계 수.
 *
 * 대부분의 시험이 이 쪽을 쓴다. 실제 운용에서 일어나는 전환은 모두 끊김이
 * 없어야 하며, 그것이 이 드라이버가 STE 를 여러 단계로 나눠 쓰는 이유다.
 *
 * 호출 체인:
 *   각 STE 전환 KUNIT_CASE → [이 함수]
 *     → arm_smmu_v3_test_ste_expect_transition()
 */
static void arm_smmu_v3_test_ste_expect_hitless_transition(
	struct kunit *test, const struct arm_smmu_ste *cur,
	const struct arm_smmu_ste *target, unsigned int num_syncs_expected)
{
	arm_smmu_v3_test_ste_expect_transition(test, cur, target,	/* [한국어] 끊김을 허용하지 않는 전환으로 넘긴다. */
					       num_syncs_expected, true);
}

static const dma_addr_t fake_cdtab_dma_addr = 0xF0F0F0F0F0F0;	/* [한국어] 문맥 서술자 표의 가짜 장치 쪽 주소. 눈에 띄는 값이라 항목에 제대로 실렸는지 확인하기 쉽다 — 실제로 접근하지는 않는다. */

/*
 * [한국어]
 * arm_smmu_test_make_cdtable_ste - 1단계(문맥 표) STE 를 가짜 장치로 짓는다
 *
 * @ste: 지어 담을 자리.
 * @s1dss: PASID 0 이 아닌 트랜잭션을 어떻게 다룰지 (우회/중단/서술자 사용).
 * @dma_addr: 문맥 서술자 표의 장치 쪽 주소.
 * @feat: 시험할 장치 성질 조합 (ATS/멈춤/중첩).
 *
 * 실제 드라이버가 장치를 붙일 때 짓는 것과 똑같은 항목을 하드웨어 없이
 * 만든다. 가짜 master 구조체에 필요한 필드만 채워 넘기는 것이 요령이다.
 *
 * 중첩을 요청하면 한 걸음 더 나아간다 — 먼저 1단계 항목을 짓고, 설정 갈래를
 * NESTED 로 바꾼 뒤, 2단계 항목의 뒤쪽 워드를 그대로 덧댄다. 실제
 * arm-smmu-v3-iommufd.c 가 하는 일을 시험용으로 재현한 것이다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   각 KUNIT_CASE → [이 함수] → arm_smmu_make_cdtable_ste()
 */
static void arm_smmu_test_make_cdtable_ste(struct arm_smmu_ste *ste,	/* [한국어] 중첩 갈래의 항목을 짓는다 — 인자가 길어 줄을 나눴다. */
					   unsigned int s1dss,
					   const dma_addr_t dma_addr,
					   enum arm_smmu_test_master_feat feat)
{
	bool ats_enabled = feat & ARM_SMMU_MASTER_TEST_ATS;	/* [한국어] 플래그를 개별 성질로 푼다. */
	bool stall_enabled = feat & ARM_SMMU_MASTER_TEST_STALL;	/* [한국어] 멈춤을 쓰는 장치인가. */

	struct arm_smmu_master master = {	/* [한국어] 항목을 짓는 데 필요한 필드만 채운 가짜 장치. 스택에 둔다. */
		.ats_enabled = ats_enabled,	/* [한국어] STE 의 EATS 필드를 좌우한다. */
		.cd_table.cdtab_dma = dma_addr,	/* [한국어] 항목에 실릴 문맥 표 주소. */
		.cd_table.s1cdmax = 0xFF,	/* [한국어] 서술자 표가 아주 크다고 가정한다 — 사용 비트가 많아져 전환이 까다로워진다. */
		.cd_table.s1fmt = STRTAB_STE_0_S1FMT_64K_L2,	/* [한국어] 2단계 서술자 표 형식. 역시 더 복잡한 쪽을 고른다. */
		.smmu = &smmu,	/* [한국어] 위에서 만든 가짜 하드웨어 — 능력 비트를 여기서 읽는다. */
		.stall_enabled = stall_enabled,	/* [한국어] STE 의 S1STALLD 필드를 좌우한다. */
	};

	arm_smmu_make_cdtable_ste(ste, &master, ats_enabled, s1dss);	/* [한국어] 검증 대상인 실제 드라이버 함수로 항목을 짓는다. */
	if (feat & ARM_SMMU_MASTER_TEST_NESTED) {	/* [한국어] 중첩 항목을 원한 경우. */
		struct arm_smmu_ste s2ste;	/* [한국어] 뒤쪽 워드를 가져올 2단계 항목. */
		int i;	/* [한국어] 워드 반복자. */

		arm_smmu_test_make_s2_ste(&s2ste,	/* [한국어] 같은 성질의 2단계 항목을 짓는다. */
					  feat & ~ARM_SMMU_MASTER_TEST_NESTED);	/* [한국어] 중첩 플래그는 빼고 넘긴다 — 무한 재귀를 막는다. */
		ste->data[0] |= cpu_to_le64(	/* [한국어] 설정 갈래를 중첩으로 바꾼다. */
			FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_NESTED));
		ste->data[1] |= cpu_to_le64(STRTAB_STE_1_MEV);	/* [한국어] 중첩에서는 이벤트 병합을 강제한다 — 실제 코드와 같은 처리다. */
		for (i = 2; i < NUM_ENTRY_QWORDS; i++)	/* [한국어] 셋째 워드부터 끝까지. */
			ste->data[i] = s2ste.data[i];	/* [한국어] 2단계 표 주소와 VTCR 이 그 자리에 있다 — 앞 두 워드는 1단계 설정이라 그대로 둔다. */
	}
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_bypass_to_abort - 우회에서 중단으로 가는 전환
 *
 * @test: 시험 문맥.
 *
 * (아래 영어 주석 참고) 우회 항목은 첫 두 워드를 쓰고, 중단 항목은 첫 워드만
 * 쓴다. 그래서 두 단계가 필요하다 — 먼저 첫 워드를 중단으로 바꾸고(이 순간
 * 이미 유효한 중단 항목이다), 그다음 이제는 읽히지 않는 둘째 워드를 정리한다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_bypass_to_abort(struct kunit *test)
{
	/*
	 * Bypass STEs has used bits in the first two Qwords, while abort STEs
	 * only have used bits in the first QWord. Transitioning from bypass to
	 * abort requires two syncs: the first to set the first qword and make
	 * the STE into an abort, the second to clean up the second qword.
	 */
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 두 단계로, 끊김 없이 끝나야 한다. */
		test, &bypass_ste, &abort_ste, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_abort_to_bypass - 중단에서 우회로 가는 전환
 *
 * @test: 시험 문맥.
 *
 * (아래 영어 주석 참고) 방향이 반대라 순서도 뒤집힌다 — 아직 읽히지 않는
 * 둘째 워드를 먼저 채워 두고, 그다음 첫 워드를 우회로 바꾼다. 첫 워드를
 * 먼저 바꾸면 하드웨어가 아직 채워지지 않은 둘째 워드를 읽게 된다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_abort_to_bypass(struct kunit *test)
{
	/*
	 * Transitioning from abort to bypass also requires two syncs: the first
	 * to set the second qword data required by the bypass STE, and the
	 * second to set the first qword and switch to bypass.
	 */
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 역시 두 단계. */
		test, &abort_ste, &bypass_ste, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_cdtable_to_abort - 1단계 변환에서 중단으로
 *
 * @test: 시험 문맥.
 *
 * 장치를 떼거나 도메인을 내릴 때 실제로 일어나는 전환이다. 문맥 표를
 * 가리키던 항목을 중단으로 바꾸는데, 이 과정에서 장치가 한순간이라도
 * 변환 없이 메모리에 닿으면 안 되므로 끊김 없어야 한다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_test_make_cdtable_ste()
 *     → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_cdtable_to_abort(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 출발이 될 1단계 항목. */

	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] PASID 0 만 서술자를 쓰는 흔한 설정. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &ste, &abort_ste,	/* [한국어] 두 단계로 끝나야 한다. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_abort_to_cdtable - 중단에서 1단계 변환으로
 *
 * @test: 시험 문맥.
 *
 * 장치를 처음 붙일 때 일어나는 전환이다. 중단 상태에서 시작해 문맥 표를
 * 가리키게 만든다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_abort_to_cdtable(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 도착이 될 1단계 항목. */

	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 출발이 될 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &abort_ste, &ste,	/* [한국어] 두 단계. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_cdtable_to_bypass - 1단계 변환에서 우회로
 *
 * @test: 시험 문맥.
 *
 * 세 단계가 필요하다 — 중단을 거쳐 가야 하기 때문이다. 1단계 항목과 우회
 * 항목은 읽는 비트가 크게 달라, 한 번에 바꾸면 어느 쪽으로도 해석되지 않는
 * 중간 상태가 생긴다. 그래서 일단 안전한 중단 상태로 내렸다가 다시 올린다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_cdtable_to_bypass(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 출발이 될 1단계 항목. */

	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 도착이 될 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &ste, &bypass_ste,	/* [한국어] 중단을 경유하므로 세 단계다. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_bypass_to_cdtable - 우회에서 1단계 변환으로
 *
 * @test: 시험 문맥.
 *
 * 위와 반대 방향이며, 같은 이유로 세 단계가 든다. 장치를 우회 상태로 두었다가
 * 실제 도메인에 붙일 때 일어나는 전환이다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_bypass_to_cdtable(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 도착이 될 1단계 항목. */

	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 출발이 될 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &bypass_ste, &ste,	/* [한국어] 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_cdtable_s1dss_change - PASID 기본 동작만 바꾸기
 *
 * @test: 시험 문맥.
 *
 * (아래 영어 주석 참고) s1dss 는 "서술자가 없는 PASID 로 온 트랜잭션을 어떻게
 * 할 것인가"를 정하는 필드이고, STE 의 둘째 워드에만 있다. 한 워드 안의
 * 변경이라 한 번의 쓰기로 끝난다 — 이 시험은 그 최적화가 유지되는지 지킨다.
 * 양방향을 모두 확인해 대칭성까지 못 박는다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition() x2
 */
static void arm_smmu_v3_write_ste_test_cdtable_s1dss_change(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] PASID 0 만 서술자를 쓰는 항목. */
	struct arm_smmu_ste s1dss_bypass;	/* [한국어] 서술자 없는 PASID 는 우회시키는 항목. */

	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 도착이 될 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_test_make_cdtable_ste(&s1dss_bypass, STRTAB_STE_1_S1DSS_BYPASS,	/* [한국어] 나머지 설정은 같고 s1dss 만 다르다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);

	/*
	 * Flipping s1dss on a CD table STE only involves changes to the second
	 * qword of an STE and can be done in a single write.
	 */
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 한 번의 쓰기로 끝나야 한다. */
		test, &ste, &s1dss_bypass, NUM_EXPECTED_SYNCS(1));
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 되돌아오는 방향도 마찬가지. */
		test, &s1dss_bypass, &ste, NUM_EXPECTED_SYNCS(1));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s1dssbypass_to_stebypass - 두 가지 "우회" 사이의 전환
 *
 * @test: 시험 문맥.
 *
 * 이름이 헷갈리기 쉬운 두 상태를 구분하는 시험이다. s1dss 우회는 "문맥 표는
 * 있지만 서술자 없는 PASID 는 그냥 통과시켜라"이고, STE 우회는 "이 스트림은
 * 변환 자체를 하지 않는다"이다. 겉보기 동작이 비슷해도 하드웨어가 읽는
 * 비트가 다르므로, 전환에 두 단계가 든다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void
arm_smmu_v3_write_ste_test_s1dssbypass_to_stebypass(struct kunit *test)
{
	struct arm_smmu_ste s1dss_bypass;	/* [한국어] 문맥 표를 가진 채 s1dss 만 우회인 항목. */

	arm_smmu_test_make_cdtable_ste(&s1dss_bypass, STRTAB_STE_1_S1DSS_BYPASS,	/* [한국어] s1dss 만 우회로 둔 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 두 단계. */
		test, &s1dss_bypass, &bypass_ste, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_stebypass_to_s1dssbypass - 그 반대 방향
 *
 * @test: 시험 문맥.
 *
 * 위 시험의 역방향. 두 상태 사이를 양쪽으로 오갈 수 있어야 실제 붙이기·떼기
 * 흐름에서 막히지 않는다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void
arm_smmu_v3_write_ste_test_stebypass_to_s1dssbypass(struct kunit *test)
{
	struct arm_smmu_ste s1dss_bypass;	/* [한국어] 도착이 될 항목. */

	arm_smmu_test_make_cdtable_ste(&s1dss_bypass, STRTAB_STE_1_S1DSS_BYPASS,	/* [한국어] 같은 항목을 도착점으로 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_v3_test_ste_expect_hitless_transition(	/* [한국어] 두 단계. */
		test, &bypass_ste, &s1dss_bypass, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_test_make_s2_ste - 2단계 변환 STE 를 가짜 도메인으로 짓는다
 *
 * @ste: 지어 담을 자리.
 * @feat: 시험할 장치 성질 조합.
 *
 * 2단계 항목은 페이지 테이블 설정(VTTBR, VTCR)을 STE 에 그대로 옮겨 담는다.
 * 실제로는 io-pgtable 이 그 값을 계산하지만, 여기서는 눈에 띄는 가짜 값을
 * 직접 채워 넣는다 — 항목을 짓는 계산만 검증하면 되기 때문이다.
 *
 * 각 필드에 서로 다른 값을 넣은 것도 뜻이 있다. 모두 같은 값을 넣으면
 * 필드를 뒤바꿔 담는 실수를 잡아내지 못한다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   각 KUNIT_CASE → [이 함수] → arm_smmu_make_s2_domain_ste()
 */
static void arm_smmu_test_make_s2_ste(struct arm_smmu_ste *ste,
				      enum arm_smmu_test_master_feat feat)
{
	bool ats_enabled = feat & ARM_SMMU_MASTER_TEST_ATS;	/* [한국어] 플래그를 개별 성질로 푼다. */
	bool stall_enabled = feat & ARM_SMMU_MASTER_TEST_STALL;	/* [한국어] 멈춤을 쓰는 장치인가. */
	struct arm_smmu_master master = {	/* [한국어] 가짜 장치 — 2단계 항목에는 문맥 표가 필요 없어 필드가 적다. */
		.ats_enabled = ats_enabled,	/* [한국어] 2단계 항목에서도 ATS 여부가 EATS 필드를 좌우한다. */
		.smmu = &smmu,	/* [한국어] 가짜 하드웨어의 능력 비트를 참조한다. */
		.stall_enabled = stall_enabled,
	};
	struct io_pgtable io_pgtable = {};	/* [한국어] 가짜 페이지 테이블 설정. 실제 테이블은 만들지 않는다. */
	struct arm_smmu_domain smmu_domain = {	/* [한국어] 가짜 도메인 — 항목을 지을 때 이 안의 설정을 읽는다. */
		.pgtbl_ops = &io_pgtable.ops,	/* [한국어] 설정 구조체로 되짚어 갈 수 있게 연산표 주소를 건다. */
	};

	io_pgtable.cfg.arm_lpae_s2_cfg.vttbr = 0xdaedbeefdeadbeefULL;	/* [한국어] 2단계 페이지 테이블의 뿌리 주소. 눈에 띄는 가짜 값이다. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.ps = 1;	/* [한국어] 물리 주소 폭 코드. 아래 값들은 모두 서로 달라, 필드를 섞어 담는 실수를 잡아낸다. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.tg = 2;	/* [한국어] 알갱이 크기 코드. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.sh = 3;	/* [한국어] 공유 영역 코드. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.orgn = 1;	/* [한국어] 바깥 캐시 정책. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.irgn = 2;	/* [한국어] 안쪽 캐시 정책. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.sl = 3;	/* [한국어] 시작 단계 — 2단계 표는 어느 단계부터 걷는지가 다르다. */
	io_pgtable.cfg.arm_lpae_s2_cfg.vtcr.tsz = 4;	/* [한국어] 입력 주소 폭. */

	arm_smmu_make_s2_domain_ste(ste, &master, &smmu_domain, ats_enabled);	/* [한국어] 검증 대상인 실제 드라이버 함수로 항목을 짓는다. */
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s2_to_abort - 2단계 변환에서 중단으로
 *
 * @test: 시험 문맥.
 *
 * 가상 머신에서 장치를 떼어 낼 때 일어나는 전환이다. 2단계 항목은 뒤쪽
 * 워드까지 쓰지만, 중단으로 가는 길은 첫 워드만 바꾸면 되므로 두 단계다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s2_to_abort(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 출발이 될 2단계 항목. */

	arm_smmu_test_make_s2_ste(&ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 출발이 될 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &ste, &abort_ste,	/* [한국어] 두 단계. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_abort_to_s2 - 중단에서 2단계 변환으로
 *
 * @test: 시험 문맥.
 *
 * 가상 머신에 장치를 넘길 때 일어나는 전환이다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_abort_to_s2(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 도착이 될 2단계 항목. */

	arm_smmu_test_make_s2_ste(&ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 도착이 될 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &abort_ste, &ste,	/* [한국어] 두 단계. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s2_to_bypass - 2단계 변환에서 우회로
 *
 * @test: 시험 문맥.
 *
 * 두 항목 모두 둘째 워드를 쓰지만 쓰는 비트가 겹쳐, 중단을 경유하지 않고도
 * 두 단계로 끝난다. 앞의 cdtable→bypass 가 세 단계였던 것과 대비된다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s2_to_bypass(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 출발이 될 2단계 항목. */

	arm_smmu_test_make_s2_ste(&ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 출발이 될 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &ste, &bypass_ste,	/* [한국어] 두 단계. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_bypass_to_s2 - 우회에서 2단계 변환으로
 *
 * @test: 시험 문맥.
 *
 * 위 시험의 역방향.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_bypass_to_s2(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 도착이 될 2단계 항목. */

	arm_smmu_test_make_s2_ste(&ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 도착이 될 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &bypass_ste, &ste,	/* [한국어] 두 단계. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s1_to_s2 - 1단계에서 2단계 변환으로 갈아타기
 *
 * @test: 시험 문맥.
 *
 * 호스트가 쓰던 장치를 가상 머신에 넘길 때 일어나는 전환이다. 두 설정이
 * 읽는 비트가 거의 겹치지 않아 중단을 경유해야 하므로 세 단계가 든다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s1_to_s2(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 출발이 될 1단계 항목. */
	struct arm_smmu_ste s2_ste;	/* [한국어] 도착이 될 2단계 항목. */

	arm_smmu_test_make_cdtable_ste(&s1_ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 1단계 쪽 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_test_make_s2_ste(&s2_ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 2단계 쪽 항목을 짓는다 — 같은 성질(ATS 켬)로 맞춰야 비교가 뜻을 갖는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s1_ste, &s2_ste,	/* [한국어] 중단을 경유하므로 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s2_to_s1 - 2단계에서 1단계 변환으로 갈아타기
 *
 * @test: 시험 문맥.
 *
 * 가상 머신에서 회수한 장치를 호스트가 다시 쓸 때의 전환. 역시 세 단계다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s2_to_s1(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 도착이 될 1단계 항목. */
	struct arm_smmu_ste s2_ste;	/* [한국어] 출발이 될 2단계 항목. */

	arm_smmu_test_make_cdtable_ste(&s1_ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 1단계 쪽 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_test_make_s2_ste(&s2_ste, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 2단계 쪽 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s2_ste, &s1_ste,	/* [한국어] 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_non_hitless - 끊김이 생길 수밖에 없는 전환
 *
 * @test: 시험 문맥.
 *
 * (아래 영어 주석 참고) 실제 운용에서는 일어나지 않는 조합이지만, 기록기가
 * "이건 끊김 없이 할 수 없다"를 옳게 판단하는지 확인해야 한다. 문맥 표 주소와
 * s1dss 를 동시에 바꾸면, 어느 순서로 써도 하드웨어가 볼 수 있는 중간 상태가
 * 생긴다. 그럴 때 기록기는 무효 항목을 한 번 거쳐 가는 안전한 길을 택하며,
 * 이 시험이 그 동작을 못 박는다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_non_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_non_hitless(struct kunit *test)
{
	struct arm_smmu_ste ste;	/* [한국어] 출발 항목. */
	struct arm_smmu_ste ste_2;	/* [한국어] 도착 항목 — 표 주소와 s1dss 가 모두 다르다. */

	/*
	 * Although no flow resembles this in practice, one way to force an STE
	 * update to be non-hitless is to change its CD table pointer as well as
	 * s1 dss field in the same update.
	 */
	arm_smmu_test_make_cdtable_ste(&ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 표 주소는 기본 가짜 값. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_ATS);
	arm_smmu_test_make_cdtable_ste(&ste_2, STRTAB_STE_1_S1DSS_BYPASS,	/* [한국어] s1dss 도 다르고. */
				       0x4B4B4b4B4B, ARM_SMMU_MASTER_TEST_ATS);	/* [한국어] 표 주소도 다르다 — 두 워드가 동시에 바뀌어야 한다. */
	arm_smmu_v3_test_ste_expect_non_hitless_transition(	/* [한국어] 무효 항목을 한 번 거쳐 세 단계로 끝나야 한다. */
		test, &ste, &ste_2, NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_test_cd_expect_transition - 한 CD 전환이 규약을 지키는지 시험한다
 *
 * @test: 시험 문맥.
 * @cur: 출발 서술자.
 * @target: 도착 서술자.
 * @num_syncs_expected: 기대 단계 수.
 * @hitless: 끊김 없이 끝나야 하는가.
 *
 * STE 쪽과 짜임이 같지만 연산표가 test_cd_ops 라는 점이 다르다. 문맥 서술자도
 * 8워드라 나눠 써야 하고, 하드웨어가 중간 상태를 보면 엉뚱한 페이지 테이블을
 * 걷는 것은 마찬가지다. 다만 CD 에는 "달라도 안전한 비트"라는 개념이 없어
 * get_update_safe 갈고리가 NULL 이고, 그래서 검사가 조금 더 엄격하다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   각 CD KUNIT_CASE → expect_hitless/non_hitless_transition() → [이 함수]
 *     → arm_smmu_write_entry()
 */
static void arm_smmu_v3_test_cd_expect_transition(
	struct kunit *test, const struct arm_smmu_cd *cur,
	const struct arm_smmu_cd *target, unsigned int num_syncs_expected,
	bool hitless)
{
	struct arm_smmu_cd cur_copy = *cur;	/* [한국어] 원본을 건드리지 않도록 복사한다. */
	struct arm_smmu_test_writer test_writer = {	/* [한국어] 시험용 기록기. */
		.writer = {	/* [한국어] CD 용 연산표로 초기화한다 — get_update_safe 가 없는 쪽이다. */
			.ops = &test_cd_ops,	/* [한국어] CD 용 연산표 — get_update_safe 가 없다. */
		},
		.test = test,
		.init_entry = cur->data,	/* [한국어] 출발점. */
		.target_entry = target->data,	/* [한국어] 도착점. */
		.entry = cur_copy.data,	/* [한국어] 실제로 고쳐질 복사본. */
		.num_syncs = 0,
		.invalid_entry_written = false,

	};

	pr_debug("CD initial value: ");	/* [한국어] 실패 추적용 흔적. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8, cur_copy.data,	/* [한국어] 출발 서술자를 16진수로 찍는다. */
			     sizeof(cur_copy), false);
	arm_smmu_v3_test_debug_print_used_bits(&test_writer.writer, cur->data);	/* [한국어] 출발 서술자가 읽는 비트도 함께 찍는다. */
	pr_debug("CD target value: ");	/* [한국어] 이어서 도착 서술자를 찍는다는 머리말. */
	print_hex_dump_debug("    ", DUMP_PREFIX_NONE, 16, 8, target->data,	/* [한국어] 도착 서술자를 16진수로 찍는다. */
			     sizeof(cur_copy), false);
	arm_smmu_v3_test_debug_print_used_bits(&test_writer.writer,	/* [한국어] 도착 서술자가 읽는 비트 — 출발과의 차이가 전환 단계 수를 결정한다. */
					       target->data);

	arm_smmu_write_entry(&test_writer.writer, cur_copy.data, target->data);	/* [한국어] 검증 대상 — STE 와 같은 함수를 쓴다. 항목 종류는 연산표가 가른다. */

	KUNIT_EXPECT_EQ(test, test_writer.invalid_entry_written, !hitless);	/* [한국어] 무효 상태를 거쳤는지가 기대와 맞아야 한다. */
	KUNIT_EXPECT_EQ(test, test_writer.num_syncs, num_syncs_expected);	/* [한국어] 단계 수가 정확해야 한다. */
	KUNIT_EXPECT_MEMEQ(test, target->data, cur_copy.data, sizeof(cur_copy));	/* [한국어] 최종 값이 도착 서술자와 완전히 같아야 한다. */
}

/*
 * [한국어]
 * arm_smmu_v3_test_cd_expect_non_hitless_transition - 끊김이 있어야 하는 CD 전환
 *
 * @test: 시험 문맥.
 * @cur: 출발 서술자.
 * @target: 도착 서술자.
 * @num_syncs_expected: 기대 단계 수.
 *
 * 서술자를 처음 만들거나 완전히 지우는 전환은 원리상 끊김이 있다 —
 * 유효하지 않은 서술자를 반드시 거치기 때문이다. 그 경우를 위한 껍데기다.
 *
 * 호출 체인:
 *   arm_smmu_v3_write_cd_test_*_clear() → [이 함수]
 */
static void arm_smmu_v3_test_cd_expect_non_hitless_transition(
	struct kunit *test, const struct arm_smmu_cd *cur,
	const struct arm_smmu_cd *target, unsigned int num_syncs_expected)
{
	arm_smmu_v3_test_cd_expect_transition(test, cur, target,	/* [한국어] 끊김을 기대하는 전환. */
					      num_syncs_expected, false);
}

/*
 * [한국어]
 * arm_smmu_v3_test_cd_expect_hitless_transition - 끊김 없어야 하는 CD 전환
 *
 * @test: 시험 문맥.
 * @cur: 출발 서술자.
 * @target: 도착 서술자.
 * @num_syncs_expected: 기대 단계 수.
 *
 * 이미 유효한 서술자를 다른 유효한 서술자로 바꾸는 경우다. 장치가 계속 DMA 를
 * 내고 있는 중일 수 있어 끊김이 없어야 한다.
 *
 * 호출 체인:
 *   arm_smmu_v3_write_cd_test_s1_change_asid()/sva_release() → [이 함수]
 */
static void arm_smmu_v3_test_cd_expect_hitless_transition(
	struct kunit *test, const struct arm_smmu_cd *cur,
	const struct arm_smmu_cd *target, unsigned int num_syncs_expected)
{
	arm_smmu_v3_test_cd_expect_transition(test, cur, target,	/* [한국어] 끊김을 허용하지 않는 전환. */
					      num_syncs_expected, true);
}

/*
 * [한국어]
 * arm_smmu_test_make_s1_cd - 1단계 변환 문맥 서술자를 가짜 도메인으로 짓는다
 *
 * @cd: 지어 담을 자리.
 * @asid: 이 서술자에 넣을 주소 공간 번호.
 *
 * 페이지 테이블 설정을 눈에 띄는 가짜 값으로 채운 뒤 실제 드라이버 함수로
 * 서술자를 짓는다. ASID 를 인자로 받는 이유는, ASID 만 다른 두 서술자 사이의
 * 전환이 한 번의 쓰기로 끝나는지 시험해야 하기 때문이다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   CD KUNIT_CASE → [이 함수] → arm_smmu_make_s1_cd()
 */
static void arm_smmu_test_make_s1_cd(struct arm_smmu_cd *cd, unsigned int asid)
{
	struct arm_smmu_master master = {	/* [한국어] 가짜 장치 — 서술자를 짓는 데는 SMMU 능력 비트만 있으면 된다. */
		.smmu = &smmu,	/* [한국어] 서술자를 짓는 데 필요한 능력 비트를 가짜 하드웨어에서 읽는다. */
	};
	struct io_pgtable io_pgtable = {};	/* [한국어] 가짜 페이지 테이블 설정. */
	struct arm_smmu_domain smmu_domain = {	/* [한국어] 가짜 도메인. */
		.pgtbl_ops = &io_pgtable.ops,	/* [한국어] 설정 구조체로 되짚어 갈 수 있게 한다. */
		.cd = {
			.asid = asid,	/* [한국어] 호출자가 준 번호 — 서술자에 그대로 실린다. */
		},
	};

	io_pgtable.cfg.arm_lpae_s1_cfg.ttbr = 0xdaedbeefdeadbeefULL;	/* [한국어] 1단계 페이지 테이블의 뿌리 주소. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.ips = 1;	/* [한국어] 아래 값들은 모두 서로 달라, 필드를 섞어 담는 실수를 잡아낸다. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.tg = 2;	/* [한국어] 알갱이 크기 코드. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.sh = 3;	/* [한국어] 공유 영역 코드. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.orgn = 1;	/* [한국어] 바깥 캐시 정책. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.irgn = 2;	/* [한국어] 안쪽 캐시 정책. */
	io_pgtable.cfg.arm_lpae_s1_cfg.tcr.tsz = 4;	/* [한국어] 입력 주소 폭. */
	io_pgtable.cfg.arm_lpae_s1_cfg.mair = 0xabcdef012345678ULL;	/* [한국어] 메모리 속성 표 — 서술자의 넷째 워드에 그대로 실린다. */

	arm_smmu_make_s1_cd(cd, &master, &smmu_domain);	/* [한국어] 검증 대상인 실제 드라이버 함수. */
}

/*
 * [한국어]
 * arm_smmu_v3_write_cd_test_s1_clear - 빈 서술자와 1단계 서술자 사이의 전환
 *
 * @test: 시험 문맥.
 *
 * PASID 를 새로 달고 떼는 경로가 이 전환을 쓴다. 빈 서술자(전부 0)는 유효
 * 비트가 꺼져 있으므로 무효 상태를 반드시 거치게 되고, 따라서 끊김이 있다.
 * 양방향을 모두 확인한다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_cd_expect_non_hitless_transition() x2
 */
static void arm_smmu_v3_write_cd_test_s1_clear(struct kunit *test)
{
	struct arm_smmu_cd cd = {};	/* [한국어] 전부 0 인 빈 서술자 — "이 PASID 는 쓰지 않는다"는 뜻이다. */
	struct arm_smmu_cd cd_2;	/* [한국어] 실제 1단계 서술자. */

	arm_smmu_test_make_s1_cd(&cd_2, 1997);	/* [한국어] 임의의 ASID 로 서술자를 짓는다. */
	arm_smmu_v3_test_cd_expect_non_hitless_transition(	/* [한국어] 빈 것에서 유효한 것으로 — 끊김 있음, 두 단계. */
		test, &cd, &cd_2, NUM_EXPECTED_SYNCS(2));
	arm_smmu_v3_test_cd_expect_non_hitless_transition(	/* [한국어] 유효한 것에서 빈 것으로 — 마찬가지. */
		test, &cd_2, &cd, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_cd_test_s1_change_asid - ASID 만 바꾸는 전환
 *
 * @test: 시험 문맥.
 *
 * ASID 는 서술자의 첫 워드 안에 있으므로 한 번의 쓰기로 바꿀 수 있어야 한다.
 * 도메인이 ASID 를 다시 배정받는 드문 경로에서 실제로 쓰이며, 그때 장치는
 * 계속 DMA 를 내고 있으므로 끊김이 없어야 한다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_cd_expect_hitless_transition() x2
 */
static void arm_smmu_v3_write_cd_test_s1_change_asid(struct kunit *test)
{
	struct arm_smmu_cd cd = {};	/* [한국어] 아래에서 곧바로 덮어쓴다 — 초기화 경고를 피하려는 형태다. */
	struct arm_smmu_cd cd_2;	/* [한국어] 다른 ASID 를 가진 서술자. */

	arm_smmu_test_make_s1_cd(&cd, 778);	/* [한국어] 첫 번째 ASID. */
	arm_smmu_test_make_s1_cd(&cd_2, 1997);	/* [한국어] 두 번째 ASID — 나머지 설정은 완전히 같다. */
	arm_smmu_v3_test_cd_expect_hitless_transition(test, &cd, &cd_2,	/* [한국어] 한 번의 쓰기로 끝나야 한다. */
						      NUM_EXPECTED_SYNCS(1));
	arm_smmu_v3_test_cd_expect_hitless_transition(test, &cd_2, &cd,	/* [한국어] 되돌아오는 방향도 마찬가지. */
						      NUM_EXPECTED_SYNCS(1));
}

/*
 * [한국어]
 * arm_smmu_test_make_sva_cd - SVA 문맥 서술자를 가짜 mm 으로 짓는다
 *
 * @cd: 지어 담을 자리.
 * @asid: 그 주소 공간의 번호.
 *
 * arm-smmu-v3-sva.c 의 서술자 짓기를 시험에서 그대로 부른다. 가짜 mm 의
 * pgd 값이 서술자의 TTB0 에 실리는지가 관심사이며, 실제로 그 주소에
 * 접근하지는 않는다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   SVA CD KUNIT_CASE → [이 함수] → arm_smmu_make_sva_cd()
 */
static void arm_smmu_test_make_sva_cd(struct arm_smmu_cd *cd, unsigned int asid)
{
	struct arm_smmu_master master = {	/* [한국어] 가짜 장치. 멈춤을 쓰지 않는 설정으로 둔다. */
		.smmu = &smmu,	/* [한국어] SVA 서술자도 하드웨어 능력에 따라 값이 달라져 이 포인터가 필요하다. */
	};

	arm_smmu_make_sva_cd(cd, &master, &sva_mm, asid);	/* [한국어] 가짜 프로세스의 pgd 를 꽂은 서술자를 짓는다. */
}

/*
 * [한국어]
 * arm_smmu_test_make_sva_release_cd - "모두 폴트" SVA 서술자를 짓는다
 *
 * @cd: 지어 담을 자리.
 * @asid: 그 주소 공간의 번호.
 *
 * mm 을 NULL 로 넘겨, 프로세스가 죽은 뒤 쓰이는 변형을 만든다. 이 서술자는
 * 유효하되 TTB0 를 읽지 않으므로 모든 접근이 폴트가 된다. 실제 SVA 해제
 * 경로가 이 서술자로 갈아 끼우며, 그 전환이 끊김 없어야 죽어 가는 프로세스의
 * 장치가 안전하게 멈춘다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_v3_write_cd_test_sva_release() → [이 함수] → arm_smmu_make_sva_cd()
 */
static void arm_smmu_test_make_sva_release_cd(struct arm_smmu_cd *cd,
					      unsigned int asid)
{
	struct arm_smmu_master master = {	/* [한국어] 가짜 장치. */
		.smmu = &smmu,	/* [한국어] "모두 폴트" 서술자도 같은 가짜 하드웨어를 쓴다. */
	};

	arm_smmu_make_sva_cd(cd, &master, NULL, asid);	/* [한국어] mm 을 NULL 로 넘기는 것이 요점 — EPD0 가 켜진 서술자가 나온다. */
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s1_to_s2_stall - 멈춤을 쓰는 장치의 1→2단계 전환
 *
 * @test: 시험 문맥.
 *
 * 멈춤을 켜면 STE 의 S1STALLD 비트가 사용 비트에 더해져 전환 계산이 달라진다.
 * ATS 를 켠 경우와 같은 세 단계로 끝나는지 확인해, 멈춤 지원이 전환 비용을
 * 늘리지 않는다는 것을 못 박는다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s1_to_s2_stall(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 출발이 될 1단계 항목 (멈춤 켬). */
	struct arm_smmu_ste s2_ste;	/* [한국어] 도착이 될 2단계 항목 (멈춤 켬). */

	arm_smmu_test_make_cdtable_ste(&s1_ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 멈춤을 켠 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_STALL);	/* [한국어] ATS 대신 멈춤을 켠 장치. */
	arm_smmu_test_make_s2_ste(&s2_ste, ARM_SMMU_MASTER_TEST_STALL);	/* [한국어] 멈춤을 켠 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s1_ste, &s2_ste,	/* [한국어] 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_s2_to_s1_stall - 멈춤을 쓰는 장치의 2→1단계 전환
 *
 * @test: 시험 문맥.
 *
 * 위 시험의 역방향.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void arm_smmu_v3_write_ste_test_s2_to_s1_stall(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 도착이 될 1단계 항목. */
	struct arm_smmu_ste s2_ste;	/* [한국어] 출발이 될 2단계 항목. */

	arm_smmu_test_make_cdtable_ste(&s1_ste, STRTAB_STE_1_S1DSS_SSID0,	/* [한국어] 멈춤을 켠 1단계 항목을 짓는다. */
				       fake_cdtab_dma_addr, ARM_SMMU_MASTER_TEST_STALL);
	arm_smmu_test_make_s2_ste(&s2_ste, ARM_SMMU_MASTER_TEST_STALL);	/* [한국어] 멈춤을 켠 2단계 항목을 짓는다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s2_ste, &s1_ste,	/* [한국어] 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_nested_s1dssbypass_to_s1bypass - 중첩에서 순수 2단계로
 *
 * @test: 시험 문맥.
 *
 * 게스트가 자기 문맥 표를 쓰던 상태에서, 그 표를 떼고 호스트의 2단계만 남기는
 * 전환이다. 중첩 항목에는 EATS 와 MEV 처럼 순수 2단계 항목에서는 읽히지 않는
 * 비트가 켜져 있어, 그것들을 정리하는 단계가 하나 더 든다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void
arm_smmu_v3_write_ste_test_nested_s1dssbypass_to_s1bypass(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 출발이 될 중첩 항목. */
	struct arm_smmu_ste s2_ste;	/* [한국어] 도착이 될 순수 2단계 항목. */

	arm_smmu_test_make_cdtable_ste(	/* [한국어] 중첩 플래그를 함께 넘겨 NESTED 갈래 항목을 만든다. */
		&s1_ste, STRTAB_STE_1_S1DSS_BYPASS, fake_cdtab_dma_addr,
		ARM_SMMU_MASTER_TEST_ATS | ARM_SMMU_MASTER_TEST_NESTED);
	arm_smmu_test_make_s2_ste(&s2_ste, 0);	/* [한국어] 아무 성질도 없는 순수 2단계 항목 — ATS 도 꺼져 있다. */
	/* Expect an additional sync to unset ignored bits: EATS and MEV */
	/* [한국어] (위 영어 주석 참고) 중첩에서만 뜻이 있던 두 비트를 지우는 단계가
	 * 하나 더 필요하다. 그래서 보통 두 단계인 전환이 세 단계가 된다. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s1_ste, &s2_ste,	/* [한국어] 세 단계. */
						       NUM_EXPECTED_SYNCS(3));
}

/*
 * [한국어]
 * arm_smmu_v3_write_ste_test_nested_s1bypass_to_s1dssbypass - 순수 2단계에서 중첩으로
 *
 * @test: 시험 문맥.
 *
 * 위 시험의 역방향인데 단계 수가 다르다 — 두 단계다. 켜는 방향은 새 비트를
 * 채워 넣기만 하면 되고, 그 비트들은 아직 읽히지 않는 상태에서 미리 써 둘 수
 * 있기 때문이다. 지우는 방향은 이미 읽히던 비트를 건드려야 해서 더 조심스럽다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_ste_expect_hitless_transition()
 */
static void
arm_smmu_v3_write_ste_test_nested_s1bypass_to_s1dssbypass(struct kunit *test)
{
	struct arm_smmu_ste s1_ste;	/* [한국어] 도착이 될 중첩 항목. */
	struct arm_smmu_ste s2_ste;	/* [한국어] 출발이 될 순수 2단계 항목. */

	arm_smmu_test_make_cdtable_ste(	/* [한국어] 중첩 갈래의 항목을 도착점으로 짓는다. */
		&s1_ste, STRTAB_STE_1_S1DSS_BYPASS, fake_cdtab_dma_addr,
		ARM_SMMU_MASTER_TEST_ATS | ARM_SMMU_MASTER_TEST_NESTED);
	arm_smmu_test_make_s2_ste(&s2_ste, 0);	/* [한국어] 아무 성질도 없는 순수 2단계 항목. */
	arm_smmu_v3_test_ste_expect_hitless_transition(test, &s2_ste, &s1_ste,	/* [한국어] 두 단계 — 지우는 방향보다 한 단계 적다. */
						       NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_cd_test_sva_clear - 빈 서술자와 SVA 서술자 사이의 전환
 *
 * @test: 시험 문맥.
 *
 * SVA 로 프로세스를 묶고 푸는 경로가 쓰는 전환이다. 빈 서술자를 거치므로
 * 끊김이 있으며, 그 시점에는 아직 장치가 그 PASID 를 쓰지 않으므로 문제가
 * 되지 않는다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_cd_expect_non_hitless_transition() x2
 */
static void arm_smmu_v3_write_cd_test_sva_clear(struct kunit *test)
{
	struct arm_smmu_cd cd = {};	/* [한국어] 전부 0 인 빈 서술자. */
	struct arm_smmu_cd cd_2;	/* [한국어] SVA 서술자. */

	arm_smmu_test_make_sva_cd(&cd_2, 1997);	/* [한국어] 가짜 mm 을 가리키는 서술자를 짓는다. */
	arm_smmu_v3_test_cd_expect_non_hitless_transition(	/* [한국어] 빈 것 → SVA, 끊김 있음, 두 단계. */
		test, &cd, &cd_2, NUM_EXPECTED_SYNCS(2));
	arm_smmu_v3_test_cd_expect_non_hitless_transition(	/* [한국어] SVA → 빈 것, 마찬가지. */
		test, &cd_2, &cd, NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_write_cd_test_sva_release - SVA 서술자와 "모두 폴트" 변형 사이
 *
 * @test: 시험 문맥.
 *
 * 이 시험이 지키는 성질이 SVA 의 안전을 떠받친다. 프로세스가 죽는 순간에도
 * 장치는 DMA 를 내고 있을 수 있으므로, 정상 서술자에서 "모두 폴트" 서술자로
 * 가는 길에 무효 상태가 있어서는 안 된다. 무효 서술자를 하드웨어가 읽으면
 * C_BAD_CD 오류가 쏟아지기 때문이다. EPD0 설계가 그 끊김 없음을 가능하게
 * 하며, 이 시험이 그것을 확인한다.
 *
 * 호출 체인:
 *   KUnit → [이 함수] → arm_smmu_v3_test_cd_expect_hitless_transition() x2
 */
static void arm_smmu_v3_write_cd_test_sva_release(struct kunit *test)
{
	struct arm_smmu_cd cd;	/* [한국어] 정상 SVA 서술자. */
	struct arm_smmu_cd cd_2;	/* [한국어] "모두 폴트" 변형. */

	arm_smmu_test_make_sva_cd(&cd, 1997);	/* [한국어] mm 을 가리키는 정상 서술자. */
	arm_smmu_test_make_sva_release_cd(&cd_2, 1997);	/* [한국어] 같은 ASID 의 "모두 폴트" 서술자 — ASID 를 유지해야 TLB 를 지울 수 있다. */
	arm_smmu_v3_test_cd_expect_hitless_transition(test, &cd, &cd_2,	/* [한국어] 프로세스가 죽을 때의 방향 — 끊김이 없어야 한다. */
						      NUM_EXPECTED_SYNCS(2));
	arm_smmu_v3_test_cd_expect_hitless_transition(test, &cd_2, &cd,	/* [한국어] 되돌아오는 방향도 확인한다. */
						      NUM_EXPECTED_SYNCS(2));
}

/*
 * [한국어]
 * arm_smmu_v3_invs_test_verify - 무효화 배열의 내용을 기대 표와 대조한다
 *
 * @test: 시험 문맥.
 * @invs: 검사할 배열.
 * @num_invs: 기대하는 유효 항목 수.
 * @num_trashes: 기대하는 "버려진 자리" 수 (참조 계수가 0 이 된 항목).
 * @ids: 각 항목의 기대 id 배열.
 * @users: 각 항목의 기대 참조 수 배열.
 * @ssids: 각 항목의 기대 PASID 배열.
 *
 * 배열 연산 시험의 판정 도구다. 항목 하나하나의 id·참조 수·PASID 를 모두
 * 대조하므로, 병합이 순서를 뒤섞거나 참조 수를 잘못 세면 바로 드러난다.
 *
 * 뒤에서부터 훑는 것(num_invs-- 를 조건에 쓴 것)은 단순히 짧게 쓰기 위한
 * 형태이며, 순서 자체에 뜻은 없다.
 *
 * 실행 컨텍스트: 시험 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_v3_invs_test() → [이 함수]
 */
static void arm_smmu_v3_invs_test_verify(struct kunit *test,
					 struct arm_smmu_invs *invs,
					 int num_invs, const int num_trashes,
					 const int *ids, const int *users,
					 const int *ssids)
{
	KUNIT_EXPECT_EQ(test, invs->num_invs, num_invs);	/* [한국어] 유효 항목 수가 기대와 같아야 한다. */
	KUNIT_EXPECT_EQ(test, invs->num_trashes, num_trashes);	/* [한국어] 참조가 0 이 되어 자리만 남은 항목의 수도 확인한다 — 이 값이 커지면 정리(purge)가 필요해진다. */
	while (num_invs--) {	/* [한국어] 마지막 항목부터 거꾸로 훑는다. */
		KUNIT_EXPECT_EQ(test, invs->inv[num_invs].id, ids[num_invs]);	/* [한국어] 항목이 가리키는 대상 번호 (VMID/ASID/스트림 번호). */
		KUNIT_EXPECT_EQ(test, READ_ONCE(invs->inv[num_invs].users),	/* [한국어] 참조 수는 다른 CPU 가 동시에 고칠 수 있는 자리라 READ_ONCE 로 읽는다 — 시험에서는 경합이 없지만 규약을 지킨다. */
				users[num_invs]);
		KUNIT_EXPECT_EQ(test, invs->inv[num_invs].ssid, ssids[num_invs]);	/* [한국어] PASID 별 항목을 구분하는 값. */
	}
}

/* [한국어] 시험에 쓸 무효화 항목 묶음 1 — 서로 다른 종류가 섞인 기본 묶음.
 *
 * 2단계 VMID 무효화, 그 아래 1단계를 함께 지우는 변형, 그리고 장치 캐시
 * 무효화 하나. 실제 도메인 하나에 장치가 붙었을 때 만들어지는 모양이다. */
static struct arm_smmu_invs invs1 = {
	.num_invs = 3,	/* [한국어] 항목 세 개. */
	.inv = { { .type = INV_TYPE_S2_VMID, .id = 1, },	/* [한국어] VMID 1 의 2단계 변환을 지우는 항목. */
		 { .type = INV_TYPE_S2_VMID_S1_CLEAR, .id = 1, },	/* [한국어] 같은 VMID 아래의 1단계 변환까지 지우는 항목 — 종류가 다르므로 별개로 센다. */
		 { .type = INV_TYPE_ATS, .id = 3, }, },	/* [한국어] 스트림 3 의 장치 캐시를 지우는 항목. */
};

/* [한국어] 묶음 2 — 묶음 1 과 하나가 겹치고 둘은 새롭다.
 *
 * 겹치는 항목은 병합 때 참조 수만 올라가야 하고, 새 항목은 자리를 얻어야 한다.
 * 그 두 동작을 한 번에 시험하려고 이렇게 짰다. */
static struct arm_smmu_invs invs2 = {
	.num_invs = 3,	/* [한국어] 항목 세 개. */
	.inv = { { .type = INV_TYPE_S2_VMID, .id = 1, }, /* duplicated */	/* [한국어] 묶음 1 과 완전히 같은 항목 — 참조 수만 2 로 올라야 한다. */
		 { .type = INV_TYPE_ATS, .id = 4, },	/* [한국어] 새 장치. */
		 { .type = INV_TYPE_ATS, .id = 5, }, },	/* [한국어] 또 다른 새 장치. */
};

/* [한국어] 묶음 3 — 겹치는 항목 하나, 이미 버려진 자리를 되살리는 항목 하나, 새 항목 하나.
 *
 * 참조가 0 이 되어 자리만 남은 항목을 다시 참조하면 새 자리를 만들지 않고
 * 그 자리를 되살려야 한다. 그 최적화를 시험하는 것이 이 묶음의 목적이다. */
static struct arm_smmu_invs invs3 = {
	.num_invs = 3,	/* [한국어] 항목 세 개. */
	.inv = { { .type = INV_TYPE_S2_VMID, .id = 1, }, /* duplicated */	/* [한국어] 이미 있는 항목. */
		 { .type = INV_TYPE_ATS, .id = 5, }, /* recover a trash */	/* [한국어] 앞서 참조가 0 이 된 자리를 되살린다. */
		 { .type = INV_TYPE_ATS, .id = 6, }, },	/* [한국어] 새 항목. */
};

/* [한국어] 묶음 4 — 같은 장치의 여러 PASID 를 다루는 묶음.
 *
 * 스트림 번호가 같아도 PASID 가 다르면 별개의 무효화 대상이다. 정렬과 중복
 * 판정이 PASID 까지 고려하는지 시험한다. */
static struct arm_smmu_invs invs4 = {
	.num_invs = 3,	/* [한국어] 항목 세 개. */
	.inv = { { .type = INV_TYPE_ATS, .id = 10, .ssid = 1 },	/* [한국어] 장치 10 의 PASID 1. */
		 { .type = INV_TYPE_ATS, .id = 10, .ssid = 3 },	/* [한국어] 같은 장치의 PASID 3 — 별개 항목이다. */
		 { .type = INV_TYPE_ATS, .id = 12, .ssid = 1 }, },	/* [한국어] 다른 장치의 PASID 1. */
};

/* [한국어] 묶음 5 — 묶음 4 와 PASID 가 엇갈리게 겹치는 묶음.
 *
 * 병합 결과에서 항목이 (id, ssid) 순으로 정렬되어야 하고, 정확히 겹치는
 * 하나만 참조 수가 올라야 한다. 정렬 삽입의 정확성을 시험한다. */
static struct arm_smmu_invs invs5 = {
	.num_invs = 3,	/* [한국어] 항목 세 개. */
	.inv = { { .type = INV_TYPE_ATS, .id = 10, .ssid = 2 },	/* [한국어] 장치 10 의 PASID 2 — 새 항목이며 1 과 3 사이에 끼어들어야 한다. */
		 { .type = INV_TYPE_ATS, .id = 10, .ssid = 3 }, /* duplicate */	/* [한국어] 이미 있는 항목 — 참조 수만 오른다. */
		 { .type = INV_TYPE_ATS, .id = 12, .ssid = 2 }, },	/* [한국어] 장치 12 의 새 PASID. */
};

/*
 * [한국어]
 * arm_smmu_v3_invs_test - 무효화 배열의 병합·해제·정리를 11단계로 시험한다
 *
 * @test: 시험 문맥.
 *
 * 도메인의 무효화 배열은 장치가 붙고 떨어질 때마다 바뀌는데, 그 계산이
 * 틀리면 무효화가 빠지거나(정확성 문제) 쓸데없이 늘어난다(성능 문제).
 * 실제 하드웨어에서는 아주 드문 순서로만 드러나므로, 여기서 순서를
 * 인위적으로 만들어 매 단계의 배열 내용을 표와 대조한다.
 *
 * 세 연산의 성질이 다르다는 점이 이 시험의 뼈대다.
 * - merge: 실패할 수 있으므로 새 배열을 만들어 돌려준다. 옛 배열은 호출자가 놓는다.
 * - unref: 실패하면 안 되는 자리에서 쓰이므로 배열을 제자리에서 고친다.
 *   참조가 0 이 된 항목은 지우지 않고 "버려진 자리"로 표시만 한다 —
 *   지우려면 배열을 옮겨야 하고, 그 사이 무효화가 항목을 놓칠 수 있기 때문이다.
 * - purge: 버려진 자리를 실제로 걷어 낸 새 배열을 만든다.
 *
 * 각 단계의 기대값이 results1~results10 표에 세 줄(id, 참조 수, PASID)로
 * 적혀 있어, 표만 봐도 배열이 어떻게 변해 가는지 따라갈 수 있다.
 *
 * 실행 컨텍스트: 시험 문맥. 메모리를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   KUnit → [이 함수]
 *     → arm_smmu_invs_alloc()/merge()/unref()/purge()
 *     → arm_smmu_v3_invs_test_verify()
 */
static void arm_smmu_v3_invs_test(struct kunit *test)
{
	const int results1[3][3] = { { 1, 1, 3, }, { 1, 1, 1, }, { 0, 0, 0, } };	/* [한국어] 각 표는 [0]=id, [1]=참조 수, [2]=PASID 세 줄이다. 빈 배열에 묶음 1 을 넣은 결과. */
	const int results2[3][5] = { { 1, 1, 3, 4, 5, }, { 2, 1, 1, 1, 1, }, { 0, 0, 0, 0, 0, } };	/* [한국어] 묶음 2 를 더한 결과 — 겹친 첫 항목의 참조가 2 로 올랐다. */
	const int results3[3][3] = { { 1, 1, 3, }, { 1, 1, 1, }, { 0, 0, 0, } };	/* [한국어] 묶음 2 를 걷어 낸 결과 — 묶음 1 만 남아 처음으로 돌아왔다. */
	const int results4[3][5] = { { 1, 1, 3, 5, 6, }, { 2, 1, 1, 1, 1, }, { 0, 0, 0, 0, 0, } };	/* [한국어] 묶음 3 을 더한 결과. */
	const int results5[3][5] = { { 1, 1, 3, 5, 6, }, { 1, 0, 0, 1, 1, }, { 0, 0, 0, 0, 0, } };	/* [한국어] 묶음 1 을 걷어 낸 결과 — 참조가 0 이 된 자리가 둘 생겼지만 배열에는 그대로 남아 있다. */
	const int results6[3][3] = { { 1, 5, 6, }, { 1, 1, 1, }, { 0, 0, 0, } };	/* [한국어] 정리한 결과 — 참조 0 인 자리가 실제로 사라졌다. */
	const int results7[3][3] = { { 10, 10, 12, }, { 1, 1, 1, }, { 1, 3, 1, } };	/* [한국어] 빈 배열에 묶음 4 를 넣은 결과 — PASID 가 다른 항목들이다. */
	const int results8[3][5] = { { 10, 10, 10, 12, 12, }, { 1, 1, 2, 1, 1, }, { 1, 2, 3, 1, 2, } };	/* [한국어] 묶음 5 를 더한 결과 — PASID 2 가 1 과 3 사이에 정확히 끼어들었다. */
	const int results9[3][4] = { { 10, 10, 10, 12, }, { 1, 0, 1, 1, }, { 1, 2, 3, 1, } };	/* [한국어] 묶음 5 를 걷어 낸 결과 — 참조가 0 이 된 자리 하나가 남았다. */
	const int results10[3][3] = { { 10, 10, 12, }, { 1, 1, 1, }, { 1, 3, 1, } };	/* [한국어] 정리 후 — 묶음 4 만 남아 처음으로 돌아왔다. */
	struct arm_smmu_invs *test_a, *test_b;	/* [한국어] 두 배열을 번갈아 쓴다 — merge/purge 가 새 배열을 만들기 때문이다. */

	/* New array */
	/* [한국어] (위 영어 주석 참고) 빈 배열에서 시작한다 — 도메인을 막 만들었을 때의 상태다. */
	test_a = arm_smmu_invs_alloc(0);	/* [한국어] 항목 0 개짜리 배열. */
	KUNIT_EXPECT_EQ(test, test_a->num_invs, 0);	/* [한국어] 정말 비어 있는지 확인한다. */

	/* Test1: merge invs1 (new array) */
	/* [한국어] 빈 배열에 첫 묶음을 더한다 — 장치를 처음 붙일 때의 동작이다. */
	test_b = arm_smmu_invs_merge(test_a, &invs1);	/* [한국어] 새 배열을 만들어 돌려준다. */
	kfree(test_a);	/* [한국어] 옛 배열은 호출자가 놓는다 — 실제 코드에서는 RCU 유예를 거치지만 시험에서는 아무도 보고 있지 않다. */
	arm_smmu_v3_invs_test_verify(test, test_b, ARRAY_SIZE(results1[0]), 0,	/* [한국어] 항목 세 개, 버려진 자리 0. */
				     results1[0], results1[1], results1[2]);

	/* Test2: merge invs2 (new array) */
	/* [한국어] 겹치는 항목이 있는 묶음을 더한다 — 같은 도메인에 장치를 하나 더 붙이는 상황. */
	test_a = arm_smmu_invs_merge(test_b, &invs2);	/* [한국어] 겹친 항목은 참조 수만 오르고, 새 항목은 자리를 얻는다. */
	kfree(test_b);	/* [한국어] 옛 배열을 놓는다. */
	arm_smmu_v3_invs_test_verify(test, test_a, ARRAY_SIZE(results2[0]), 0,	/* [한국어] 다섯 항목으로 늘고, 첫 항목의 참조가 2 다. */
				     results2[0], results2[1], results2[2]);

	/* Test3: unref invs2 (same array) */
	/* [한국어] 방금 더한 묶음을 도로 걷어 낸다 — 장치를 떼는 상황이다. */
	arm_smmu_invs_unref(test_a, &invs2);	/* [한국어] 제자리에서 고친다 — 새 배열을 만들지 않으므로 실패할 수 없다. */
	arm_smmu_v3_invs_test_verify(test, test_a, ARRAY_SIZE(results3[0]), 0,	/* [한국어] 참조가 0 이 된 항목이 바로 걷혀 세 개로 돌아왔다 — 배열 끝쪽이라 자를 수 있었다. */
				     results3[0], results3[1], results3[2]);

	/* Test4: merge invs3 (new array) */
	/* [한국어] 이번에는 버려진 자리를 되살리는 항목이 섞인 묶음을 더한다. */
	test_b = arm_smmu_invs_merge(test_a, &invs3);
	kfree(test_a);	/* [한국어] 옛 배열을 놓는다 — 병합이 새 배열을 돌려주므로 이전 것은 쓸모가 없다. */
	arm_smmu_v3_invs_test_verify(test, test_b, ARRAY_SIZE(results4[0]), 0,	/* [한국어] 다섯 항목. */
				     results4[0], results4[1], results4[2]);

	/* Test5: unref invs1 (same array) */
	/* [한국어] 배열 가운데 있는 항목들을 걷어 낸다 — 이번에는 자를 수 없어 자리가 남는다. */
	arm_smmu_invs_unref(test_b, &invs1);
	arm_smmu_v3_invs_test_verify(test, test_b, ARRAY_SIZE(results5[0]), 2,	/* [한국어] 항목 수는 그대로지만 버려진 자리가 둘이다 — 참조 0 인 항목은 무효화 때 건너뛰어진다. */
				     results5[0], results5[1], results5[2]);

	/* Test6: purge test_b (new array) */
	/* [한국어] 버려진 자리가 쌓이면 무효화 때 헛도는 비용이 커진다. 그때 정리한다. */
	test_a = arm_smmu_invs_purge(test_b);	/* [한국어] 참조 0 인 자리를 걷어 낸 새 배열을 만든다. */
	kfree(test_b);	/* [한국어] 옛 배열을 놓는다. */
	arm_smmu_v3_invs_test_verify(test, test_a, ARRAY_SIZE(results6[0]), 0,	/* [한국어] 세 항목만 남고 버려진 자리는 0 이다. */
				     results6[0], results6[1], results6[2]);

	/* Test7: unref invs3 (same array) */
	/* [한국어] 남은 것을 모두 걷어 낸다 — 마지막 장치를 떼는 상황이다. */
	arm_smmu_invs_unref(test_a, &invs3);
	KUNIT_EXPECT_EQ(test, test_a->num_invs, 0);	/* [한국어] 배열이 완전히 비었다. */
	KUNIT_EXPECT_EQ(test, test_a->num_trashes, 0);	/* [한국어] 버려진 자리도 남지 않았다 — 끝에서부터 걷혔기 때문이다. */

	/* Test8: merge invs4 (new array) */
	/* [한국어] 여기부터는 PASID 가 있는 항목들을 시험한다. */
	test_b = arm_smmu_invs_merge(test_a, &invs4);
	kfree(test_a);	/* [한국어] 옛 배열을 놓는다. */
	arm_smmu_v3_invs_test_verify(test, test_b, ARRAY_SIZE(results7[0]), 0,	/* [한국어] 같은 장치의 다른 PASID 가 별개 항목으로 들어갔다. */
				     results7[0], results7[1], results7[2]);

	/* Test9: merge invs5 (new array) */
	/* [한국어] PASID 가 엇갈리게 겹치는 묶음을 더해 정렬 삽입을 시험한다. */
	test_a = arm_smmu_invs_merge(test_b, &invs5);
	kfree(test_b);	/* [한국어] 옛 배열을 놓는다. */
	arm_smmu_v3_invs_test_verify(test, test_a, ARRAY_SIZE(results8[0]), 0,	/* [한국어] PASID 2 가 1 과 3 사이에 정확히 끼어들었는지가 관심사다. */
				     results8[0], results8[1], results8[2]);

	/* Test10: unref invs5 (same array) */
	/* [한국어] 그 묶음을 걷어 낸다. */
	arm_smmu_invs_unref(test_a, &invs5);
	arm_smmu_v3_invs_test_verify(test, test_a, ARRAY_SIZE(results9[0]), 1,	/* [한국어] 가운데 있던 항목 하나가 버려진 자리로 남았다. */
				     results9[0], results9[1], results9[2]);

	/* Test11: purge test_a (new array) */
	/* [한국어] 마지막으로 정리해 처음 모양으로 돌아오는지 확인한다. */
	test_b = arm_smmu_invs_purge(test_a);
	kfree(test_a);	/* [한국어] 옛 배열을 놓는다. */
	arm_smmu_v3_invs_test_verify(test, test_b, ARRAY_SIZE(results10[0]), 0,	/* [한국어] 묶음 4 만 남았다 — 병합과 해제가 대칭임을 보인다. */
				     results10[0], results10[1], results10[2]);

	kfree(test_b);	/* [한국어] 마지막 배열을 놓는다 — 시험이 메모리를 남기면 안 된다. */
}

/* [한국어] 이 모듈이 돌릴 시험 목록.
 *
 * 앞쪽은 STE 전환, 가운데는 CD 전환, 마지막이 무효화 배열이다. KUnit 이
 * 이 배열을 순서대로 돌리며, 각 항목이 독립적으로 실행된다 — 앞 시험이
 * 실패해도 뒤 시험은 그대로 돈다. */
static struct kunit_case arm_smmu_v3_test_cases[] = {
	KUNIT_CASE(arm_smmu_v3_write_ste_test_bypass_to_abort),	/* [한국어] 우회 → 중단. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_abort_to_bypass),	/* [한국어] 중단 → 우회. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_cdtable_to_abort),	/* [한국어] 1단계 → 중단 (장치를 뗄 때). */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_abort_to_cdtable),	/* [한국어] 중단 → 1단계 (장치를 붙일 때). */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_cdtable_to_bypass),	/* [한국어] 1단계 → 우회. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_bypass_to_cdtable),	/* [한국어] 우회 → 1단계. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_cdtable_s1dss_change),	/* [한국어] s1dss 만 바꾸기 — 한 번의 쓰기로 끝나야 한다. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s1dssbypass_to_stebypass),	/* [한국어] 두 종류의 우회 사이. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_stebypass_to_s1dssbypass),	/* [한국어] 그 역방향. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s2_to_abort),	/* [한국어] 2단계 → 중단. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_abort_to_s2),	/* [한국어] 중단 → 2단계. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s2_to_bypass),	/* [한국어] 2단계 → 우회. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_bypass_to_s2),	/* [한국어] 우회 → 2단계. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s1_to_s2),	/* [한국어] 1단계 → 2단계 (장치를 가상 머신에 넘길 때). */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s2_to_s1),	/* [한국어] 2단계 → 1단계 (회수할 때). */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_non_hitless),	/* [한국어] 끊김이 생길 수밖에 없는 전환. */
	KUNIT_CASE(arm_smmu_v3_write_cd_test_s1_clear),	/* [한국어] 빈 서술자 ↔ 1단계 서술자. */
	KUNIT_CASE(arm_smmu_v3_write_cd_test_s1_change_asid),	/* [한국어] ASID 만 바꾸기. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s1_to_s2_stall),	/* [한국어] 멈춤을 쓰는 장치의 1 → 2단계. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_s2_to_s1_stall),	/* [한국어] 그 역방향. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_nested_s1dssbypass_to_s1bypass),	/* [한국어] 중첩 → 순수 2단계. */
	KUNIT_CASE(arm_smmu_v3_write_ste_test_nested_s1bypass_to_s1dssbypass),	/* [한국어] 그 역방향. */
	KUNIT_CASE(arm_smmu_v3_write_cd_test_sva_clear),	/* [한국어] 빈 서술자 ↔ SVA 서술자. */
	KUNIT_CASE(arm_smmu_v3_write_cd_test_sva_release),	/* [한국어] SVA 서술자 ↔ "모두 폴트" 변형 — SVA 안전의 핵심. */
	KUNIT_CASE(arm_smmu_v3_invs_test),	/* [한국어] 무효화 배열의 병합·해제·정리. */
	{},	/* [한국어] 목록의 끝을 알리는 빈 항목 — KUnit 이 이것을 보고 멈춘다. */
};

/*
 * [한국어]
 * arm_smmu_v3_test_suite_init - 시험 묶음을 시작하기 전에 공용 항목을 지어 둔다
 *
 * @test: 시험 묶음 (여기서는 쓰지 않는다).
 * @return: 항상 0 — 실패할 일이 없다.
 *
 * 우회 STE 와 중단 STE 는 여러 시험이 함께 쓰는 상수 같은 값이라, 매번 짓지
 * 않고 묶음 시작 때 한 번만 짓는다. 두 값 모두 가짜 SMMU 의 능력 비트에
 * 따라 달라지므로, 전역 변수로 그냥 둘 수는 없고 이렇게 만들어야 한다.
 *
 * 실행 컨텍스트: KUnit 묶음 초기화. 잠들 수 있다.
 *
 * 호출 체인:
 *   KUnit 실행기 → [이 함수]
 *     → arm_smmu_make_bypass_ste() / arm_smmu_make_abort_ste()
 */
static int arm_smmu_v3_test_suite_init(struct kunit_suite *test)
{
	arm_smmu_make_bypass_ste(&smmu, &bypass_ste);	/* [한국어] 우회 항목은 SMMU 의 메모리 속성 능력에 따라 값이 달라져 하드웨어 포인터가 필요하다. */
	arm_smmu_make_abort_ste(&abort_ste);	/* [한국어] 중단 항목은 하드웨어와 무관하게 늘 같은 값이다. */
	return 0;	/* [한국어] 준비 완료. */
}

/* [한국어] KUnit 에 등록할 시험 묶음.
 *
 * 이름이 로그와 결과 보고에 그대로 찍히므로, 어느 드라이버의 시험인지
 * 한눈에 알 수 있게 지어 두었다. */
static struct kunit_suite arm_smmu_v3_test_module = {
	.name = "arm-smmu-v3-kunit-test",	/* [한국어] 결과 보고에 찍힐 이름. */
	.suite_init = arm_smmu_v3_test_suite_init,	/* [한국어] 묶음 시작 전에 한 번 불릴 준비 함수. */
	.test_cases = arm_smmu_v3_test_cases,	/* [한국어] 돌릴 시험 목록. */
};
kunit_test_suites(&arm_smmu_v3_test_module);	/* [한국어] 이 묶음을 KUnit 에 등록한다 — 모듈로 빌드하면 적재 때, 내장하면 부팅 때 돈다. */

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");	/* [한국어] 시험 전용으로만 열린 심볼(EXPORT_SYMBOL_IF_KUNIT)을 쓸 수 있게 한다 — 일반 모듈은 이 이름공간을 가져오지 않으므로 드라이버 내부가 함부로 노출되지 않는다. */
MODULE_DESCRIPTION("KUnit tests for arm-smmu-v3 driver");	/* [한국어] modinfo 에 찍힐 설명. */
MODULE_LICENSE("GPL v2");	/* [한국어] GPL 심볼을 쓸 수 있게 하는 라이선스 선언. */
