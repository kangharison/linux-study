// SPDX-License-Identifier: GPL-2.0
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *		    Horst Hummel <Horst.Hummel@de.ibm.com>
 *		    Carsten Otte <Cotte@de.ibm.com>
 *		    Martin Schwidefsky <schwidefsky@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2001
 *
 */

/* [한국어] [한국어 설명] 디시플린과 무관한 기본 오류 복구(ERP) 절반 (dasd_erp.c)
 * 
 * === 파일의 역할 ===
 * DASD 드라이버의 오류 복구 절차(ERP, Error Recovery Procedure) 가운데 **장치
 * 종류에 매이지 않는 부분** 만 모아 둔 파일이다. 199줄뿐이지만 이 디렉터리의
 * 오류 처리 전체가 여기에 뿌리를 둔다. 하는 일은 넷으로 갈린다.
 * (1) 복구용 요청(cqr)을 장치마다 미리 잡아 둔 erp_mem 풀에서 떼어 주고 되돌려
 * 받는 할당기 한 쌍 — dasd_alloc_erp_request() 와 dasd_free_erp_request().
 * (2) 디시플린이 자기 복구 절차를 내놓지 않을 때 쓰이는 기본 복구 동작 —
 * dasd_default_erp_action(). 이름 그대로 **그저 다시 시도할 뿐** 이며, 센스
 * 데이터를 해석하지 않는다.
 * (3) 그 짝인 기본 뒷정리 — dasd_default_erp_postaction(). refers 사슬을 뿌리까지
 * 거슬러 올라가며 복구용 cqr 을 모두 풀고, 마지막 결과를 원래 cqr 에 옮겨 적는다.
 * (4) 센스 데이터를 콘솔과 s390 디버그 영역에 남기는 dasd_log_sense() 와
 * dasd_log_sense_dbf(). 이 둘은 기본 ERP 와 상관없이 모든 디시플린이 쓴다.
 * dasd_int.h 의 struct dasd_discipline 이 가진 erp_action / erp_postaction 두 칸에
 * 디시플린이 자기 함수를 넣지 않기로 했을 때 대신 들어가는 값이 (2)와 (3)이다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층과 채널 서브시스템 사이가 아니라, **완료 처리 경로에서 갈라져 나온
 * 곁가지** 에 해당한다. 정상 완료는 이 파일을 지나가지 않는다. 흐름은 이렇다.
 * 
 *   채널 인터럽트 → dasd.c 의 dasd_int_handler()   [인터럽트 컨텍스트]
 *     → 오류면 cqr->status 를 DASD_CQR_ERROR 로 바꾸고 irb 를 cqr 에 복사
 *       → dasd_schedule_device_bh() 로 device 태스클릿을 깨운다
 *         → dasd.c 의 __dasd_process_cqr() 이 ERROR 를 DASD_CQR_NEED_ERP 로 승격
 *           → block 태스클릿의 __dasd_process_block_ccw_queue() 가 그것을 보고
 *             → discipline->erp_action(cqr) 로 **다음에 부를 함수** 를 받아 부른다
 *               → 디시플린이 기본값을 골랐다면 그것이 이 파일의
 *                 dasd_default_erp_action() 이다
 * 
 * 복구용 cqr 이 만들어져 다시 실행되고 끝나면, 되감기가 반대로 일어난다.
 * 
 *   복구 요청 완료 → dasd.c 의 __dasd_process_erp()
 *     → discipline->erp_postaction(cqr) 로 뒷정리 함수를 받아 부른다
 *       → 기본값이면 이 파일의 dasd_default_erp_postaction()
 *         → refers 사슬의 복구용 cqr 을 전부 풀고 원래 cqr 만 남긴다
 *           → 원래 cqr 의 callback() 이 blk-mq 요청을 끝낸다
 * 
 * 실행 컨텍스트가 셋으로 갈리는 것이 이 파일을 읽을 때 가장 주의할 점이다.
 * dasd_alloc_erp_request() 와 dasd_free_erp_request() 는 **인터럽트 문맥에서도
 * 불릴 수 있다** 는 전제로 쓰였다 — 그래서 일반 커널 할당기 대신 미리 잡아 둔
 * 풀을 쓰고, 잠금도 irqsave 판으로 잡는다. 반면 두 기본 ERP 함수와 두 로그
 * 함수는 실제로는 태스클릿(softirq)과 프로세스 컨텍스트에서만 불린다.
 * dasd.c 의 __dasd_sleep_on_erp() 가 부르는 자리는 프로세스 컨텍스트이고,
 * __dasd_process_block_ccw_queue() 가 부르는 자리는 softirq 다.
 * 어느 쪽이든 **잠들면 안 된다**.
 * 
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 dasd.c 가 유일한 진입점이다. dasd.c 는 이 파일의 함수를 직접
 * 이름으로 부르지 않고, 언제나 device->discipline 의 함수 포인터를 거쳐 부른다.
 * 그 포인터에 이 파일의 함수를 꽂아 주는 곳은 셋이다 — dasd_diag.c 의 두 콜백이
 * 무조건 이 파일의 기본값을 돌려주고(497·503줄), dasd_fba.c 는 오류가 났을 때
 * 기본값을 돌려주되 뒷정리는 '직전 단계가 기본 ERP 였는지' 를 cqr->function 으로
 * 확인한 뒤에만 기본 뒷정리를 돌려준다(215·221줄). dasd_eckd.c 는 보통
 * dasd_3990_erp_action() 을 돌려주지만, 특정 조건에서는 이 파일의 기본값으로
 * 내려온다(3572·3579줄).
 * 
 * 아래쪽으로는 dasd_int.h 의 청크 할당기 두 개(dasd_alloc_chunk /
 * dasd_free_chunk)와 참조 계수 인라인(dasd_get_device)에 기댄다.
 * dasd_3990_erp.c 는 이 파일의 할당기만 빌려 쓰는 큰 고객이다 — 자기 복구
 * 단계마다 dasd_alloc_erp_request() 로 cqr 을 만들고(200·1634·2357줄),
 * dasd_3990_erp_cleanup() 이 dasd_free_erp_request() 로 되돌린다(53·1718·2677줄).
 * 
 * 데이터 흐름은 두 갈래다. 하나는 **메모리** — device->erp_mem 이라는 미리 잡아
 * 둔 덩어리에서 cqr 한 개가 떼어져 나갔다가 그대로 되돌아온다. 다른 하나는
 * **상태** — 실패한 cqr 의 status 와 retries 가 읽히고, 복구가 끝나면 원래 cqr 의
 * status/startclk/stopclk/startdev 가 덮어써진다. 하드웨어와 직접 주고받는 것은
 * 없다. 이 파일에는 CCW 를 만드는 코드가 한 줄도 없다.
 * 
 * === 주요 함수/구조체 요약 ===
 * dasd_alloc_erp_request()   장치의 erp_mem 풀에서 cqr 하나와 그 뒤에 붙일 CCW
 *                            배열·데이터 영역을 한 덩어리로 떼어 온다. 실패하면
 *                            ERR_PTR(-ENOMEM). 장치 참조 계수를 하나 올린다.
 * dasd_free_erp_request()    그 덩어리를 풀에 되돌리고 참조 계수를 내린다.
 * dasd_default_erp_action()  재시도 횟수가 남아 있으면 cqr 을 DASD_CQR_FILLED 로
 *                            되돌려 다시 큐에 태우고, 다 썼으면 DASD_CQR_FAILED.
 *                            **복구용 cqr 을 새로 만들지 않는다** 는 점이 3990 ERP
 *                            와 결정적으로 다르다.
 * dasd_default_erp_postaction() refers 사슬을 따라 복구용 cqr 을 전부 풀고, 사슬의
 *                            마지막 결과를 원래 cqr 에 옮겨 적는다.
 * dasd_log_sense()           센스 데이터를 콘솔에 찍는다. 타임아웃과 전송 오류는
 *                            센스가 없으므로 한 줄만 찍고 돌아간다.
 * dasd_log_sense_dbf()       같은 일을 s390 디버그 영역(순환 버퍼)에 한다.
 * 
 * === 이 파일을 읽을 때 알아 두면 좋은 어휘 ===
 * ERP 사슬  복구용 cqr 이 원래 cqr 을 refers 로 가리켜 이루는 외줄 목록.
 *           뿌리(refers 가 NULL 인 것)가 블록 계층에서 온 진짜 요청이다.
 *           복구가 여러 단계로 이어지면 사슬이 여러 단이 된다.
 * function  cqr 안의 void 포인터인데, **값이 아니라 표식** 으로 쓴다. 이 cqr 을
 *           만든 ERP 함수의 주소를 넣어 두어, 다음 단계가 '직전에 무엇을 했는지'
 *           를 알아본다.
 * erp_mem   장치마다 따로 잡아 둔 복구 전용 메모리. 일반 요청 풀(ccw_mem)이
 *           말라도 복구는 진행돼야 하므로 나눠 두었다.
 * lpm       Logical Path Mask. 채널 경로 8개를 한 바이트로 나타낸 것.
 *           경로 0 이 0x80 이다. */
/* [한국어] EXPORT_SYMBOL 매크로의 정의처. 이 파일은 맨 아래에서 다섯 개, 그리고
 * dasd_log_sense_dbf() 정의 바로 뒤에서 하나, 모두 여섯 개의 심볼을 내보낸다.
 * DASD 코어(dasd_mod)와 각 디시플린이 따로 모듈로 빌드될 수 있으므로,
 * 디시플린 모듈이 이 파일의 함수를 이름으로 참조하려면 내보내기가 필요하다. */
#include <linux/export.h>
/* [한국어] isdigit/toupper 같은 문자 분류 매크로.
 * [상류 코드 관찰] 이 파일 안에서 그 매크로를 쓰는 자리는 한 곳도 없다.
 * 문자열을 다루던 옛 코드가 남긴 포함으로 보인다.
 * 원본(1f0e418bb6) 13줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/ctype.h>
/* [한국어] __init/__exit 같은 초기화 구역 표시자.
 * [상류 코드 관찰] 이 파일에는 초기화 함수가 없어 그 표시자를 쓰지 않는다.
 * 원본(1f0e418bb6) 14줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/init.h>

/* [한국어] s390 고유의 순환 디버그 로그(debug feature) 인터페이스.
 * 아래 dasd_default_erp_action() 이 쓰는 DBF_DEV_EVENT 매크로가 결국 이 계층의
 * 기록 함수를 부른다. arch/s390 소관이라 이 트리에는 없어 정의는 확인 못 함.
 * dasd_int.h 도 같은 헤더를 포함하므로 여기서는 중복 포함이지만, 이 파일이
 * 직접 쓰는 기능이라 스스로 포함해 두는 편이 의존을 분명히 한다. */
#include <asm/debug.h>
/* [한국어] ASCII 와 EBCDIC 을 서로 옮기는 변환 매크로.
 * 아래 dasd_alloc_erp_request() 가 cqr 의 눈표(magic) 4바이트를 EBCDIC 으로
 * 바꿀 때 쓰는 ASCEBC 가 여기서 온다. 메인프레임 제어 장치가 EBCDIC 문자
 * 집합을 쓰기 때문에 필요한 변환이다.
 * arch/s390 소관이라 이 트리에는 없어 정의는 확인 못 함. */
#include <asm/ebcdic.h>
/* [한국어] copy_to_user/copy_from_user 계열.
 * [상류 코드 관찰] 이 파일에는 사용자 공간과 데이터를 주고받는 코드가 없다.
 * ioctl 처리가 이 파일에 함께 있던 시절의 잔재로 보인다.
 * 원본(1f0e418bb6) 18줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/uaccess.h>

/* [한국어] DASD 서브시스템의 중앙 헤더. 이 파일이 쓰는 어휘 전부가 여기서 온다 —
 * struct dasd_ccw_req(cqr), struct dasd_device, struct dasd_discipline,
 * 청크 할당기 dasd_alloc_chunk()/dasd_free_chunk(), 참조 계수 인라인
 * dasd_get_device(), 요청 상태 상수 DASD_CQR_ 계열, 표지 비트
 * DASD_CQR_FLAGS_USE_ERP 와 DASD_CQR_VERIFY_PATH, 경로 마스크를 읽는
 * dasd_path_get_opm(), 그리고 로그 매크로 DBF_DEV_EVENT 가 모두 그것이다. */
#include "dasd_int.h"

/* [한국어]
 * dasd_alloc_erp_request - 오류 복구 전용 풀에서 복구용 cqr 한 덩어리를 떼어 온다
 * 
 * @magic: 이 요청을 만든 디시플린의 눈표(eye catcher). 호출자는 언제나 복구
 *         대상 cqr 의 magic 을 그대로 물려준다(dasd_3990_erp.c:200/1634/2357).
 *         ASCII 로 들어와 이 함수 안에서 EBCDIC 으로 바뀐다.
 * @cplength: 뒤에 붙일 CCW(Channel Command Word) 개수. 0 이면 CCW 영역을 만들지
 *         않고 cqr->cpaddr 을 NULL 로 둔다.
 * @datasize: 뒤에 붙일 데이터 영역의 바이트 수. 0 이면 cqr->data 가 NULL 이다.
 * @device: 메모리를 떼어 올 장치. 이 장치의 erp_mem 풀과 mem_lock 을 쓴다.
 * @return: 성공하면 0 으로 초기화된 cqr 포인터, 자리가 없으면 ERR_PTR(-ENOMEM).
 *         호출자는 반드시 IS_ERR() 로 검사해야 한다 — NULL 이 아니라 오류
 *         포인터를 돌려주기 때문이다.
 * 
 * **일반 커널 할당기를 쓰지 않는 이유** 가 이 함수의 존재 이유 전부다. 오류
 * 복구는 이미 무언가 잘못된 상황에서 시작되며, 그때 메모리가 모자라 복구용
 * 요청조차 만들지 못하면 I/O 가 그대로 실패한다. 특히 이 장치로 스왑 I/O 가
 * 나가고 있으면 '메모리를 확보하려면 I/O 가 되어야 하고, I/O 가 되려면 메모리가
 * 있어야 하는' 순환에 빠진다. 그래서 장치를 만들 때 미리 잡아 둔 erp_mem 덩어리를
 * 쪼개 쓴다. 그리고 그 풀은 일반 요청용 ccw_mem 과 **따로** 두어, 일반 요청이
 * 풀을 다 써 버려도 복구는 진행되게 한다.
 * 
 * 동작은 다섯 단계다.
 * 1. 제정신 검사. 데이터 영역과 CCW 배열이 각각 한 페이지를 넘지 않는지 본다.
 * 2. 필요한 전체 크기를 센다. cqr 머리를 8바이트로 올림하고, 그 뒤에 CCW 배열과
 *    데이터 영역을 이어 붙인 크기다. **셋이 한 덩어리** 로 잡히는 것이 요점이다.
 * 3. mem_lock 을 인터럽트 차단 판으로 잡고 청크 할당기에서 그 크기를 떼어 온다.
 * 4. cqr 머리만 0 으로 지우고 두 목록 고리를 초기화한 뒤, 덩어리 안쪽을 가리키도록
 *    cpaddr 과 data 를 계산해 넣는다.
 * 5. 눈표를 EBCDIC 으로 바꾸고, 이 요청이 ERP 를 쓴다는 표지 비트를 세우고,
 *    장치 참조 계수를 하나 올린 뒤 돌려준다.
 * 
 * 실행 컨텍스트: **인터럽트 문맥에서도 불릴 수 있다는 전제** 로 쓰였다. 잠금을
 * spin_lock_irqsave 로 잡는 것과, 잠들 수 있는 할당기를 쓰지 않는 것이 그 증거다.
 * 실제 호출자인 dasd_3990_erp.c 의 복구 단계들은 태스클릿(softirq) 문맥에서
 * 불리지만, 이 함수 자체는 더 엄한 전제 위에 서 있다.
 * 
 * caller: dasd_3990_erp.c 의 dasd_3990_erp_DCTL()(200줄),
 * dasd_3990_erp_action_1B_32()(1634줄), dasd_3990_erp_add_erp()(2357줄).
 * 이 파일 안에서는 아무도 부르지 않는다 — 기본 ERP 는 복구용 cqr 을 만들지 않기
 * 때문이다.
 * callee: BUG_ON(), spin_lock_irqsave(), dasd_alloc_chunk()(dasd_int.h 의 인라인),
 * spin_unlock_irqrestore(), memset(), INIT_LIST_HEAD(), ASCEBC(), set_bit(),
 * dasd_get_device().
 * 
 * 에러 경로: 자리가 없으면 ERR_PTR(-ENOMEM) 을 돌려주고, 그때는 참조 계수를
 * 올리지 않으므로 호출자가 따로 되돌릴 것이 없다. 호출자들은 그 값을 받으면
 * 복구를 포기하고 원래 요청을 DASD_CQR_FAILED 로 끝낸다. 크기 검사에 걸리면
 * BUG_ON 이 커널을 멈추므로 그 경로는 복구가 아니라 프로그래밍 오류 신고다.
 * 
 * 호출 체인:
 *   dasd_3990_erp.c 의 복구 단계 → [이 함수]
 *     → dasd_alloc_chunk() → dasd_get_device() */
struct dasd_ccw_req *
dasd_alloc_erp_request(unsigned int magic, int cplength, int datasize,
		       struct dasd_device * device)
{
	/* [한국어] spin_lock_irqsave 가 저장하고 되돌릴 인터럽트 상태 비트.
	 * 지역 변수여야 하는 이유는 잠금이 중첩될 수 있어서다. */
	unsigned long flags;
	/* [한국어] 떼어 온 덩어리를 cqr 로 보는 포인터. 실패하면 NULL 이 담긴다. */
	struct dasd_ccw_req *cqr;
	/* [한국어] 덩어리 안쪽을 훑어 가며 cpaddr 과 data 의 자리를 정하는 커서.
	 * 바이트 단위로 더할 수 있도록 char 포인터로 둔다. */
	char *data;
	/* [한국어] 떼어 올 전체 바이트 수. 아래에서 세 조각의 합으로 계산된다. */
	int size;

	/* Sanity checks */
	/* [한국어] 제정신 검사. 데이터 영역과 CCW 배열이 각각 한 페이지를 넘으면 커널을 멈춘다.
	 * **한 페이지가 상한인 이유** 는 청크 할당기가 나눠 주는 덩어리가 장치를 만들 때
	 * 잡아 둔 유한한 페이지들에서 나오기 때문이며, 그보다 큰 요구는 설계상 있을 수
	 * 없다. 즉 이것은 실행 중에 일어날 수 있는 오류가 아니라, 호출자가 잘못 짠
	 * 경우를 즉시 드러내려는 장치다. 그래서 오류 반환이 아니라 BUG_ON 이다.
	 * sizeof 가 곱셈에 들어가 부호 없는 계산이 되므로, cplength 가 음수여도
	 * 아주 큰 값으로 뒤집혀 이 검사에 걸린다. */
	BUG_ON(datasize > PAGE_SIZE ||
	       (cplength*sizeof(struct ccw1)) > PAGE_SIZE);

	/* [한국어] cqr 머리의 크기를 8바이트 경계로 올림해 출발한다. `(x + 7L) & -8L` 이
	 * 올림 계산이며, -8L 은 하위 3비트가 0 인 마스크다(2의 보수).
	 * **올림하는 이유** 는 바로 뒤에 오는 struct ccw1 배열이 정렬을 요구하기
	 * 때문이다. 아래 145줄이 같은 식을 한 번 더 써서 그 자리를 다시 계산한다. */
	size = (sizeof(struct dasd_ccw_req) + 7L) & -8L;
	/* [한국어] CCW 를 하나라도 붙여 달라고 했으면 */
	if (cplength > 0)
		/* [한국어] 그 개수만큼 struct ccw1 배열 크기를 더한다. 이 배열이 채널이 실제로
		 * 실행할 명령 사슬이 들어갈 자리다. */
		size += cplength * sizeof(struct ccw1);
	/* [한국어] 데이터 영역도 요구했으면 */
	if (datasize > 0)
		/* [한국어] 그 바이트 수를 그대로 더한다. 여기는 정렬을 다시 맞추지 않는데,
		 * struct ccw1 배열이 이미 8의 배수 크기라 뒤따르는 자리도 정렬을 물려받는다. */
		size += datasize;
	/* [한국어] 풀과 청크 목록을 지키는 잠금을 **인터럽트를 막는 판** 으로 잡는다.
	 * 같은 장치의 mem_lock 을 인터럽트 처리기 쪽에서도 잡을 수 있으므로, 여기서
	 * 인터럽트를 열어 두면 자기 자신을 기다리는 교착이 난다. flags 에 이전 상태를
	 * 담아 두었다가 아래에서 그대로 되돌린다. */
	spin_lock_irqsave(&device->mem_lock, flags);
	/* [한국어] 장치의 ERP 전용 청크 목록에서 size 바이트를 떼어 온다. dasd_int.h 의
	 * 인라인이며 최초 적합(first fit) 방식이다. 돌려주는 주소는 청크 머리
	 * **뒤** 이므로, 그 자리를 그대로 cqr 로 쓴다. 일반 요청이 쓰는 ccw_chunks 가
	 * 아니라 erp_chunks 를 쓰는 것이 이 함수의 핵심이다. */
	cqr = (struct dasd_ccw_req *)
		dasd_alloc_chunk(&device->erp_chunks, size);
	/* [한국어] 잠금을 놓고 인터럽트 상태를 되돌린다. 아래의 초기화는 이미 자기 것이 된
	 * 메모리를 만지는 일이라 잠금 밖에서 해도 안전하다 — 잠금이 지키는 것은
	 * 청크 목록이지 떼어 낸 메모리가 아니다. */
	spin_unlock_irqrestore(&device->mem_lock, flags);
	/* [한국어] 자리가 없어 할당기가 NULL 을 돌려준 경우 */
	if (cqr == NULL)
		/* [한국어] NULL 이 아니라 **오류 포인터** 로 실패를 알린다. 호출자가 IS_ERR() 로
		 * 검사하는 것과 짝을 이루며, 이 파일의 다른 반환 경로가 모두 유효한
		 * 포인터라는 점과 함께 인터페이스를 이룬다. */
		return ERR_PTR(-ENOMEM);
	/* [한국어] cqr **머리만** 0 으로 지운다. 뒤에 붙은 CCW 배열과 데이터 영역은 여기서
	 * 지우지 않고 아래에서 조각별로 따로 지운다. 이 한 줄로 refers·function·
	 * memdev·startdev 를 비롯한 모든 필드가 NULL/0 이 되므로, 호출자는 자기가
	 * 쓸 필드만 채우면 된다. */
	memset(cqr, 0, sizeof(struct dasd_ccw_req));
	/* [한국어] 장치 큐(device->ccw_queue)에 매달릴 고리를 자기 자신을 가리키게 초기화한다.
	 * 큐에 넣기 전에도 list_del 이나 list_empty 를 안전하게 부를 수 있게 하는 것이
	 * 목적이다. */
	INIT_LIST_HEAD(&cqr->devlist);
	/* [한국어] 블록 큐(block->ccw_queue)에 매달릴 고리도 같은 이유로 초기화한다.
	 * **이 초기화가 있어야** 아래 dasd_default_erp_postaction() 이 복구용 cqr 에
	 * list_del 을 걸어도 안전하다 — 복구용 cqr 은 블록 큐에 들어간 적이 없을 수
	 * 있기 때문이다. */
	INIT_LIST_HEAD(&cqr->blocklist);
	/* [한국어] cqr 머리 바로 뒤의 자리를 계산한다. 131줄과 똑같은 올림 식을 다시 써서
	 * 크기 계산과 자리 계산이 어긋나지 않게 맞춘다. char 포인터로 캐스팅하는 것은
	 * 바이트 단위 산술을 하기 위해서다. */
	data = (char *) cqr + ((sizeof(struct dasd_ccw_req) + 7L) & -8L);
	/* [한국어] 기본값은 CCW 없음. 아래 분기가 잡히지 않으면 이 값이 그대로 남는다. */
	cqr->cpaddr = NULL;
	/* [한국어] CCW 배열을 요구했으면 */
	if (cplength > 0) {
		/* [한국어] 지금 커서가 가리키는 자리를 CCW 사슬의 시작으로 삼는다. 채널이 DMA 로
		 * 읽는 유일한 포인터이며, 내용은 호출자가 채운다. */
		cqr->cpaddr = (struct ccw1 *) data;
		/* [한국어] 커서를 배열 크기만큼 앞으로 민다. 이제 커서는 데이터 영역이 올 자리를
		 * 가리킨다. 131~135줄의 크기 계산과 **같은 순서** 로 밀어야 덩어리 밖을
		 * 넘지 않는다. */
		data += cplength*sizeof(struct ccw1);
		/* [한국어] CCW 배열을 0 으로 지운다. 채널이 읽는 영역이라 쓰레기 값이 남아 있으면
		 * 정의되지 않은 명령이 실행될 수 있다. */
		memset(cqr->cpaddr, 0, cplength*sizeof(struct ccw1));
	}
	/* [한국어] 기본값은 데이터 영역 없음. */
	cqr->data = NULL;
	/* [한국어] 데이터 영역을 요구했으면 */
	if (datasize > 0) {
		/* [한국어] CCW 배열 뒤로 밀린 커서 자리를 데이터 영역의 시작으로 삼는다.
		 * CCW 를 요구하지 않았다면 커서는 아직 cqr 머리 바로 뒤에 있다. */
		cqr->data = data;
 		/* [한국어] 데이터 영역도 0 으로 지운다. 호출자는 자기 형식으로 캐스팅해 필요한
 		 * 필드만 채우므로, 나머지가 0 이어야 규격에 맞는 값이 된다.
 		 * [상류 코드 관찰] 이 줄만 들여쓰기가 공백 하나로 시작한 뒤 탭이 이어져,
 		 * 같은 블록의 다른 줄과 어긋난다. 원본(1f0e418bb6) 59줄에서 확인했으며
 		 * 코드는 고치지 않았다. */
 		memset(cqr->data, 0, datasize);
	}
	/* [한국어] 디시플린 눈표를 넣는다. 호출자가 복구 대상 cqr 의 magic 을 그대로 넘기므로,
	 * 복구용 cqr 도 같은 디시플린의 것으로 보인다. */
	cqr->magic = magic;
	/* [한국어] 그 눈표 4바이트를 제자리에서 ASCII 에서 EBCDIC 으로 바꾼다. 메인프레임
	 * 제어 장치가 EBCDIC 을 쓰기 때문이며, dasd.c 가 cqr 을 검사할 때
	 * discipline->ebcname 과 바이트로 비교하는 것과 짝을 이룬다.
	 * [상류 코드 관찰] 호출자들이 넘기는 magic 은 이미 EBCDIC 로 저장돼 있던
	 * cqr->magic 이라, 여기서 한 번 더 변환이 걸린다. 즉 복구용 cqr 의 magic 은
	 * 원본과 다른 바이트열이 된다. 원본(1f0e418bb6) 62줄과 dasd_3990_erp.c 의
	 * 호출부에서 확인했으며 코드는 고치지 않았다. */
	ASCEBC((char *) &cqr->magic, 4);
	/* [한국어] 이 요청도 오류가 나면 다시 ERP 를 태우라는 표지를 세운다. 마스크가 아니라
	 * **비트 번호** 이며(값 0), 원자적 비트 연산이라 잠금 없이 안전하다.
	 * dasd.c 의 __dasd_sleep_on_erp() 와 __dasd_process_block_ccw_queue() 가 이
	 * 비트를 보고 복구 경로로 들어갈지 정한다. */
	set_bit(DASD_CQR_FLAGS_USE_ERP, &cqr->flags);
	/* [한국어] 장치 참조 계수를 하나 올린다. **이 cqr 이 살아 있는 동안 장치가 사라지면
	 * 안 되기 때문** 이며, 특히 cqr 이 device->erp_mem 안에 들어 있어 장치가
	 * 해제되면 이 메모리 자체가 없어진다. 짝이 되는 감소는
	 * dasd_free_erp_request() 안에 있다. */
	dasd_get_device(device);
	/* [한국어] 초기화가 끝난 cqr 을 돌려준다. 호출자가 여기에 refers·function·startdev·
	 * memdev·expires·retries 를 채워 넣어 복구용 요청을 완성한다. */
	return cqr;
}

/* [한국어]
 * dasd_free_erp_request - 복구용 cqr 을 ERP 풀에 되돌리고 장치 참조를 놓는다
 * 
 * @cqr: dasd_alloc_erp_request() 가 돌려주었던 포인터. 청크 머리 바로 뒤의
 *       주소이므로 그대로 청크 할당기에 넘길 수 있다.
 * @device: 되돌려 줄 풀을 가진 장치. **할당할 때 쓴 장치와 같아야 한다.**
 *       호출자들은 그래서 cqr->memdev 를 넘긴다.
 * @return: 없다. 실패할 수 없는 연산이다.
 * 
 * dasd_alloc_erp_request() 의 정확한 짝이다. 하는 일은 둘뿐이다 — 청크를 풀에
 * 되돌리고, 할당 때 올렸던 장치 참조 계수를 내린다.
 * 
 * **되돌려 줄 풀을 인자로 받는 이유** 는 cqr 자신이 자기가 어느 장치의 풀에서
 * 나왔는지 기억하지 않기 때문이다. 그 기억은 cqr->memdev 에 있고, 그것을 채우는
 * 쪽은 이 파일이 아니라 호출자다. PAV 환경에서는 기본 장치의 풀에서 떼어 별칭
 * 장치로 요청을 내보내는 일이 있어, 메모리를 준 장치와 요청을 실행한 장치가
 * 다를 수 있다. 그래서 memdev 와 startdev 를 따로 두고, 해제는 반드시 memdev
 * 쪽으로 한다.
 * 
 * 실행 컨텍스트: 할당 쪽과 같은 전제다 — 인터럽트 문맥에서도 안전해야 하므로
 * 잠금을 irqsave 판으로 잡는다. 실제로는 태스클릿과 프로세스 컨텍스트에서
 * 불린다.
 * 
 * caller: 이 파일의 dasd_default_erp_postaction()(사슬을 풀 때),
 * dasd_3990_erp.c 의 dasd_3990_erp_cleanup()(53줄),
 * dasd_3990_erp_action_1B_32_error()(1718줄), dasd_3990_erp_further_erp()(2677줄).
 * callee: spin_lock_irqsave(), dasd_free_chunk(), spin_unlock_irqrestore(),
 * atomic_dec().
 * 
 * 에러 경로: 없다. 주소가 정말 이 풀에서 나온 것인지 검사하지 않으므로,
 * 엉뚱한 주소를 넘기면 청크 목록이 조용히 망가진다.
 * 
 * 호출 체인:
 *   dasd_default_erp_postaction() / dasd_3990_erp.c 의 정리 함수들 → [이 함수]
 *     → dasd_free_chunk() → atomic_dec() */
void
dasd_free_erp_request(struct dasd_ccw_req *cqr, struct dasd_device * device)
{
	/* [한국어] 인터럽트 상태를 담아 둘 지역 변수. 잠금을 놓을 때 그대로 되돌린다. */
	unsigned long flags;

	/* [한국어] 청크 목록을 지키는 잠금을 인터럽트 차단 판으로 잡는다. 할당 쪽과 같은 잠금,
	 * 같은 판이어야 서로를 막아 줄 수 있다. */
	spin_lock_irqsave(&device->mem_lock, flags);
	/* [한국어] 청크를 목록에 되돌린다. dasd_int.h 의 인라인이며, 되돌리면서 주소가 맞닿은
	 * 이웃 청크와 합쳐 단편화를 막는다. 넘기는 주소가 cqr 자신인 것은
	 * 할당기가 청크 머리 **뒤** 를 돌려주었기 때문이며, 할당기 안에서 머리
	 * 크기만큼 물러나 청크 머리를 되찾는다. */
	dasd_free_chunk(&device->erp_chunks, cqr);
	/* [한국어] 잠금을 놓고 인터럽트 상태를 되돌린다. */
	spin_unlock_irqrestore(&device->mem_lock, flags);
	/* [한국어] 할당 때 올렸던 장치 참조 계수를 내린다.
	 * [상류 코드 관찰] dasd_int.h 가 제공하는 감소용 인라인을 쓰지 않고 원자 연산을
	 * 직접 부른다. 그 인라인은 계수가 0 이 되면 해제를 기다리는 쪽을 깨워 주는데,
	 * 이 줄은 그 깨우기를 하지 않는다. 다만 복구용 cqr 을 풀 때 그 장치의 마지막
	 * 참조가 사라지는 일은 구조상 생기지 않는다 — 복구 중인 장치는 상위 경로가
	 * 따로 참조를 쥐고 있기 때문이다. 원본(1f0e418bb6) 76줄에서 확인했으며 코드는
	 * 고치지 않았다. */
	atomic_dec(&device->ref_count);
}


/* [한국어]
 * dasd_default_erp_action - 센스 데이터를 보지 않고 그냥 다시 시도하는 기본 복구
 * 
 * @cqr: 오류로 끝나 DASD_CQR_NEED_ERP 가 된 요청. 이 파일 바깥의 3990 ERP 와
 *       달리 **이 요청 자신을 되살릴 뿐, 복구용 cqr 을 새로 만들지 않는다.**
 * @return: 언제나 인자로 받은 cqr 그대로. 호출자는 반환값을 IS_ERR() 로 검사하며
 *       (dasd.c:2773), 이 함수는 오류 포인터를 돌려주는 일이 없으므로 그 검사에
 *       걸리지 않는다.
 * 
 * 디시플린이 자기 복구 절차를 내놓지 않을 때 쓰이는 최소한의 복구다. 위쪽
 * 상류 주석이 적어 둔 대로 '그냥 재시도' 가 전부이며, 그 이유는 옆의 주석이
 * 말하듯 **센스 데이터가 없어 판단할 근거가 없기 때문** 이다. 센스를 해석해
 * 단계별로 대응하는 쪽은 dasd_3990_erp.c 의 상태 기계이고, 이 함수는 그것이
 * 없을 때의 안전망이다.
 * 
 * 동작은 두 갈래뿐이다.
 * 1. 재시도 횟수가 남아 있으면 — 디버그 로그를 한 줄 남기고, 경로 검증 요청이
 *    아닌 한 허용 경로 마스크를 장치의 현재 동작 가능 경로 전체로 되돌린 뒤,
 *    상태를 DASD_CQR_FILLED 로 낮춘다. 그러면 호출자인 태스클릿이 이 요청을
 *    아직 끝나지 않은 것으로 보고 다시 큐에 태운다.
 * 2. 다 썼으면 — 콘솔에 오류를 한 줄 찍고 상태를 DASD_CQR_FAILED 로 못 박은 뒤
 *    종료 시각을 남긴다.
 * 
 * **재시도 횟수를 여기서 줄이지 않는다** 는 점이 중요하다. 줄이는 쪽은
 * dasd.c 의 I/O 시작 경로이며, 이 함수는 남은 값을 읽기만 한다.
 * 
 * 실행 컨텍스트: 두 곳에서 불린다. dasd.c 의 __dasd_process_block_ccw_queue()
 * (2772줄)가 부르는 자리는 block 태스클릿, 즉 softirq 문맥이다.
 * __dasd_sleep_on_erp()(2247줄)가 부르는 자리는 동기 요청을 기다리는 프로세스
 * 컨텍스트다. 어느 쪽도 잠들면 안 되며, 이 함수는 잠들 수 있는 호출을 하지
 * 않는다.
 * 
 * caller: 디시플린의 erp_action 콜백을 통해서만 불린다 — dasd_diag.c:497 이
 * 무조건 이 함수를 돌려주고, dasd_fba.c:215 와 dasd_eckd.c:3572 가 조건에 따라
 * 돌려준다. 그 반환값을 실제로 부르는 곳이 위의 dasd.c 두 자리다.
 * callee: DBF_DEV_EVENT(결국 s390 의 debug_sprintf_event), test_bit(),
 * dasd_path_get_opm()(dasd_int.h 인라인), pr_err(), dev_name(),
 * get_tod_clock()(arch/s390 소관이라 이 트리에서 확인 못 함).
 * 
 * 에러 경로: 이 함수 자체는 실패하지 않는다. 재시도를 다 쓴 경우가 '오류로
 * 끝내는' 경로이며, 그때 상태를 DASD_CQR_FAILED 로 두면 호출자가
 * dasd_log_sense() 로 센스를 찍고 요청을 블록 계층에 오류로 돌려준다.
 * 
 * 호출 체인:
 *   dasd.c 의 __dasd_process_block_ccw_queue() / __dasd_sleep_on_erp()
 *     → discipline->erp_action → [이 함수]
 *       → dasd_path_get_opm() / get_tod_clock() */
/*
 * dasd_default_erp_action just retries the current cqr
 */
struct dasd_ccw_req *
dasd_default_erp_action(struct dasd_ccw_req *cqr)
{
	/* [한국어] 장치를 담아 둘 지역 변수. 로그와 경로 마스크 조회에 쓴다. */
	struct dasd_device *device;

	/* [한국어] 이 요청을 **실제로 내보낸** 장치를 꺼낸다. 메모리를 준 memdev 가 아니라
	 * startdev 인 이유는, 여기서 알고 싶은 것이 '어느 장치에서 실패했는가' 와
	 * '그 장치의 지금 쓸 수 있는 경로가 무엇인가' 이기 때문이다. PAV 환경에서는
	 * 둘이 다를 수 있다. */
	device = cqr->startdev;

        /* just retry - there is nothing to save ... I got no sense data.... */
        /* [한국어] 재시도 횟수가 남았는지 본다. 옆의 상류 주석이 이 분기의 전제를 말한다 —
         * 센스 데이터가 없어 무엇이 잘못됐는지 알 수 없으므로, 할 수 있는 일은
         * 다시 해 보는 것뿐이다. */
        if (cqr->retries > 0) {
		/* [한국어] 장치별 디버그 영역에 남은 재시도 횟수를 남긴다. 콘솔이 아니라 순환
		 * 버퍼로 가므로 자주 불려도 부담이 적다. DBF_DEBUG 는 가장 낮은 수준이라
		 * 기본 설정(DBF_WARNING)에서는 걸러지고, 문제를 쫓을 때 수준을 낮춰 본다. */
		DBF_DEV_EVENT(DBF_DEBUG, device,
                             "default ERP called (%i retries left)",
                             cqr->retries);
		/* [한국어] 경로 검증용 요청인지 확인한다. **경로 검증 요청은 일부러 특정 경로
		 * 하나만 골라 내보낸 것** 이므로, 아래처럼 마스크를 전체로 되돌리면 검증의
		 * 뜻이 사라진다. 그래서 그 경우에만 마스크를 건드리지 않는다. */
		if (!test_bit(DASD_CQR_VERIFY_PATH, &cqr->flags))
			/* [한국어] 허용 경로 마스크를 장치의 현재 동작 가능 경로 전체로 되돌린다.
			 * 실패한 요청은 dasd_3990_erp.c 등이 실패한 경로를 마스크에서 깎아 두었을
			 * 수 있는데, 여기서 다시 넓혀 **다른 경로로도 시도할 기회** 를 준다.
			 * dasd_path_get_opm() 은 device->opm 을 그대로 돌려주는 한 줄짜리 인라인이다. */
			cqr->lpm = dasd_path_get_opm(device);
		/* [한국어] 상태를 '아직 제출되지 않은 요청' 으로 되돌린다. 이 한 줄이 재시도의
		 * 전부다 — 호출자인 태스클릿이 최종 상태가 아닌 요청을 보고 다시 장치 큐에
		 * 넣어 준다. dasd.c 의 __dasd_sleep_on_loop_condition() 도 이 상태를 보고
		 * 아직 끝나지 않았다고 판단한다. */
		cqr->status = DASD_CQR_FILLED;
        /* [한국어] 재시도 횟수를 다 쓴 경우. */
        } else {
		/* [한국어] 콘솔에 오류를 남긴다. 디버그 영역이 아니라 콘솔인 이유는 이것이 되돌릴
		 * 수 없는 실패이기 때문이다. 장치 이름은 CCW 장치의 버스 ID(예: 0.0.1234)다. */
		pr_err("%s: default ERP has run out of retries and failed\n",
		       dev_name(&device->cdev->dev));
		/* [한국어] 상태를 '최종적으로 실패' 로 못 박는다. 이 상태가 되면 호출자가
		 * dasd_log_sense() 로 센스를 찍고, 자동 정지 판정을 거친 뒤 요청을 블록
		 * 계층에 오류로 돌려준다. */
		cqr->status = DASD_CQR_FAILED;
		/* [한국어] 종료 시각을 s390 의 TOD(Time-Of-Day) 시계로 남긴다. 실패는 완료
		 * 인터럽트를 통해 끝난 것이 아니라 여기서 끝난 것이므로, 통계가 쓰는
		 * 구간이 비지 않도록 직접 찍어 준다. */
		cqr->stopclk = get_tod_clock();
        }
        /* [한국어] 언제나 인자로 받은 cqr 을 그대로 돌려준다. 반환값이 의미를 가지는 쪽은
         * 복구용 cqr 을 새로 만드는 3990 ERP 이며, 이 함수는 자기 자신을 되살릴
         * 뿐이라 돌려줄 새 요청이 없다. */
        return cqr;
}				/* end dasd_default_erp_action */

/* [한국어]
 * dasd_default_erp_postaction - ERP 사슬을 통째로 풀고 원래 요청에 결과를 옮긴다
 * 
 * @cqr: 방금 끝난 **사슬의 맨 앞(가장 나중에 만들어진) 복구용 cqr**.
 *       위쪽 상류 주석이 이 인자를 'erp_head' 라고 부르는 것이 그 뜻이다.
 *       refers 와 function 이 모두 채워져 있어야 한다.
 * @return: 사슬의 뿌리에 있던 **원래 cqr**. 블록 계층에서 온 진짜 요청이며,
 *       호출자는 이 값을 쓰지 않지만 3990 판 뒷정리와 형(type)을 맞추려고
 *       돌려준다.
 * 
 * 복구가 끝난 뒤의 뒷정리다. ERP 는 실패한 요청을 복구용 cqr 로 감싸 다시
 * 시도하는 방식이라, 복구가 여러 단계로 이어지면 cqr 이 refers 로 이어진 외줄
 * 사슬을 이룬다. 그 사슬을 남겨 두면 메모리도 새고 블록 계층 요청도 끝나지
 * 않으므로, 여기서 **뿌리만 남기고 전부 푼다.**
 * 
 * 동작은 넷이다.
 * 1. 사슬 맨 앞의 결과를 지역 변수에 챙긴다 — 성공 여부와 세 값(시작 시각,
 *    종료 시각, 실행 장치). **먼저 챙기는 이유** 는 그 cqr 이 아래 반복에서
 *    곧 해제되기 때문이다.
 * 2. refers 를 따라 거슬러 올라가며 복구용 cqr 을 하나씩 블록 큐에서 떼고
 *    풀에 되돌린다. refers 가 NULL 인 것이 뿌리이므로 그 앞에서 멈춘다.
 * 3. 챙겨 둔 세 값을 뿌리에 옮겨 적는다. 그래야 통계와 상위 계층이 '언제
 *    시작해 언제 끝났는가' 를 복구 과정까지 포함해 볼 수 있다.
 * 4. 성공이면 뿌리를 DASD_CQR_DONE, 실패면 DASD_CQR_FAILED 로 못 박는다.
 * 
 * **성공 판정을 반복 전에 해 두는 것** 이 이 함수의 핵심 순서다. 사슬 맨 앞의
 * 상태가 곧 복구의 최종 결과이며, 그 아래 단계들의 상태는 이미 의미가 없다.
 * 
 * 위쪽 상류 주석은 이 함수가 '디시플린 뒷정리가 없을 때만 불린다' 고 적어
 * 두었지만, 실제로는 디시플린이 자기 erp_postaction 콜백에서 이 함수를 **골라
 * 돌려주는** 방식으로 쓰인다 — dasd_diag.c:503 은 무조건, dasd_fba.c:222 는
 * 직전 단계가 기본 ERP 였을 때만, dasd_eckd.c:3579 는 조건에 따라 돌려준다.
 * 
 * 실행 컨텍스트: dasd.c 의 __dasd_process_erp()(2679줄)를 거쳐서만 불린다.
 * 그 함수를 부르는 자리는 두 곳으로, 하나는 block 태스클릿(softirq),
 * 다른 하나는 동기 요청을 기다리는 프로세스 컨텍스트다. 잠들면 안 된다.
 * 
 * caller: dasd.c 의 __dasd_process_erp() → discipline->erp_postaction 를 거쳐서.
 * callee: BUG_ON(), list_del(), dasd_free_erp_request()(이 파일 위쪽),
 * get_tod_clock().
 * 
 * 에러 경로: 사슬이 아닌 cqr 이 들어오면 맨 앞의 BUG_ON 이 커널을 멈춘다.
 * 그 밖에는 실패할 수 있는 연산이 없다. 복구가 실패로 끝난 경우는 오류가
 * 아니라 정상 경로이며, 뿌리를 DASD_CQR_FAILED 로 두어 상위에 알린다.
 * 
 * 호출 체인:
 *   dasd.c 의 __dasd_process_erp() → discipline->erp_postaction → [이 함수]
 *     → dasd_free_erp_request() → dasd_free_chunk() */
/*
 * DESCRIPTION
 *   Frees all ERPs of the current ERP Chain and set the status
 *   of the original CQR either to DASD_CQR_DONE if ERP was successful
 *   or to DASD_CQR_FAILED if ERP was NOT successful.
 *   NOTE: This function is only called if no discipline postaction
 *	   is available
 *
 * PARAMETER
 *   erp		current erp_head
 *
 * RETURN VALUES
 *   cqr		pointer to the original CQR
 */
struct dasd_ccw_req *dasd_default_erp_postaction(struct dasd_ccw_req *cqr)
{
	/* [한국어] 복구가 성공으로 끝났는지 담아 둘 표지. 아래 반복에서 사슬 맨 앞 cqr 이
	 * 해제되므로 미리 계산해 둬야 한다. */
	int success;
	/* [한국어] 사슬 맨 앞이 가진 시작·종료 시각. 같은 이유로 미리 챙긴다. */
	unsigned long startclk, stopclk;
	/* [한국어] 사슬 맨 앞이 실제로 실행된 장치. PAV 환경에서 복구가 다른 별칭 장치로
	 * 나갔을 수 있어, 그 사실을 뿌리에 옮겨 적어야 통계가 맞는다. */
	struct dasd_device *startdev;

	/* [한국어] 이 cqr 이 정말 복구용인지 확인한다. refers 가 NULL 이면 사슬의 뿌리이고,
	 * function 이 NULL 이면 어떤 ERP 함수도 이것을 만들지 않았다는 뜻이다.
	 * 둘 중 하나라도 비어 있으면 호출자가 뒷정리를 엉뚱한 요청에 걸었다는
	 * 뜻이므로, 조용히 넘어가지 않고 커널을 멈춘다. 아래 반복이 refers 를
	 * 따라가므로, 이 검사가 없으면 뿌리에서 곧바로 반복이 끝나 원래 요청의
	 * 상태를 덮어써 버린다. */
	BUG_ON(cqr->refers == NULL || cqr->function == NULL);

	/* [한국어] 복구의 최종 결과를 표지로 굳힌다. 사슬 맨 앞의 상태가 DASD_CQR_DONE 이면
	 * 마지막 복구 시도가 성공했다는 뜻이고, 그것이 곧 복구 전체의 성공이다. */
	success = cqr->status == DASD_CQR_DONE;
	/* [한국어] 시작 시각을 챙긴다. 아래에서 뿌리에 옮겨 적는다. */
	startclk = cqr->startclk;
	/* [한국어] 종료 시각을 챙긴다. */
	stopclk = cqr->stopclk;
	/* [한국어] 실행 장치를 챙긴다. */
	startdev = cqr->startdev;

	/* free all ERPs - but NOT the original cqr */
	/* [한국어] refers 가 NULL 이 될 때까지, 즉 뿌리에 닿을 때까지 거슬러 올라간다.
	 * 옆의 상류 주석대로 **복구용 cqr 만 풀고 원래 cqr 은 남긴다.**
	 * 반복 조건이 자기 자신이 아니라 자기가 가리키는 앞 요소를 보는 형태라,
	 * 뿌리는 반복 안으로 들어오지 않는다. */
	while (cqr->refers != NULL) {
		/* [한국어] 지금 요소를 풀기 전에 다음(앞) 요소를 담아 둘 자리. 해제한 뒤에는
		 * 그 필드를 읽을 수 없으므로 반드시 먼저 챙겨야 한다. */
		struct dasd_ccw_req *refers;

		/* [한국어] 다음에 볼 요소를 먼저 챙긴다. 이 한 줄이 있어야 아래에서 지금 요소를
		 * 안전하게 해제할 수 있다. */
		refers = cqr->refers;
		/* remove the request from the block queue */
		/* [한국어] 이 복구용 cqr 을 블록 계층 큐에서 뗀다. 복구용 cqr 은 원래 요청을 대신해
		 * block->ccw_queue 에 들어가 있으므로, 풀기 전에 반드시 빼야 한다.
		 * list_del 은 초기화된 빈 고리에 걸어도 안전하며, 위쪽
		 * dasd_alloc_erp_request() 가 이 고리를 초기화해 두는 것이 그 전제다.
		 * [상류 코드 관찰] 잠금을 잡지 않고 이 큐를 만진다. 이 함수를 부르는
		 * dasd.c 의 두 경로가 각각 block 태스클릿 안과 동기 대기 안이라 그 큐를
		 * 동시에 만지는 쪽이 없다는 전제로 보인다. 원본(1f0e418bb6) 140줄에서
		 * 확인했으며 코드는 고치지 않았다. */
		list_del(&cqr->blocklist);
		/* free the finished erp request */
		/* [한국어] 이 복구용 cqr 을 풀에 되돌린다. **memdev 를 넘기는 것** 이 핵심이다 —
		 * 메모리를 준 장치와 요청을 실행한 장치가 다를 수 있으므로, 반드시 준
		 * 장치의 풀로 돌려줘야 한다. */
		dasd_free_erp_request(cqr, cqr->memdev);
		/* [한국어] 커서를 앞 요소로 옮긴다. 반복이 끝나면 이 변수가 뿌리를 가리킨다. */
		cqr = refers;
	}

	/* set corresponding status to original cqr */
	/* [한국어] 뿌리에 시작 시각을 옮겨 적는다. 옆의 상류 주석대로 '해당하는 상태를
	 * 원래 cqr 에 설정' 하는 단계다. 뿌리 자신의 시각은 첫 시도 때 것이라,
	 * 복구를 포함한 실제 구간을 나타내려면 마지막 시도의 값으로 바꿔야 한다. */
	cqr->startclk = startclk;
	/* [한국어] 종료 시각도 옮겨 적는다. */
	cqr->stopclk = stopclk;
	/* [한국어] 실행 장치도 옮겨 적는다. 복구가 다른 별칭 장치로 나갔다면 그 사실이
	 * 여기서 뿌리에 반영된다. */
	cqr->startdev = startdev;
	/* [한국어] 복구가 성공했으면 */
	if (success)
		/* [한국어] 뿌리를 '정상 완료' 로 못 박는다. 상위 계층은 처음부터 아무 일도 없었던
		 * 것처럼 이 요청을 성공으로 받는다. */
		cqr->status = DASD_CQR_DONE;
	/* [한국어] 복구가 끝내 실패했으면 */
	else {
		/* [한국어] 뿌리를 '최종 실패' 로 못 박는다. */
		cqr->status = DASD_CQR_FAILED;
		/* [한국어] 종료 시각을 지금 시각으로 다시 찍는다. 실패 경로에서는 466~467줄이
		 * 옮겨 적은 값 대신 **뒷정리가 끝난 이 순간** 을 종료로 삼는다.
		 * 성공 경로에는 이 줄이 없어, 두 경로의 종료 시각 의미가 다르다. */
		cqr->stopclk = get_tod_clock();
	}

	/* [한국어] 뿌리를 돌려준다. 사슬을 다 푼 뒤라 이 포인터만이 유효한 cqr 이다.
	 * 호출자인 dasd.c 의 __dasd_process_erp() 는 반환값을 쓰지 않지만,
	 * 디시플린 콜백의 형을 맞추기 위해 반환형이 필요하다. */
	return cqr;

}				/* end default_erp_postaction */

/* [한국어]
 * dasd_log_sense - 실패한 요청의 센스 데이터를 콘솔에 남긴다
 * 
 * @cqr: 최종적으로 실패한 요청. startdev 와 intrc 를 읽는다.
 * @irb: 채널이 돌려준 인터럽트 응답 블록(Interrupt Response Block). 호출자들은
 *       모두 cqr 안에 복사해 둔 사본 cqr->irb 의 주소를 넘긴다. 구조체 정의는
 *       arch/s390 소관이라 이 트리에서 확인 못 함.
 * @return: 없다.
 * 
 * I/O 가 끝내 실패했을 때 **사람이 읽을 기록** 을 남기는 자리다. 센스 데이터의
 * 해석은 장치 종류마다 완전히 다르므로, 이 함수는 형식을 정하지 않고
 * 디시플린의 dump_sense 콜백에 넘긴다. 즉 이 함수가 하는 일은 두 가지 판단뿐이다.
 * 
 * 1. **센스가 아예 없는 두 경우를 먼저 걸러 낸다.** 요청이 시간 초과로 끝났거나
 *    (intrc 가 -ETIMEDOUT) 링크가 끊겨 끝났으면(-ENOLINK), 하드웨어가 상태를
 *    돌려준 적이 없으므로 irb 안에 볼 것이 없다. 그때는 한 줄만 찍고 돌아간다.
 * 2. 그 밖의 경우에만 디시플린의 덤프 콜백을 부른다. 디시플린이 없거나 콜백을
 *    두지 않았으면 아무것도 하지 않는다 — 조용히 넘어가는 것이 맞는다.
 * 
 * 실행 컨텍스트: dasd.c 의 세 자리에서 불린다. __dasd_sleep_on()(2354줄)과
 * __dasd_sleep_on_erp()(2252줄)은 프로세스 컨텍스트, __dasd_process_block_ccw_queue()
 * (2780줄)는 block 태스클릿(softirq)이다. 콘솔 출력은 느리므로 실패한 요청에
 * 대해서만 부른다.
 * 
 * caller: 위 세 자리. 모두 cqr->status 가 DASD_CQR_FAILED 인 것을 확인한 뒤 부른다.
 * callee: dev_err(), 그리고 device->discipline->dump_sense — 그 실체는
 * dasd_eckd.c/dasd_fba.c/dasd_diag.c 가 각각 채운다.
 * 
 * 에러 경로: 이 함수 자체가 실패하지는 않는다. 콜백이 없으면 아무 기록도
 * 남지 않는데, 그 경우를 오류로 보지 않는 것이 상류의 선택이다.
 * 
 * 호출 체인:
 *   dasd.c 의 실패 처리 → [이 함수] → discipline->dump_sense() */
void
dasd_log_sense(struct dasd_ccw_req *cqr, struct irb *irb)
{
	/* [한국어] 장치를 담아 둘 지역 변수. 오류 메시지의 대상과 콜백의 인자로 쓴다. */
	struct dasd_device *device;

	/* [한국어] 요청을 실제로 내보낸 장치를 꺼낸다. 오류가 난 곳이 그 장치이므로,
	 * 메모리를 준 memdev 가 아니라 startdev 여야 한다. */
	device = cqr->startdev;
	/* [한국어] 드라이버 내부 오류 코드가 시간 초과인지 본다. **채널이 아니라 드라이버가
	 * 직접 붙인 값** 이며, 만료 타이머가 돌아 요청을 강제로 끝낸 경우다. */
	if (cqr->intrc == -ETIMEDOUT) {
		/* [한국어] 콘솔에 한 줄만 남긴다. 하드웨어가 상태를 돌려준 적이 없어 센스가 없으므로
		 * 덤프할 것이 없다.
		 * [상류 코드 관찰] 형식 지정자가 %px 라 커널 주소가 해싱 없이 그대로 찍힌다.
		 * %p 는 보안상 주소를 가리는데, 여기서는 요청을 추적하려고 일부러 실제 값을
		 * 쓴다. 아래 -ENOLINK 경로도 같다. 원본(1f0e418bb6) 169줄과 174줄에서
		 * 확인했으며 코드는 고치지 않았다. */
		dev_err(&device->cdev->dev,
			"A timeout error occurred for cqr %px\n", cqr);
		/* [한국어] 센스가 없으므로 덤프 콜백까지 가지 않고 여기서 끝낸다. */
		return;
	}
	/* [한국어] 내부 오류 코드가 전송 오류인지 본다. 요청을 내보낼 경로가 하나도 남지
	 * 않았거나 링크가 끊긴 경우이며, 이때도 하드웨어의 상태 보고가 없다. */
	if (cqr->intrc == -ENOLINK) {
		/* [한국어] 같은 이유로 한 줄만 남긴다. */
		dev_err(&device->cdev->dev,
			"A transport error occurred for cqr %px\n", cqr);
		/* [한국어] 여기서도 덤프 없이 끝낸다. */
		return;
	}
	/* dump sense data */
	/* [한국어] 디시플린이 붙어 있고 그 디시플린이 덤프 콜백을 두었는지 확인한다.
	 * **두 검사가 모두 필요한 이유** 는, 장치 상태 기계의 아래쪽 단계에서는
	 * 디시플린이 아직 정해지지 않았을 수 있고, 정해졌더라도 콜백을 두지 않은
	 * 디시플린이 있을 수 있기 때문이다. */
	if (device->discipline && device->discipline->dump_sense)
		/* [한국어] 센스 해석과 출력 형식을 디시플린에 통째로 맡긴다. 3990 계열 제어 장치의
		 * 센스 32바이트를 규격대로 풀어 쓰는 코드가 dasd_eckd.c 안에 있으며,
		 * 장치 종류마다 그 형식이 다르므로 이 파일에는 형식이 하나도 없다. */
		device->discipline->dump_sense(device, cqr, irb);
}

/* [한국어]
 * dasd_log_sense_dbf - 같은 센스 데이터를 s390 디버그 영역에 남긴다
 * 
 * @cqr: 대상 요청. startdev 만 읽는다.
 * @irb: 채널이 돌려준 인터럽트 응답 블록. 콜백에 그대로 넘긴다.
 * @return: 없다.
 * 
 * 위 dasd_log_sense() 의 값싼 짝이다. 차이는 **어디에 남기느냐** 하나다.
 * 콘솔은 느리고 로그를 넘치게 하므로 최종 실패에만 쓸 수 있지만, s390 의
 * 디버그 기능(dbf)은 장치마다 잡아 둔 순환 버퍼에 쓰기만 하므로 훨씬 자주
 * 불러도 부담이 적다. 그래서 복구가 진행 중인 중간 오류처럼 콘솔에 올릴
 * 정도는 아닌 사건을 여기에 남긴다.
 * 
 * 콜백에 넘기는 세 번째 인자가 고정된 문자열 "log" 인 것은, 같은 덤프 콜백을
 * 여러 자리에서 부르면서 **어느 자리에서 불렀는지 꼬리표를 붙이기 위해서** 다.
 * 그 꼬리표가 순환 버퍼에 함께 찍힌다.
 * 
 * cqr 을 받으면서 정작 쓰는 것은 cqr->startdev 하나뿐이고, irb 는 인자로 받은
 * 것을 그대로 넘긴다 — 즉 이 함수는 장치를 찾아 콜백으로 이어 주는 얇은 다리다.
 * 
 * 실행 컨텍스트: 디시플린 코드에서 불리며, 태스클릿과 프로세스 컨텍스트 양쪽에서
 * 불릴 수 있다. dbf 기록은 잠들지 않으므로 인터럽트 문맥에서도 안전하다.
 * 
 * caller: 이 디렉터리 안에서 이 함수를 직접 부르는 자리는 없다. EXPORT_SYMBOL 로
 * 내보내 두어 별도 모듈로 빌드된 디시플린이 쓸 수 있게 해 둔 것이다.
 * callee: device->discipline->dump_sense_dbf — 그 실체는 각 디시플린이 채운다.
 * 
 * 에러 경로: 없다. 디시플린이나 콜백이 없으면 조용히 아무것도 하지 않는다.
 * 
 * 호출 체인:
 *   디시플린의 오류 처리 → [이 함수] → discipline->dump_sense_dbf() */
void
dasd_log_sense_dbf(struct dasd_ccw_req *cqr, struct irb *irb)
{
	struct dasd_device *device;

	/* [한국어] 장치를 담아 둘 지역 변수. */
	device = cqr->startdev;
	/* dump sense data to s390 debugfeature*/
	/* [한국어] 요청을 내보낸 장치를 꺼낸다. 그 장치의 디버그 영역에 기록이 남는다. */
	if (device->discipline && device->discipline->dump_sense_dbf)
		device->discipline->dump_sense_dbf(device, irb, "log");
/* [한국어] 위 dasd_log_sense() 와 같은 두 겹 검사. 디시플린이 있고 dbf 덤프 콜백을
 * 두었을 때만 부른다. */
}
/* [한국어] 디시플린에 기록을 맡긴다. 세 번째 인자 "log" 는 이 호출 지점을 나타내는
 * 꼬리표로, 순환 버퍼에서 어느 경로가 남긴 기록인지 가려 준다.
 * cqr 을 넘기지 않고 irb 만 넘기는 것은, dbf 판 덤프가 인터럽트 응답 블록의
 * 내용만 요약해 찍으면 되기 때문이다. */
EXPORT_SYMBOL(dasd_log_sense_dbf);

/* [한국어] dasd_log_sense_dbf 를 모듈 밖으로 내보낸다.
 * [상류 코드 관찰] 이 파일의 다른 다섯 개는 파일 맨 아래에 모여 있는데
 * 이것 하나만 정의 바로 뒤에 떨어져 있다. 나중에 더해진 함수의 흔적으로
 * 보이며 동작에는 차이가 없다. 원본(1f0e418bb6) 192줄에서 확인했으며
 * 코드는 고치지 않았다. */
EXPORT_SYMBOL(dasd_default_erp_action);
EXPORT_SYMBOL(dasd_default_erp_postaction);
/* [한국어] 기본 복구 동작을 내보낸다. 디시플린 모듈(dasd_eckd_mod 등)이 자기
 * erp_action 콜백에서 이 주소를 돌려주려면 이름을 볼 수 있어야 한다. */
EXPORT_SYMBOL(dasd_alloc_erp_request);
/* [한국어] 기본 뒷정리도 같은 이유로 내보낸다. dasd_fba.c 는 이 주소를 돌려주는 데
 * 더해 cqr->function 과 비교하는 데도 쓴다. */
EXPORT_SYMBOL(dasd_free_erp_request);
/* [한국어] 복구용 요청 할당기를 내보낸다. dasd_3990_erp.c 가 별도 모듈로 빌드될 때
 * 필요하다. */
EXPORT_SYMBOL(dasd_log_sense);
/* [한국어] 그 짝인 해제 함수도 내보낸다. */

/* [한국어] 콘솔 센스 덤프를 내보낸다. 디시플린이 자기 오류 경로에서 부를 수 있게
 * 공개해 둔 것이다. */
