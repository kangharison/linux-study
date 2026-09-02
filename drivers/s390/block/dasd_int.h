/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *		    Horst Hummel <Horst.Hummel@de.ibm.com>
 *		    Martin Schwidefsky <schwidefsky@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2009
 */

/* [한국어] [한국어 설명] s390 DASD 드라이버의 중앙 헤더 (dasd_int.h)
 * 
 * === 파일의 역할 ===
 * DASD(Direct Access Storage Device)는 IBM 메인프레임(S/390, z/Architecture)에
 * 붙는 디스크 장치를 가리키는 이름이다. 이 헤더는 그 드라이버를 이루는
 * drivers/s390/block/ 의 모든 .c 파일이 공유하는 **어휘 전부** 를 한자리에 모아
 * 둔 곳이다. 장치를 나타내는 자료구조 둘(struct dasd_device 와 struct
 * dasd_block), I/O 한 건을 나타내는 자료구조 하나(struct dasd_ccw_req), 장치
 * 종류별 동작을 갈아 끼우는 가상 함수표(struct dasd_discipline), 그리고 장치
 * 상태 기계와 요청 상태 기계의 상수들이 여기서 정의된다.
 * 여기에 더해, 채널 경로(channel path) 8개의 상태를 비트로 다루는 인라인 함수
 * 67개가 전부 이 헤더 안에 본문까지 들어 있다. 즉 이 파일은 선언만 모아 둔
 * 헤더가 아니라, **경로 관리 로직 자체를 담고 있는 구현 파일** 이기도 하다.
 * DASD 서브시스템을 읽을 때 가장 먼저 읽어야 하는 파일이며, 이 한 파일만으로
 * 장치 모델과 요청 모델을 이해할 수 있도록 아래에 그 골격을 적어 둔다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 블록 계층 아래, s390 채널 서브시스템(channel subsystem) 위에 있다.
 * 위에서 아래로 훑으면 이렇다.
 * 
 *   파일시스템 / 사용자 공간
 *     -> blk-mq (block/blk-mq.c)
 *       -> dasd_mq_ops.queue_rq == dasd.c 의 do_dasd_request()
 *         -> discipline->build_cp() 가 struct dasd_ccw_req 를 만든다
 *           -> block->ccw_queue 에 매달린다      [블록 계층 큐]
 *             -> dasd_block_tasklet 이 device->ccw_queue 로 옮긴다
 *               -> dasd_device_tasklet 이 dasd_start_IO() 를 부른다
 *                 -> discipline->start_IO() -> ccw_device_start() (arch/s390)
 *                   -> 채널 서브시스템이 CCW 사슬을 하드웨어에 실행시킨다
 * 
 * 완료는 반대 방향으로 인터럽트를 타고 올라온다.
 * 
 *   채널 서브시스템 인터럽트
 *     -> dasd.c 의 dasd_int_handler()   [인터럽트 컨텍스트, cdev 락 보유]
 *       -> cqr->status 를 DASD_CQR_DONE 또는 DASD_CQR_ERROR 로 바꾼다
 *         -> dasd_schedule_device_bh() 로 device tasklet 을 깨운다
 *           -> 오류면 discipline->erp_action() 이 복구용 cqr 을 새로 만든다
 *             -> 정상 완료면 cqr->callback() -> blk_mq_end_request()
 * 
 * **두 겹의 큐와 두 개의 tasklet** 이 이 드라이버의 뼈대다. block->ccw_queue 는
 * 블록 계층 요청을 담고, device->ccw_queue 는 실제로 채널에 내보낼 CCW 요청을
 * 담는다. 둘을 나눈 이유는 PAV(Parallel Access Volume) 때문이다. 하나의 논리
 * 볼륨(dasd_block)에 여러 개의 별칭 장치(dasd_device)가 붙어 동시에 I/O 를
 * 낼 수 있어야 하므로, 요청을 만드는 자리와 내보내는 자리를 떼어 놓았다.
 * 
 * 실행 컨텍스트는 셋이 섞인다. build_cp 와 sysfs 경로는 프로세스 컨텍스트,
 * dasd_int_handler 는 인터럽트 컨텍스트, 두 tasklet 은 softirq 컨텍스트다.
 * 이 헤더의 인라인 함수들은 세 곳 모두에서 불리므로 잠들 수 없다.
 * 
 * === 타 모듈과의 연결 ===
 * 이 헤더를 포함하는 파일들과 각자의 역할은 다음과 같다.
 * 
 *   dasd.c          코어. 장치 상태 기계, 두 tasklet, 인터럽트 처리기,
 *                   cqr 할당기, blk-mq 연결. 이 헤더가 선언한 전역 대부분의 정의처.
 *   dasd_devmap.c   busid("0.0.1234") 와 dasd_device 의 대응표, sysfs 속성 전부,
 *                   copy pair(PPRC) 설정. dasd_create_device/dasd_delete_device.
 *   dasd_genhd.c    gendisk 생성과 파티션 스캔. (헤더 주석은 dasd_gendisk.c 라고
 *                   적혀 있지만 실제 파일명은 dasd_genhd.c 다.)
 *   dasd_ioctl.c    BIODASD 계열 ioctl.
 *   dasd_proc.c     /proc/dasd/devices, /proc/dasd/statistics.
 *   dasd_erp.c      discipline 이 자기 ERP 를 내놓지 않을 때 쓰는 기본 ERP.
 *   dasd_3990_erp.c 3990 제어 장치의 센스 데이터를 해석하는 대규모 ERP 상태 기계.
 *   dasd_eer.c      EER(extended error reporting) 문자 장치. 오류를 사용자 공간
 *                   데몬에게 올린다.
 *   dasd_alias.c    PAV 별칭 장치의 그룹 관리(LCU 단위).
 *   dasd_eckd.c     ECKD(Extended Count Key Data) discipline — 실제 메인프레임
 *                   디스크. 이 디렉터리에서 가장 큰 파일이다.
 *   dasd_fba.c      FBA(Fixed Block Architecture) discipline — 고정 블록 장치.
 *   dasd_diag.c     z/VM 의 DIAG 250 인터페이스를 쓰는 discipline.
 * 
 * 아래쪽으로는 s390 아키텍처 계층에 기댄다. struct ccw_device, struct ccw1,
 * struct irb, ccw_device_start(), IDAL(Indirect Data Address List) 처리,
 * debug_sprintf_event() 가 모두 arch/s390 소관이다.
 * **이 트리는 sparse checkout 이라 arch/s390 이 없다.** 따라서 그 계층의 구체적
 * 동작(CCW 실행 규칙, 인터럽트 전달 방식, 진단 명령 인터페이스)은 이 트리에서
 * 확인 못 함으로 적었다. 확인 가능한 것은 이 디렉터리 안의 사용 방식뿐이다.
 * 
 * 데이터 흐름은 한 방향으로 정리된다. 블록 계층의 struct request 가
 * build_cp() 에서 struct dasd_ccw_req 로 바뀌고, 그 안의 cpaddr 이 가리키는
 * CCW 사슬(또는 전송 모드일 때는 TCW)이 하드웨어가 읽는 최종 형태다.
 * 반대로 완료 상태는 struct irb 로 돌아와 cqr->irb 에 복사되고, 그 안의 센스
 * 데이터를 ERP 가 해석한다.
 * 
 * === 주요 함수/구조체 요약 ===
 * struct dasd_ccw_req    I/O 한 건. 흔히 'cqr' 이라 부른다. CCW 사슬 주소,
 *                        상태, 재시도 횟수, ERP 사슬 연결(refers), 네 개의
 *                        TOD 시계 눈금을 담는다.
 * struct dasd_device     장치 하나. ccw_device 와 1:1 이며, CCW 큐와 정적
 *                        메모리 풀, 경로 8개의 상태, 상태 기계 변수를 갖는다.
 * struct dasd_block      블록 장치 하나. gendisk 와 blk-mq 태그셋을 갖는다.
 *                        PAV 별칭 장치는 이것을 갖지 않는다.
 * struct dasd_discipline ECKD/FBA/DIAG 가 각각 채우는 가상 함수표. build_cp,
 *                        start_IO, erp_action 등 40여 개의 콜백.
 * struct dasd_path       채널 경로 하나의 상태. device->path[8] 로 배열이다.
 * struct dasd_mchunk     정적 메모리 풀의 자유 덩어리 헤더. 인터럽트 문맥에서도
 *                        cqr 을 할당할 수 있게 미리 잡아 둔 페이지를 쪼갠다.
 * dasd_get_device()      참조 계수를 올린다. 이 헤더의 인라인.
 * dasd_alloc_chunk()     정적 풀에서 메모리를 떼어 온다. 이 헤더의 인라인.
 * dasd_path_ 계열        경로 8개의 비트 상태를 읽고 쓰는 인라인 46개.
 *                        비트 순서는 항상 0x80 을 chp 만큼 오른쪽으로 민 것이며,
 *                        경로 0 이 최상위 비트다(채널 서브시스템의 lpm 규약).
 * 
 * === 이 파일을 읽을 때 알아 두면 좋은 약어 ===
 * CCW   Channel Command Word. 채널이 실행하는 명령 하나. 사슬로 이어 쓴다.
 * CQR   이 드라이버가 struct dasd_ccw_req 를 부르는 이름.
 * TCW   Transport Control Word. 고성능 FICON(HPF)에서 CCW 사슬 대신 쓰는 형식.
 * IRB   Interrupt Response Block. 완료 상태와 센스 데이터를 담아 오는 구조체.
 * ERP   Error Recovery Procedure. 실패한 cqr 을 복구용 cqr 로 감싸 다시 시도한다.
 * PAV   Parallel Access Volume. 한 볼륨에 별칭 장치를 여러 개 붙여 병렬 I/O.
 * LCU   Logical Control Unit. PAV 별칭을 묶는 단위.
 * LPM   Logical Path Mask. 8개 경로를 한 바이트 비트마스크로 나타낸 것.
 * CUIR  Control Unit Initiated Reconfiguration. 제어 장치가 경로를 내리라고
 *       먼저 알려 오는 절차.
 * ESE   Extent Space Efficient. 씬 프로비저닝된 볼륨.
 * PPRC  Peer-to-Peer Remote Copy. 원격 복제 쌍.
 * SNSS  Sense Subsystem Status. EER 이 쓰는 상태 수집 명령. */
#ifndef DASD_INT_H
/* [한국어] 헤더 중복 포함 방지용 가드 심볼. 이 헤더는 drivers/s390/block/ 의 거의 모든
 * .c 파일이 포함하므로 가드가 없으면 재정의 오류가 난다.
 * [상류 코드 관찰] 파일 맨 끝의 닫는 지시자에는 'DASD_H' 라고 적혀 있어
 * 가드 이름과 어긋난다. 주석일 뿐이라 동작에는 영향이 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#define DASD_INT_H

/* we keep old device allocation scheme; IOW, minors are still in 0..255 */
/* [한국어] 메이저 번호 하나에 담을 수 있는 DASD 장치 개수.
 * MINORBITS(20, include/linux/kdev_t.h)에서 파티션에 쓰는 비트 수를 빼면
 * 장치 인덱스에 남는 비트 수가 되고, 그만큼이 장치 개수 상한이다.
 * DASD_PARTN_BITS 는 asm/dasd.h 소관인데 이 트리에 arch/s390 이 없어
 * 그 값은 이 트리에서 확인 못 함. 다만 상단의 상류 주석이 '마이너 번호는
 * 여전히 0..255' 라고 적어 두었으므로 2 임을 짐작할 수 있다.
 * 읽는 자: dasd_genhd.c 의 dasd_gendisk_alloc() 이 장치 인덱스가 이 값을
 * 넘으면 -EBUSY 로 거절한다. */
#define DASD_PER_MAJOR (1U << (MINORBITS - DASD_PARTN_BITS))
/* [한국어] 마이너 번호에서 파티션 번호만 남기는 비트 마스크.
 * [상류 코드 관찰] 이 트리 어디에서도 쓰이지 않는다. 파티션 번호를 직접
 * 계산하던 옛 코드의 잔재로 보이며, 지금은 gendisk 의 first_minor/minors 를
 * 블록 계층이 대신 계산한다. 원본(1f0e418bb6) 15줄에서 확인했으며 코드는
 * 고치지 않았다. */
#define DASD_PARTN_MASK ((1 << DASD_PARTN_BITS) - 1)

/*
 * States a dasd device can have:
 *   new: the dasd_device structure is allocated.
 *   known: the discipline for the device is identified.
 *   basic: the device can do basic i/o.
 *   unfmt: the device could not be analyzed (format is unknown).
 *   ready: partition detection is done and the device is can do block io.
 *   online: the device accepts requests from the block device queue.
 *
 * Things to do for startup state transitions:
 *   new -> known: find discipline for the device and create devfs entries.
 *   known -> basic: request irq line for the device.
 *   basic -> ready: do the initial analysis, e.g. format detection,
 *                   do block device setup and detect partitions.
 *   ready -> online: schedule the device tasklet.
 * Things to do for shutdown state transitions:
 *   online -> ready: just set the new device state.
 *   ready -> basic: flush requests from the block device layer, clear
 *                   partition information and reset format information.
 *   basic -> known: terminate all requests and free irq.
 *   known -> new: remove devfs entries and forget discipline.
 */

/* [한국어] 장치 상태 기계의 0단계. struct dasd_device 를 할당만 한 상태다.
 * 설정자: dasd.c 의 dasd_alloc_device() 가 state 와 target 을 모두 이 값으로
 * 초기화한다. dasd_state_known_to_new() 가 내려올 때도 이 값으로 되돌린다.
 * 이 단계에서는 어떤 discipline 이 이 장치를 다룰지도 아직 모른다. */
#define DASD_STATE_NEW	  0
/* [한국어] 1단계. 이 장치를 다룰 discipline(ECKD/FBA/DIAG)이 정해진 상태다.
 * 설정자: dasd.c 의 dasd_state_new_to_known(). 아직 인터럽트 처리기를
 * 걸지 않았으므로 I/O 는 할 수 없다. */
#define DASD_STATE_KNOWN  1
/* [한국어] 2단계. 인터럽트 처리기를 걸어 기본 I/O(장치 특성 읽기 등)를 할 수 있다.
 * 설정자: dasd.c 의 dasd_state_known_to_basic(). 이 단계에서 debugfs 항목과
 * 장치 디버그 영역이 만들어지고 ccw_device_set_online() 이 불린다.
 * 블록 계층에는 아직 노출되지 않는다. */
#define DASD_STATE_BASIC  2
/* [한국어] 곁가지 단계. do_analysis() 가 -EMEDIUMTYPE 을 돌려줘 볼륨 형식을 알아내지
 * 못했을 때 들어간다. BASIC 과 READY 사이에 끼어 있지만 정상 경로가 아니다.
 * 설정자: dasd.c:314 의 dasd_state_basic_to_ready()가 분석 실패 시 설정한다.
 * 읽는 자: dasd_proc.c 가 'unformatted' 로 표시하고, dasd_devmap.c 의
 * sysfs status 속성이 같은 문자열을 내놓는다. 이 상태에서는 포맷 ioctl 만
 * 받을 수 있고 일반 블록 I/O 는 거절된다. */
#define DASD_STATE_UNFMT  3
/* [한국어] 4단계. 볼륨 형식을 알아냈고 gendisk 를 만들어 파티션 스캔까지 끝난 상태다.
 * 설정자: dasd.c 의 dasd_state_basic_to_ready(). 블록 장치 노드는 생겼지만
 * 아직 요청 큐에서 요청을 꺼내 처리하지는 않는다. */
#define DASD_STATE_READY  4
/* [한국어] 마지막 5단계. 블록 계층 요청을 실제로 받아 처리한다.
 * 설정자: dasd.c 의 dasd_state_ready_to_online(). 이 단계에서 block tasklet 이
 * 예약되고 디스크가 사용자에게 보인다.
 * 값이 커질수록 더 많은 기능이 열리는 단조 증가 척도라, dasd_increase_state()/
 * dasd_decrease_state() 가 state 와 target 을 단순 비교해 한 칸씩 오르내린다. */
#define DASD_STATE_ONLINE 5

/* [한국어] struct module 과 MODULE_ 계열 매크로. struct dasd_discipline 의 owner 필드가
 * struct module 포인터라 이 헤더에 필요하다. */
#include <linux/module.h>
/* [한국어] wait_queue_head_t 와 wait_event 계열. 참조 계수가 0이 될 때까지 기다리는
 * dasd_delete_device() 흐름과 sleep_on 계열이 이 기능에 기댄다. */
#include <linux/wait.h>
/* [한국어] struct request, struct gendisk, blk_mode_t 등 블록 계층 기본 타입.
 * build_cp() 콜백이 struct request 를 받으므로 반드시 필요하다. */
#include <linux/blkdev.h>
/* [한국어] struct hd_geometry. discipline 의 fill_geometry() 콜백이 이 구조체에
 * 실린더/헤드/섹터 수를 채워 HDIO_GETGEO ioctl 에 답한다. */
#include <linux/hdreg.h>
/* [한국어] struct tasklet_struct 와 tasklet 조작 함수들. 이 드라이버의 두 tasklet
 * (device tasklet, block tasklet)이 struct dasd_device / struct dasd_block 에
 * 직접 박혀 있어 — 포인터가 아니라 값으로 품는다 — 정의가 필요하다.
 * dasd.c 가 tasklet_init() 으로 초기화하고(:120, :160), 예약할 때는 보통
 * 판이 아니라 **높은 우선순위 판** 인 tasklet_hi_schedule() 을 쓴다
 * (:2150, :3005) — I/O 완료 처리를 다른 tasklet 보다 앞세우려는 것이다. */
#include <linux/interrupt.h>
/* [한국어] is_power_of_2(). 아래 dasd_check_blocksize() 가 블록 크기가 2의 거듭제곱인지
 * 확인할 때 쓴다. */
#include <linux/log2.h>
/* [한국어] struct ccw_device, struct ccw1, get_ccwdev_lock(), ccw_device_get_id().
 * 이 드라이버가 하드웨어와 만나는 유일한 접점이 CCW 장치다.
 * arch/s390 소관이라 이 트리에는 없다 — 실제 정의는 확인 못 함. */
#include <asm/ccwdev.h>
/* [한국어] struct work_struct 와 INIT_WORK(). 잠들 수 있어야 하는 일(장치 상태 전이,
 * 경로 검증, 요청 재큐잉)을 tasklet 대신 워크큐로 미루는 데 쓴다. */
#include <linux/workqueue.h>
/* [한국어] debug_info_t 와 debug_sprintf_event(). s390 고유의 순환 디버그 로그(dbf)
 * 기능이며, 아래 DBF 계열 매크로가 전부 이것을 감싼다.
 * arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/debug.h>
/* [한국어] 사용자 공간과 주고받는 DASD 전용 타입들 — struct dasd_information2_t,
 * struct format_data_t, struct format_check_t, DASD_PARTN_BITS,
 * DASD_FEATURE_ 계열 비트가 여기서 온다. arch/s390 소관이라 이 트리에 없다. */
#include <asm/dasd.h>
/* [한국어] IDAL(Indirect Data Address List) 관련 도우미. CCW 하나가 가리킬 수 있는
 * 주소 범위 제약을 넘기 위해 간접 주소 목록을 만드는 s390 고유 기법이다.
 * arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/idals.h>
/* [한국어] test_bit/__set_bit/set_bit/clear_bit. 이 헤더의 경로 상태 함수 46개가 전부
 * 이 원자적(또는 비원자적) 비트 연산 위에 세워져 있다. */
#include <linux/bitops.h>
/* [한국어] struct blk_mq_tag_set, struct blk_mq_ops, enum blk_eh_timer_return.
 * struct dasd_block 이 태그셋을 통째로 품고 있어 정의가 필요하다. */
#include <linux/blk-mq.h>

/* DASD discipline magic */
/* [한국어] ECKD discipline 의 눈표(eye catcher). 바이트를 EBCDIC 으로 읽으면
 * 0xC5='E', 0xC3='C', 0xD2='K', 0xC4='D' 라서 'ECKD' 가 된다.
 * 설정자: dasd_eckd.c 가 cqr 을 만들 때 cqr->magic 에 넣는다.
 * 읽는 자: dasd.c:1253/1507/1687 이 strncmp 로 discipline->ebcname 과
 * 비교해, 다른 discipline 의 cqr 이 잘못 흘러들어 왔는지 걸러 낸다.
 * ebcname 은 초기화 시점에 ASCEBC() 로 EBCDIC 으로 바뀌므로 두 값이 맞는다. */
#define DASD_ECKD_MAGIC 0xC5C3D2C4
/* [한국어] DIAG discipline 의 눈표. EBCDIC 으로 0xC4='D', 0xC9='I', 0xC1='A',
 * 0xC7='G' 이므로 'DIAG' 다. z/VM 게스트가 하이퍼바이저의 진단 명령으로
 * 디스크에 접근할 때 쓰는 discipline 을 가리킨다.
 * 설정자/읽는 자: dasd_diag.c 안에서만 쓰인다. */
#define DASD_DIAG_MAGIC 0xC4C9C1C7
/* [한국어] FBA discipline 의 눈표. EBCDIC 으로 0xC6='F', 0xC2='B', 0xC1='A' 이고
 * 0x40 은 EBCDIC 의 빈칸이라 'FBA ' 가 된다. 세 글자뿐이라 네 번째를 빈칸으로
 * 채운 것이며, dasd_fba.c 의 ebcname 도 "FBA " 로 빈칸을 포함한다.
 * 설정자/읽는 자: dasd_fba.c 안에서만 쓰인다. */
#define DASD_FBA_MAGIC 0xC6C2C140

/*
 * SECTION: Type definitions
 */
/* [한국어] 전방 선언. 아래 struct dasd_ccw_req 가 dasd_device 포인터를 여러 개 갖고,
 * dasd_device 도 dasd_block 을 가리키므로 서로 순환 참조가 된다.
 * 실제 정의는 이 파일 아래쪽에 있다. */
struct dasd_device;
/* [한국어] 전방 선언. 위와 같은 순환 참조를 끊기 위한 것이다.
 * 실제 정의도 이 파일 아래쪽에 있다. */
struct dasd_block;

/* BIT DEFINITIONS FOR SENSE DATA */
/* [한국어] 센스 바이트의 0번 비트를 고르는 마스크. IBM 은 비트를 왼쪽부터 세므로
 * 0번 비트가 최상위 비트(0x80)다 — 리눅스의 관례와 반대라 헷갈리기 쉽다.
 * 읽는 자: dasd_3990_erp.c 가 sense[25]/sense[27]/sense[2] 에, dasd_eckd.c 가
 * sense[27] 과 irb->ecw[27] 에 이 마스크를 씌워 조건을 가른다.
 * 같은 비트라도 어느 바이트에 씌우느냐에 따라 뜻이 완전히 달라진다. */
#define DASD_SENSE_BIT_0 0x80
/* [한국어] 센스 바이트의 1번 비트(0x40).
 * 읽는 자: dasd_3990_erp.c:1982 한 곳뿐이며, sense[25] 의 이 비트와
 * sense[26] 의 2번 비트를 함께 봐 특정 오류 조합을 가려낸다. */
#define DASD_SENSE_BIT_1 0x40
/* [한국어] 센스 바이트의 2번 비트(0x20).
 * 읽는 자: dasd_3990_erp.c:1934, 1982 와 dasd_eckd.c:225 에서 쓴다. */
#define DASD_SENSE_BIT_2 0x20
/* [한국어] 센스 바이트의 3번 비트(0x10).
 * 읽는 자: dasd_3990_erp.c:1900 과 dasd_eckd.c:231 에서 쓴다.
 * 이 네 개 말고 4~7번 비트에 해당하는 매크로는 정의되어 있지 않다 —
 * 필요한 것만 그때그때 더한 흔적이다. */
#define DASD_SENSE_BIT_3 0x10

/* BIT DEFINITIONS FOR SIM SENSE */
/* [한국어] SIM(Service Information Message) 센스인지 가리는 마스크이자 기댓값.
 * SIM 은 장치가 '지금 당장 I/O 는 되지만 정비가 필요하다' 고 알려 오는
 * 정보성 메시지다.
 * 읽는 자: dasd_3990_erp.c:2090 과 dasd_eckd.c:3641 이 (sense[6] & 0x0F) 가
 * 0x0F 와 같은지 봐 SIM 여부를 판정한다. 마스크와 기댓값이 같은 값이라
 * '하위 4비트가 모두 1' 이라는 뜻이 된다. */
#define DASD_SIM_SENSE 0x0F
/* [한국어] SIM 센스 바이트 24 안에서 '운영자에게 알려야 하는 메시지' 를 뜻하는 비트들.
 * 읽는 자: dasd_3990_erp.c:2053 이 이 비트가 서면 콘솔에 경고를 찍는다. */
#define DASD_SIM_MSG_TO_OP 0x03
/* [한국어] SIM 센스 바이트 24 안에서 '로그만 남기면 되는 메시지' 를 뜻하는 비트들.
 * 읽는 자: dasd_3990_erp.c:2058 이 MSG_TO_OP 가 아닐 때 이것을 확인해
 * 조용히 로그만 남긴다. */
#define DASD_SIM_LOG 0x0C

/* lock class for nested cdev lock */
/* [한국어] 중첩된 ccw_device 락에 붙이려던 lockdep 하위 클래스 번호(첫 번째).
 * [상류 코드 관찰] 이 트리 어디에서도 쓰이지 않는다. 두 장치의 cdev 락을
 * 동시에 잡는 코드에 spin_lock_nested() 를 쓰려던 흔적으로 보인다.
 * 원본(1f0e418bb6) 84줄에서 확인했으며 코드는 고치지 않았다. */
#define CDEV_NESTED_FIRST 1
/* [한국어] 위와 짝이 되는 두 번째 lockdep 하위 클래스 번호.
 * [상류 코드 관찰] 마찬가지로 이 트리 어디에서도 쓰이지 않는다.
 * 원본(1f0e418bb6) 85줄에서 확인했으며 코드는 고치지 않았다. */
#define CDEV_NESTED_SECOND 2

/*
 * SECTION: MACROs for klogd and s390 debug feature (dbf)
 */
/* [한국어] 장치별 디버그 로그 한 줄을 남기는 매크로.
 * @d_level: 아래 DBF 레벨 상수 중 하나. 값이 작을수록 심각하다.
 * @d_device: struct dasd_device 포인터. 이 장치 전용 순환 버퍼에 기록한다.
 * @d_str: printf 형식 문자열. 매크로가 뒤에 개행을 붙여 준다.
 * @d_data: 형식 문자열에 들어갈 가변 인자.
 * 장치마다 debug_area 를 따로 두는 이유는, 한 장치의 오류 흐름을 다른 장치의
 * 잡음과 섞이지 않게 보기 위해서다. debug_sprintf_event() 는 s390 의 dbf
 * 기능이며 잠들지 않으므로 인터럽트 컨텍스트에서도 부를 수 있다.
 * do/while(0) 로 감싼 것은 if 문 뒤에 세미콜론과 함께 써도 안전하게 하려는
 * 커널의 통상 관례다.
 * 쓰는 자: dasd.c, dasd_eckd.c, dasd_3990_erp.c 등 전 파일에 걸쳐 159곳. */
#define DBF_DEV_EVENT(d_level, d_device, d_str, d_data...) \
do { \
	debug_sprintf_event(d_device->debug_area, \
			    d_level, \
			    d_str "\n", \
			    d_data); \
} while(0)

/* [한국어] 드라이버 전역 디버그 로그 한 줄을 남기는 매크로.
 * @d_level: DBF 레벨 상수.
 * @d_str: printf 형식 문자열. 개행은 매크로가 붙인다.
 * @d_data: 가변 인자.
 * 위 DBF_DEV_EVENT 와 달리 장치를 특정하지 않고 전역 영역인 dasd_debug_area 에
 * 쓴다. 아직 장치 구조체가 없거나(초기화 중) 장치와 무관한 사건에 쓴다.
 * 쓰는 자: dasd_devmap.c, dasd.c, dasd_diag.c, dasd_eer.c. */
#define DBF_EVENT(d_level, d_str, d_data...)\
do { \
	debug_sprintf_event(dasd_debug_area, \
			    d_level,\
			    d_str "\n", \
			    d_data); \
} while(0)

/* [한국어] 장치 식별자를 앞에 붙여 전역 디버그 로그를 남기는 매크로.
 * @d_level: DBF 레벨 상수.
 * @d_cdev: struct ccw_device 포인터. dasd_device 가 아직 없어도 쓸 수 있다.
 * @d_str: printf 형식 문자열.
 * @d_data: 가변 인자.
 * ccw_device_get_id() 로 (ssid, devno) 를 얻어 '0.x.xxxx' 형태의 버스 ID 를
 * 메시지 앞에 붙인다. 전역 영역에 쓰지만 어느 장치인지는 알 수 있게 하려는
 * 절충이며, dasd_int_handler() 처럼 dasd_device 를 아직 못 얻은 자리에서
 * 특히 유용하다.
 * 블록 안에서 struct ccw_dev_id __dev_id 를 지역 변수로 선언하므로,
 * 이 매크로를 쓰는 자리에 __dev_id 라는 이름이 이미 있으면 가려진다. */
#define DBF_EVENT_DEVID(d_level, d_cdev, d_str, d_data...)	\
do { \
	struct ccw_dev_id __dev_id;			\
	ccw_device_get_id(d_cdev, &__dev_id);		\
	debug_sprintf_event(dasd_debug_area,		\
			    d_level,					\
			    "0.%x.%04x " d_str "\n",			\
			    __dev_id.ssid, __dev_id.devno, d_data);	\
} while (0)

/* definition of dbf debug levels */
/* [한국어] dbf 로그 레벨 0. syslog 의 KERN_EMERG 와 같은 뜻이며 가장 심각하다.
 * 값이 작을수록 심각하고, dbf 는 설정된 레벨 이하의 기록만 남긴다.
 * [상류 코드 관찰] 이 여섯 레벨 중 실제로 쓰이는 것은 아래쪽 몇 개뿐이다. */
#define	DBF_EMERG	0	/* system is unusable			*/
/* [한국어] dbf 로그 레벨 1. 즉시 조치가 필요한 사건.
 * syslog 의 KERN_ALERT 에 대응하는 자리를 차지한다. */
#define	DBF_ALERT	1	/* action must be taken immediately	*/
/* [한국어] dbf 로그 레벨 2. 치명적 조건.
 * syslog 의 KERN_CRIT 에 대응한다. */
#define	DBF_CRIT	2	/* critical conditions			*/
/* [한국어] dbf 로그 레벨 3. 오류 조건. 이 드라이버가 실패 경로에서 가장 흔히 쓰는
 * 레벨이다. */
#define	DBF_ERR		3	/* error conditions			*/
/* [한국어] dbf 로그 레벨 4. 경고. dasd_int_handler() 가 타임아웃이나 알 수 없는 오류를
 * 만났을 때 이 레벨로 남긴다. */
#define	DBF_WARNING	4	/* warning conditions			*/
/* [한국어] dbf 로그 레벨 5. 정상이지만 눈여겨볼 만한 사건. */
#define	DBF_NOTICE	5	/* normal but significant condition	*/
/* [한국어] dbf 로그 레벨 6. 정보성 기록. */
#define	DBF_INFO	6	/* informational			*/
/* [한국어] dbf 로그 레벨 6. 디버그용 기록.
 * [상류 코드 관찰] 바로 위 DBF_INFO 와 **값이 같다**(둘 다 6). 이름만 다르고
 * dbf 필터에는 구별되지 않으므로, DBF_DEBUG 로 남긴 기록은 DBF_INFO 를 켜면
 * 함께 나온다. 원본(1f0e418bb6) 123-124줄에서 확인했으며 코드는 고치지 않았다. */
#define	DBF_DEBUG	6	/* debug-level messages			*/

/* Macro to calculate number of blocks per page */
/* [한국어] 한 페이지에 들어가는 논리 블록 개수를 구하는 매크로.
 * @blksize: 볼륨의 논리 블록 크기(바이트). 512~4096 사이의 2의 거듭제곱이다.
 * 읽는 자: dasd_fba.c:338 한 곳뿐이다. FBA discipline 이 discard(공간 해제)
 * 요청을 만들 때, 0으로 채운 페이지 하나로 몇 블록을 덮을 수 있는지 계산한다.
 * 괄호를 씌우지 않은 인자(blksize)를 나눗셈의 오른쪽에 그대로 두었으므로,
 * 'a+b' 같은 식을 넘기면 연산자 우선순위 때문에 뜻이 달라진다.
 * 지금 유일한 호출자는 단순 변수를 넘기므로 문제되지 않는다. */
#define BLOCKS_PER_PAGE(blksize) (PAGE_SIZE / blksize)

/* [한국어] DASD I/O 한 건을 나타내는 구조체. 코드 전반에서 'cqr' 이라는 이름으로
 * 쓰인다. 블록 계층의 struct request 하나가 discipline 의 build_cp() 를 거쳐
 * 이것 하나로 바뀌고, ERP 가 개입하면 이것을 감싸는 cqr 이 하나 더 생겨
 * refers 로 사슬을 이룬다.
 * 할당은 dasd.c 의 dasd_smalloc_request()(정적 풀)나 dasd_fmalloc_request()
 * (ESE 포맷 전용 풀)가 맡으며, 둘 다 인터럽트 문맥에서도 실패하지 않도록
 * 장치마다 미리 잡아 둔 페이지를 쪼개 쓴다(struct dasd_mchunk 참조).
 * 이 구조체는 세 컨텍스트에서 동시에 만져진다 — 프로세스(build_cp),
 * softirq(두 tasklet), 인터럽트(dasd_int_handler). 그래서 아래 각 필드의
 * 동기화 규칙이 서로 다르다. */
struct dasd_ccw_req {
	/* [한국어] 이 cqr 을 만든 discipline 을 나타내는 EBCDIC 4글자 눈표.
	 * 설정자: dasd_smalloc_request()/dasd_fmalloc_request() 가 인자로 받은 값을
	 * 그대로 넣는다. 호출자가 DASD_ECKD_MAGIC / DASD_FBA_MAGIC / DASD_DIAG_MAGIC
	 * 중 하나를 넘긴다. ERP 사슬의 cqr 은 dasd_3990_erp.c:225 처럼 원본 cqr 의
	 * magic 을 물려받는다.
	 * 읽는 자: dasd.c 의 dasd_check_cqr()(1253줄), dasd_term_IO 계열(1507줄),
	 * dasd_int_handler()(1687줄)가 discipline->ebcname 과 strncmp 로 비교한다.
	 * 값 범위: 위 세 매크로 중 하나. 어긋나면 '잘못된 장치의 요청' 으로 보고
	 * -EINVAL 을 돌려주거나 조용히 버린다.
	 * 동기화: 만들 때 한 번 쓰고 그 뒤로는 읽기만 하므로 락이 필요 없다. */
	unsigned int magic;		/* Eye catcher */
	/* [한국어] 채널이 아니라 드라이버 내부에서 난 오류 코드.
	 * 설정자: dasd.c 의 dasd_start_IO() 가 ccw_device_start() 의 반환값을 담고,
	 * 타임아웃/취소 경로도 여기에 -ETIMEDOUT, -EIO 등을 남긴다.
	 * 읽는 자: sleep_on 계열과 dasd_cancel_req() 가 호출자에게 돌려줄 값으로
	 * 쓰고, discipline 의 오류 처리기도 참고한다.
	 * 값 범위: 0(내부 오류 없음) 또는 음수 errno.
	 * 동기화: cdev 락 아래에서 설정되고, 요청이 최종 상태가 된 뒤에 읽히므로
	 * 별도 보호가 없다. 하드웨어가 준 상태는 여기가 아니라 irb 에 담긴다. */
	int intrc;			/* internal error, e.g. from start_IO */
	/* [한국어] device->ccw_queue 에 매달리는 연결 고리.
	 * 설정자/읽는 자: dasd.c 의 dasd_add_request_head()/dasd_add_request_tail() 이
	 * 넣고, device tasklet 이 최종 상태가 된 요청을 꺼낸다.
	 * 값 범위: 큐에 없을 때는 list_del_init 으로 자기 자신을 가리킨다.
	 * 동기화: **반드시 get_ccwdev_lock(device->cdev) 을 쥐고** 조작해야 한다.
	 * 인터럽트 처리기와 device tasklet 이 같은 큐를 만지기 때문이다. */
	struct list_head devlist;	/* for dasd_device request queue */
	/* [한국어] block->ccw_queue 에 매달리는 연결 고리. devlist 와 별개의 고리를 둔 이유는,
	 * 한 cqr 이 블록 계층 큐와 장치 큐에 **동시에** 들어 있을 수 있기 때문이다.
	 * 설정자/읽는 자: dasd.c 의 do_dasd_request() 가 넣고, block tasklet 의
	 * __dasd_process_block_ccw_queue() 가 최종 상태의 요청을 꺼낸다.
	 * 값 범위: 위와 같다.
	 * 동기화: block->queue_lock 아래에서 조작한다. devlist 와는 다른 락이므로
	 * 두 고리의 보호자를 헷갈리면 곧바로 경쟁 조건이 된다. */
	struct list_head blocklist;	/* for dasd_block request queue */
	/* [한국어] 이 요청이 나온 블록 장치. 블록 계층에서 온 요청만 값이 있다.
	 * 설정자: build_cp() 가 만든 cqr 에 do_dasd_request() 가 채운다.
	 * 읽는 자: 완료 처리에서 blocklist 를 어느 큐에서 뺄지 정할 때, 그리고
	 * dasd_ese_needs_format() 이 ESE 볼륨인지 볼 때 쓴다.
	 * 값 범위: NULL 이면 내부 요청(장치 특성 읽기, ERP, EER 의 SNSS 등)이다.
	 * 동기화: 만들 때 한 번 쓰고 읽기만 한다. */
	struct dasd_block *block;	/* the originating block device */
	/* [한국어] 이 cqr 의 메모리를 떼어 준 장치. 해제할 때 **반드시 같은 장치의 풀** 로
	 * 돌려줘야 하므로 따로 기억해 둔다.
	 * 설정자: 각 discipline 의 build_cp()/요청 생성 함수가 채운다.
	 * 읽는 자: dasd_sfree_request()/dasd_ffree_request() 를 부르는 자리들.
	 * 값 범위: 유효한 dasd_device 포인터.
	 * 동기화: 만들 때 한 번 쓴다. startdev 와 다를 수 있다는 점이 핵심이다 —
	 * PAV 환경에서는 기본 장치의 풀에서 떼어 별칭 장치로 내보내기도 한다. */
	struct dasd_device *memdev;	/* the device used to allocate this */
	/* [한국어] 이 요청을 실제로 내보낼 장치. 이 파일에서 가장 자주 읽히는 필드다
	 * (이 디렉터리 안에서 117곳).
	 * 설정자: build_cp() 가 정하고, PAV 별칭을 고르는 dasd_alias.c 의
	 * dasd_alias_get_start_dev() 결과에 따라 바뀔 수 있다.
	 * 읽는 자: dasd_start_IO(), dasd_term_IO(), dasd_int_handler(), ERP 전부.
	 * 값 범위: 유효한 dasd_device 포인터.
	 * 동기화: 요청이 큐에 들어가기 전에 정해지고 그 뒤로는 읽기만 한다. */
	struct dasd_device *startdev;	/* device the request is started on */
	/* [한국어] 블록 장치가 없는 요청에서 '기준 장치' 를 가리키는 자리.
	 * 설정자: dasd_eckd.c:2554/2646/2907 등 포맷·검사용 cqr 을 만드는 곳.
	 * 읽는 자: dasd.c:2448 이 startdev 와 다르면 별칭 장치로 나간 요청임을 알고,
	 * dasd_eckd.c:1733 이 prefix 데이터의 base 로 쓴다.
	 * 값 범위: NULL 이거나 유효한 dasd_device 포인터. block 이 있으면 보통
	 * block->base 를 쓰므로 이 필드는 비어 있다.
	 * 동기화: 만들 때 한 번 쓴다. */
	struct dasd_device *basedev;	/* base device if no block->base */
	/* [한국어] 채널이 실행할 명령 사슬의 시작 주소. **이 구조체에서 하드웨어가 실제로
	 * 읽는 유일한 포인터** 다.
	 * 설정자: dasd_smalloc_request()/dasd_fmalloc_request() 가 정적 풀에서 떼어 낸
	 * 메모리 안쪽을 가리키게 하고, discipline 이 그 자리에 CCW 를 채워 넣는다.
	 * 읽는 자: discipline->start_IO() 가 ccw_device_start() 계열에 넘긴다.
	 * 값 범위: cpmode 가 0이면 struct ccw1 배열의 첫 원소, 1이면 struct tcw 를
	 * 가리킨다. void 포인터인 이유가 바로 이 이중성이다.
	 * 동기화: 만들 때 정해지고, 하드웨어가 읽는 동안에는 드라이버가 만지지
	 * 않는다. 채널이 DMA 로 접근하므로 GFP_DMA 페이지에서 떼어 낸다. */
	void *cpaddr;			/* address of ccw or tcw */
	/* [한국어] 남은 재시도 횟수. 0이 되면 더 이상 다시 시도하지 않는다.
	 * 설정자: 요청을 만드는 쪽이 device->default_retries 등으로 초기화하고,
	 * ERP 가 한 번 실패할 때마다 줄인다.
	 * 읽는 자: ERP 전반과 dasd.c:1925 의 자동 quiesce 판정
	 * ((default_retries - retries) 가 aq_timeouts 이상이면 장치를 멈춘다).
	 * 값 범위: 0 이상. 상한은 DASD_RETRIES_MAX(32768)이며 short 라 그 이상은
	 * 담을 수 없다.
	 * 동기화: cdev 락이나 ERP 흐름 안에서만 바뀐다. */
	short retries;			/* A retry counter */
	/* [한국어] 명령 모드 선택. 0이면 전통적인 CCW 사슬, 1이면 전송 모드(TCW/ITCW)다.
	 * 전송 모드는 HPF(High Performance FICON)에서 쓰며, 여러 CCW 를 한 덩어리로
	 * 묶어 왕복 횟수를 줄인다.
	 * 설정자: dasd_eckd.c:2551/4622 처럼 TCW 를 쓰는 build 함수가 1로 세운다.
	 * 읽는 자: dasd.c:806/824/842 가 어떤 시작 함수를 부를지 고르고, 1371줄이
	 * 전송 모드 전용 준비를 하며, 1763줄이 HPF 오류인지 판정한다.
	 * ERP 쪽에서는 dasd_3990_erp.c:2347/2376 이 모드에 따라 다른 복구를 고른다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 만들 때 정해지고 그 뒤로는 읽기만 한다. */
	unsigned char cpmode;		/* 0 = cmd mode, 1 = itcw */
	/* [한국어] 이 요청의 상태. **이 드라이버 전체의 상태 기계가 이 한 바이트로 돌아간다.**
	 * 아래 DASD_CQR_ 계열 상수 중 하나를 담는다.
	 * 설정자: 만들 때 DASD_CQR_FILLED, 큐에 넣을 때 DASD_CQR_QUEUED,
	 * dasd_start_IO() 가 DASD_CQR_IN_IO, dasd_int_handler() 가 완료/오류에 따라
	 * DASD_CQR_DONE 이나 DASD_CQR_ERROR 로 바꾼다.
	 * 읽는 자: 두 tasklet 과 ERP 전부.
	 * 값 범위: 0x00~0x05(장치 큐 밖의 상태)와 0x80~0x85(장치 큐 안의 상태).
	 * 최상위 비트가 '아직 장치 큐에 매달려 있는가' 를 가르는 표지 역할을 한다.
	 * 동기화: cdev 락 또는 block->queue_lock 아래에서만 바꿔야 한다.
	 * 인터럽트 처리기가 이 값을 바꾼 뒤 tasklet 을 깨우는 것이 완료 통보의 전부다. */
	char status;			/* status of this request */
	/* [한국어] 이 요청을 내보낼 때 허용할 채널 경로 비트마스크(LPM).
	 * 설정자: 기본은 0이며, dasd.c:1360-1362 가 시작 직전에 장치의 동작 가능
	 * 경로(opm)와 AND 하고, 결과가 0이면 opm 전체로 되돌린다.
	 * ERP 는 실패한 경로를 빼기 위해 dasd_3990_erp.c:155 에서 이 값을 깎는다.
	 * 읽는 자: discipline->start_IO() 가 채널 서브시스템에 넘긴다.
	 * 값 범위: 비트 8개. 경로 0이 0x80, 경로 7이 0x01 이다(IBM 의 왼쪽 우선 규약).
	 * 동기화: cdev 락 아래에서 바뀐다.
	 * [상류 코드 관찰] 타입이 부호 있는 char 라, 0x80 비트만 선 값은 음수가 된다.
	 * 이 파일의 경로 마스크 함수들은 __u8 을 쓰는데 이 필드만 char 다.
	 * 원본(1f0e418bb6) 142줄에서 확인했으며 코드는 고치지 않았다. */
	char lpm;			/* logical path mask */
	/* [한국어] 요청별 표지 비트 모음. 아래 DASD_CQR_FLAGS_ 계열과 DASD_CQR_SUPPRESS_ 계열
	 * 상수가 이 필드의 **비트 번호** 다(마스크가 아니다 — test_bit 에 쓴다).
	 * 설정자: dasd_smalloc_request() 가 DASD_CQR_FLAGS_USE_ERP 를 기본으로 세우고,
	 * 요청을 만드는 쪽이 필요에 따라 나머지를 더한다.
	 * 읽는 자: ERP 진입 여부, 실패 시 즉시 포기할지, 로그를 억제할지 판단하는 곳들.
	 * 값 범위: 0~7번 비트만 쓴다.
	 * 동기화: set_bit/test_bit 같은 원자적 비트 연산으로 다루므로 락이 없어도
	 * 비트 단위로는 안전하다. */
	unsigned long flags;        	/* flags of this request */
	/* [한국어] 이 요청이 나온 blk-mq 하드웨어 큐의 락 상자.
	 * 설정자: dasd.c:3089 의 do_dasd_request() 가 hctx->driver_data 에서 꺼내 넣는다.
	 * 그 driver_data 는 dasd.c:3183 의 init_hctx 콜백이 큐마다 하나씩 할당한다.
	 * 읽는 자: dasd.c:2916/2986/3124/3175 와 block tasklet 이 요청을 블록 계층으로
	 * 돌려줄 때 dq->lock 을 잡는다.
	 * 값 범위: 유효한 dasd_queue 포인터.
	 * 동기화: 이 필드 자체는 만들 때 한 번 쓰지만, 가리키는 락은 blk-mq 요청의
	 * 생명주기를 지키는 핵심이다. 완료 콜백이 이 락 아래에서 불린다. */
	struct dasd_queue *dq;
	/* [한국어] 요청을 채널에 내보낸 시각(jiffies).
	 * 설정자: dasd.c:1357 의 dasd_start_IO() 와 dasd_diag.c:190 이 jiffies 를 넣고,
	 * dasd.c:1287/1303 이 큐에서 도로 뺄 때 0으로 지운다.
	 * 읽는 자: dasd.c:1941 의 만료 검사가 (starttime + expires) 를 지금과 비교한다.
	 * 값 범위: jiffies 값 또는 0(아직 시작 안 함).
	 * 동기화: cdev 락 아래에서 다룬다. time_after_eq() 로 비교하므로 jiffies
	 * 되돌이(wrap)에도 안전하다. */
	unsigned long starttime;	/* jiffies time of request start */
	/* [한국어] 시작 시각으로부터 몇 jiffies 뒤에 만료로 볼지.
	 * 설정자: 요청을 만드는 쪽이 device->default_expires 를 초 단위로 받아
	 * HZ 를 곱해 넣는다.
	 * 읽는 자: dasd.c:1941 의 만료 검사와 타이머 설정.
	 * 값 범위: 상한은 DASD_EXPIRES_MAX(40000000초)이며, 0이면 만료를 보지 않는다.
	 * 동기화: 만들 때 정해지고 ERP 가 늘릴 수 있다. cdev 락 아래. */
	unsigned long expires;		/* expiration period in jiffies */
	/* [한국어] CCW 들이 가리키는 데이터 영역의 시작 주소.
	 * 설정자: dasd_smalloc_request()/dasd_fmalloc_request() 가 cpaddr 바로 뒤에
	 * datasize 만큼을 잡아 여기를 가리키게 한다. 내용은 discipline 이 채운다.
	 * 읽는 자: discipline 이 자기 형식(prefix 데이터, define extent, locate record
	 * 등)으로 캐스팅해 쓴다. 하드웨어도 CCW 를 통해 이 영역을 읽고 쓴다.
	 * 값 범위: datasize 가 0이면 NULL 일 수 있다.
	 * 동기화: cpaddr 과 같다. 채널이 DMA 로 접근하는 동안 만지면 안 된다. */
	void *data;			/* pointer to data area */
	/* [한국어] 오류가 났을 때 채널이 돌려준 IRB(Interrupt Response Block)의 사본.
	 * 구조체를 통째로 품고 있어 cqr 하나가 그만큼 커진다.
	 * 설정자: dasd_int_handler() 가 정상 완료가 아닌 인터럽트에서 memcpy 로 복사해
	 * 둔다. 인터럽트가 준 원본은 곧 사라지므로 사본이 필요하다.
	 * 읽는 자: ERP 전부. dasd_get_sense() 로 센스 데이터를 꺼내 해석하고,
	 * dasd_3990_erp.c:155 처럼 irb.esw.esw0.sublog.lpum 으로 실패한 경로도 알아낸다.
	 * 값 범위: struct irb 는 arch/s390 소관이라 이 트리에서 정의는 확인 못 함.
	 * 동기화: 인터럽트 컨텍스트에서 쓰고 softirq/프로세스 컨텍스트에서 읽는다.
	 * 상태를 최종값으로 바꾸기 **전에** 복사하는 순서가 그 안전을 보장한다. */
	struct irb irb;			/* device status in case of an error */
	/* [한국어] ERP 사슬에서 '내가 복구하려는 원래 요청' 을 가리킨다.
	 * 설정자: dasd_erp.c 와 dasd_3990_erp.c 가 복구용 cqr 을 만들 때 채운다.
	 * 읽는 자: dasd_get_callback_data()(이 파일 아래)가 사슬을 끝까지 거슬러 올라가
	 * 원래 blk-mq 요청을 찾고, 완료 처리도 이 고리를 따라 되감는다.
	 * 값 범위: NULL 이면 사슬의 뿌리(원래 요청)다. ERP 가 여러 번 겹치면 사슬이
	 * 여러 단이 될 수 있다.
	 * 동기화: ERP 흐름 안에서만 다루며, 그 흐름은 device tasklet 안에서 직렬화된다. */
	struct dasd_ccw_req *refers;	/* ERP-chain queueing. */
	/* [한국어] 이 cqr 을 만든 ERP 함수의 주소. **함수 포인터를 값이 아니라 표식으로 쓴다.**
	 * 설정자: dasd_3990_erp.c 가 단계마다 자기 함수 주소를 넣는다
	 * (예: 260줄 dasd_3990_erp_action_1_sec, 221줄 dasd_3990_erp_DCTL).
	 * 읽는 자: 같은 파일 107줄처럼 '직전 단계가 무엇이었는지' 를 비교해 다음
	 * 복구 단계를 고른다. dasd_fba.c:221 은 기본 ERP 였는지 확인한다.
	 * 값 범위: ERP 함수 주소 또는 NULL.
	 * 동기화: ERP 흐름 안에서만 다룬다.
	 * 타입이 void 포인터라 컴파일러가 형을 검사해 주지 못한다 — 비교는 전적으로
	 * 프로그래머의 규율에 기댄다. */
	void *function; 		/* originating ERP action */
	/* [한국어] 이 cqr 이 떼어져 나온 메모리 덩어리의 시작 주소.
	 * 설정자: dasd_smalloc_request()(dasd.c:1161)가 dasd_alloc_chunk() 의 반환값을
	 * 그대로 넣는다.
	 * 읽는 자: dasd_sfree_request()(dasd.c:1226)가 이 주소로 풀에 되돌린다.
	 * 값 범위: 유효한 포인터. cqr 자신이 그 덩어리 안에 들어 있을 수도 있고
	 * (cqr 을 따로 받지 않은 경우), 덩어리 밖에 있을 수도 있다.
	 * 동기화: 만들 때 한 번 쓴다. 해제는 device->mem_lock 아래에서 한다.
	 * [상류 코드 관찰] dasd_fmalloc_request() 로 만든 cqr 은 이 필드를 채우지
	 * 않는다. 그쪽은 cqr 주소 자체가 덩어리 시작이라 dasd_ffree_request() 가
	 * cqr 을 그대로 넘긴다. 원본(1f0e418bb6)의 dasd.c:1176-1215 에서 확인했다. */
	void *mem_chunk;

	/* [한국어] 요청을 만든 시각. s390 의 TOD(Time-Of-Day) 시계 값이다.
	 * 설정자: 각 discipline 의 build_cp() 마지막에 get_tod_clock() 값을 넣는다.
	 * 읽는 자: dasd.c 의 프로파일 수집이 buildclk~startclk 구간을 '큐에서 기다린
	 * 시간' 으로 집계한다(dasd_io_time1).
	 * 값 범위: TOD 시계 눈금. 마이크로초의 약 1/4096 단위라 jiffies 보다 훨씬 곱다.
	 * 동기화: 만들 때 한 번 쓴다.
	 * 아래 네 개의 시계 값(build/start/stop/end)이 요청 한 건의 생애를 네 구간으로
	 * 나누며, /proc/dasd/statistics 의 히스토그램이 그 구간들을 센다. */
	unsigned long buildclk;		/* TOD-clock of request generation */
	/* [한국어] 채널에 요청을 내보낸 시각(TOD).
	 * 설정자: dasd_start_IO() 가 ccw_device_start() 직전에 넣는다.
	 * 읽는 자: 프로파일 수집이 startclk~stopclk 를 '채널에서 보낸 시간'
	 * (dasd_io_time2)으로 집계한다.
	 * 값 범위: TOD 눈금.
	 * 동기화: cdev 락 아래. */
	unsigned long startclk;		/* TOD-clock of request start */
	/* [한국어] 완료 인터럽트가 도착한 시각(TOD).
	 * 설정자: dasd_int_handler() 가 인터럽트 진입 직후 얻은 값을 넣는다.
	 * 읽는 자: 프로파일 수집이 stopclk~endclk 를 '완료 처리에 든 시간'
	 * (dasd_io_time3)으로 집계한다.
	 * 값 범위: TOD 눈금.
	 * 동기화: 인터럽트 컨텍스트에서 쓴다. */
	unsigned long stopclk;		/* TOD-clock of request interrupt */
	/* [한국어] 요청을 블록 계층에 완전히 돌려준 시각(TOD).
	 * 설정자: 완료 콜백을 부르기 직전 dasd.c 의 __dasd_cleanup_cqr() 근처에서 넣는다.
	 * 읽는 자: 프로파일 수집. buildclk~endclk 전체가 '요청 총 지연'(dasd_io_times)이다.
	 * 값 범위: TOD 눈금.
	 * 동기화: block tasklet 컨텍스트에서 쓴다. */
	unsigned long endclk;		/* TOD-clock of request termination */

	/* [한국어] 요청이 최종 상태가 되었을 때 부를 함수.
	 * 설정자: 요청을 만드는 쪽. 블록 I/O 는 dasd.c 내부의 정리 함수를,
	 * 동기 요청은 dasd_wakeup_cb() 를 건다.
	 * 읽는 자: device tasklet 의 __dasd_device_process_final_queue() 가 부른다.
	 * 값 범위: NULL 이면 아무 것도 하지 않는다.
	 * 동기화: **락을 놓은 뒤** 불린다. 콜백 안에서 다시 큐를 만질 수 있어야 하기
	 * 때문이다. 실행 컨텍스트는 softirq(tasklet)이므로 잠들면 안 된다. */
	void (*callback)(struct dasd_ccw_req *, void *data);
	/* [한국어] 콜백에 함께 넘길 값. 블록 I/O 에서는 원래의 struct request 포인터다.
	 * 설정자: 요청을 만드는 쪽. 동기 요청은 DASD_SLEEPON_START_TAG 같은
	 * 표식 상수를 넣기도 한다.
	 * 읽는 자: 콜백 자신과 dasd_get_callback_data()(이 파일 아래). 후자는 ERP
	 * 사슬을 뿌리까지 거슬러 올라가 원래 요청을 꺼낸다.
	 * 값 범위: 임의의 포인터 또는 위 표식 상수.
	 * 동기화: 만들 때 정해진다. */
	void *callback_data;
	/* [한국어] 부분 완료로 처리된 바이트 수. ESE(씬 프로비저닝) 볼륨에서 아직 할당되지
	 * 않은 트랙을 읽을 때, 실제 I/O 없이 0으로 채워 돌려주는 몫이다.
	 * 설정자: dasd_eckd.c:3312 가 (블록 수 x 블록 크기)를 넣는다.
	 * 읽는 자: dasd.c:2693 이 값을 읽어 2735줄에서 blk_update_request() 로
	 * 그만큼을 먼저 완료 처리한다.
	 * 값 범위: 0이면 부분 완료가 아니다.
	 * 동기화: 요청 완료 흐름 안에서만 다룬다. */
	unsigned int proc_bytes;	/* bytes for partial completion */
	/* [한국어] 이 cqr 을 만들 때 관측한 블록 장치의 포맷 카운터 값.
	 * 설정자: dasd.c:1369 가 시작 직전에 atomic_read(&cqr->block->trkcount) 를 찍어 둔다.
	 * 읽는 자: dasd_eckd.c:3121 이 지금 값과 다르면 '내가 대기하는 사이 누군가
	 * 트랙을 포맷했다' 고 판단한다.
	 * 값 범위: 단조 증가하는 카운터의 스냅숏.
	 * 동기화: 원자 변수에서 읽어 온 사본이라 그 자체로는 락이 필요 없다.
	 * 이 방식(값 비교로 변경 감지)은 ABA 문제를 회피하기 위한 세대 번호 기법이다. */
	unsigned int trkcount;		/* count formatted tracks */
};

/*
 * dasd_ccw_req -> status can be:
 */
#define DASD_CQR_FILLED 	0x00	/* request is ready to be processed */
#define DASD_CQR_DONE		0x01	/* request is completed successfully */
#define DASD_CQR_NEED_ERP	0x02	/* request needs recovery action */
#define DASD_CQR_IN_ERP 	0x03	/* request is in recovery */
#define DASD_CQR_FAILED 	0x04	/* request is finally failed */
#define DASD_CQR_TERMINATED	0x05	/* request was stopped by driver */

#define DASD_CQR_QUEUED 	0x80	/* request is queued to be processed */
#define DASD_CQR_IN_IO		0x81	/* request is currently in IO */
#define DASD_CQR_ERROR		0x82	/* request is completed with error */
#define DASD_CQR_CLEAR_PENDING	0x83	/* request is clear pending */
#define DASD_CQR_CLEARED	0x84	/* request was cleared */
#define DASD_CQR_SUCCESS	0x85	/* request was successful */

/* default expiration time*/
#define DASD_EXPIRES	  300
#define DASD_EXPIRES_MAX  40000000
#define DASD_RETRIES	  256
#define DASD_RETRIES_MAX  32768

/* per dasd_ccw_req flags */
#define DASD_CQR_FLAGS_USE_ERP   0	/* use ERP for this request */
#define DASD_CQR_FLAGS_FAILFAST  1	/* FAILFAST */
#define DASD_CQR_VERIFY_PATH	 2	/* path verification request */
#define DASD_CQR_ALLOW_SLOCK	 3	/* Try this request even when lock was
					 * stolen. Should not be combined with
					 * DASD_CQR_FLAGS_USE_ERP
					 */
/*
 * The following flags are used to suppress output of certain errors.
 */
#define DASD_CQR_SUPPRESS_NRF	4	/* Suppress 'No Record Found' error */
#define DASD_CQR_SUPPRESS_IT	5	/* Suppress 'Invalid Track' error*/
#define DASD_CQR_SUPPRESS_IL	6	/* Suppress 'Incorrect Length' error */
#define DASD_CQR_SUPPRESS_CR	7	/* Suppress 'Command Reject' error */

#define DASD_REQ_PER_DEV 4

/* Signature for error recovery functions. */
typedef struct dasd_ccw_req *(*dasd_erp_fn_t) (struct dasd_ccw_req *);

/*
 * A single CQR can only contain a maximum of 255 CCWs. It is limited by
 * the locate record and locate record extended count value which can only hold
 * 1 Byte max.
 */
#define DASD_CQR_MAX_CCW 255

/*
 * Unique identifier for dasd device.
 */
/* [한국어] 장치가 아직 구성되지 않음. 아래 dasd_uid.type 이 가질 수 있는 네 값 중
 * 첫째이며, UID 를 아직 읽지 못한 상태를 뜻한다. */
#define UA_NOT_CONFIGURED  0x00
/* [한국어] **기본 장치** — 별칭이 아닌 실제 볼륨이다. PAV 구성에서 I/O 가 최종적으로
 * 도달하는 곳이며, 아래 두 별칭 종류가 모두 이것을 가리킨다. */
#define UA_BASE_DEVICE	   0x01
/* [한국어] **기본 PAV 별칭** — 기본 장치 하나에 고정으로 묶인 별칭이다.
 * PAV(Parallel Access Volume)는 볼륨 하나에 장치 주소를 여럿 두어 동시에
 * 여러 I/O 를 띄우는 IBM 기법인데, 이 종류는 그 짝이 구성 시점에 정해진다. */
#define UA_BASE_PAV_ALIAS  0x02
/* [한국어] **하이퍼 PAV 별칭** — 위와 달리 어느 기본 장치에 붙을지가 **I/O 마다
 * 동적으로** 정해지는 별칭이다. 별칭 풀을 여러 볼륨이 나눠 쓸 수 있어
 * 같은 수의 주소로 더 높은 병렬도를 낸다. */
#define UA_HYPER_PAV_ALIAS 0x03

struct dasd_uid {
	/* [한국어] 이 장치가 기본 장치인지 별칭인지.
	 * 설정자: 디시플린의 UID 읽기 경로가 채운다.
	 * 읽는 자: 별칭 처리 코드(dasd_alias.c)가 이 값으로 장치를 분류한다.
	 * 값 범위: 위 UA_ 계열 넷.
	 * 동기화: 장치 인식 이후 사실상 불변이다. */
	__u8 type;
	char vendor[4];
	char serial[15];
	__u16 ssid;
	/* [한국어] 제어 장치 안에서 이 장치의 **실제** 단위 주소.
	 * 설정자: 디시플린의 UID 읽기 경로.
	 * 읽는 자: 별칭과 기본 장치를 짝지을 때.
	 * 값 범위: 8비트 단위 주소.
	 * 동기화: 인식 이후 불변. */
	__u8 real_unit_addr;
	/* [한국어] 이 장치가 별칭이라면 그것이 가리키는 **기본 장치의** 단위 주소.
	 * 설정자: 디시플린의 UID 읽기 경로.
	 * 읽는 자: 별칭을 기본 장치에 연결할 때. 기본 장치 자신에게는 의미가 없다.
	 * 값 범위: 8비트 단위 주소.
	 * 동기화: 인식 이후 불변.
	 * 위 real_unit_addr 과 이 필드의 관계가 곧 별칭 구조 그 자체다. */
	__u8 base_unit_addr;
	char vduit[33];
};

/* [한국어] UID 를 사람이 읽는 문자열로 풀었을 때의 최대 길이.
 * 각 항목의 자릿수에 구분자 한 칸씩을 더해 계산하며, 주석으로 그 셈이
 * 그대로 적혀 있다.
 * 줄 끝이 역슬래시로 이어지므로 이 설명을 매크로 위에 블록으로 둔다. */
#define DASD_UID_STRLEN ( /* vendor */ 3 + 1 + /* serial    */ 14 + 1 +	\
			  /* SSID   */ 4 + 1 + /* unit addr */ 2 + 1 +	\
			  /* vduit */ 32 + 1)

/*
 * PPRC Status data
 */
struct dasd_pprc_header {
	/* [한국어] 이 응답에 담긴 장치 항목 수.
	 * 설정자: **하드웨어가 채운다.** 이 구조체는 제어 장치가 돌려주는 응답을
	 * 그대로 얹어 읽는 틀이라, 리눅스 쪽에서 값을 쓰는 일이 없다.
	 * 읽는 자: 아래 dev_info 배열을 몇 개까지 읽을지 정할 때.
	 * 값 범위: 0 이상. 아래 배열 크기 5 를 넘지 않는다는 보장은 이 파일에 없다.
	 * 동기화: 응답 버퍼는 요청 하나에 딸린 것이라 공유되지 않는다. */
	__u8 entries;		/* 0     Number of device entries */
	/* [한국어] 쓰이지 않는 한 바이트. 아래 필드들의 자리를 맞추는 역할이다. */
	__u8 unused;		/* 1     unused */
	/* [한국어] 장치 항목 하나의 길이.
	 * 설정자: 하드웨어.
	 * 읽는 자: 항목을 순회할 때의 걸음 폭으로 쓸 수 있는 값이다.
	 * 값 범위: 아래 dasd_pprc_dev_info 의 크기와 맞아야 한다.
	 * 동기화: 위와 같다. */
	__u16 entry_length;	/* 2-3   Length of device entry */
	/* [한국어] 쓰이지 않는 네 바이트. 머리 부분을 8바이트로 맞춘다. */
	__u32 unused2;		/* 4-7   unused */
/* [한국어] `__packed` 가 필수다. 이 구조체는 하드웨어가 돌려준 바이트를 그대로
 * 겹쳐 읽는 **전송 형식(wire format)** 이라, 컴파일러가 정렬용 빈 공간을
 * 끼워 넣으면 필드가 통째로 어긋난다. 오른쪽의 상류 주석이 각 필드의
 * 바이트 오프셋을 적어 둔 것도 같은 이유다. */
} __packed;

struct dasd_pprc_dev_info {
	/* [한국어] 복사 상태.
	 * 설정자: 하드웨어.
	 * 읽는 자: PPRC 구성 정보를 해석하는 코드.
	 * 값 범위: 이 트리에서 확인 못 함 — 값의 목록이 이 헤더에 없다.
	 * 동기화: 응답 버퍼는 공유되지 않는다. */
	__u8 state;		/* 0       Copy State */
	/* [한국어] 플래그 모음.
	 * 설정자: 하드웨어.
	 * 읽는 자: 위와 같다.
	 * 값 범위: 비트별 의미가 이 헤더에 정의돼 있지 않아 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 flags;		/* 1       Flags */
	/* [한국어] 예약 두 바이트. 하드웨어 형식을 맞추기 위한 자리다. */
	__u8 reserved1[2];	/* 2-3     reserved */
	/* [한국어] **주** 장치가 속한 LSS(Logical SubSystem) 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: PPRC 짝을 식별할 때.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 prim_lss;		/* 4       Primary device LSS */
	/* [한국어] 주 장치의 주소. PPRC(Peer-to-Peer Remote Copy)는 두 장치를 주-부로
	 * 묶어 한쪽 쓰기를 다른 쪽에 그대로 복제하는 IBM 원격 복제 기능이며,
	 * 이 필드와 아래 secondary 가 그 짝을 이룬다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 위와 같다.
	 * 값 범위: 8비트 장치 주소.
	 * 동기화: 위와 같다. */
	__u8 primary;		/* 5       Primary device address */
	/* [한국어] **부** 장치가 속한 LSS 번호.
	 * 설정자·읽는 자·동기화: 위 prim_lss 와 같다.
	 * 값 범위: 8비트. */
	__u8 sec_lss;		/* 6       Secondary device LSS */
	/* [한국어] 부 장치의 주소 — 복제본이 놓이는 쪽이다.
	 * 설정자·읽는 자·동기화: 위 primary 와 같다.
	 * 값 범위: 8비트 장치 주소. */
	__u8 secondary;		/* 7       Secondary device address */
	/* [한국어] 이 PPRC 짝의 식별자.
	 * 설정자: 하드웨어.
	 * 읽는 자: 같은 짝에 속한 항목들을 묶을 때.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 pprc_id;		/* 8-9     Peer-to-Peer Remote Copy ID */
	/* [한국어] 예약 열두 바이트. */
	__u8 reserved2[12];	/* 10-21   reserved */
	/* [한국어] 주 제어 장치의 SSID(SubSystem ID).
	 * 설정자: 하드웨어.
	 * 읽는 자: 제어 장치 단위로 짝을 식별할 때.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 prim_cu_ssid;	/* 22-23   Primary Control Unit SSID */
	/* [한국어] 예약 열두 바이트. */
	__u8 reserved3[12];	/* 24-35   reserved */
	/* [한국어] 부 제어 장치의 SSID.
	 * 설정자·읽는 자·동기화: 위 prim_cu_ssid 와 같다.
	 * 값 범위: 16비트. */
	__u16 sec_cu_ssid;	/* 36-37   Secondary Control Unit SSID */
	/* [한국어] 예약 아흔 바이트. 이 예약 자리들이 항목 하나를 정확히 **128바이트**로
	 * 맞춘다 — 오른쪽 상류 주석의 오프셋이 0 부터 127 까지 이어지는 것이 그 근거다. */
	__u8 reserved4[90];	/* 38-127  reserved */
/* [한국어] 역시 전송 형식이라 `__packed` 가 필수다. */
} __packed;

struct dasd_pprc_data_sc4 {
	/* [한국어] 머리 부분 — 항목 수와 항목 길이를 담는다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 아래 배열을 몇 개까지 읽을지 정할 때.
	 * 값 범위: 위 dasd_pprc_header.
	 * 동기화: 응답 버퍼는 공유되지 않는다. */
	struct dasd_pprc_header header;
	/* [한국어] 장치 항목 배열.
	 * 설정자: 하드웨어.
	 * 읽는 자: PPRC 구성을 해석하는 코드.
	 * 값 범위: **다섯 개 고정.** 위 머리의 entries 가 실제로 채워진 수를 알려
	 * 주므로, 그 값까지만 읽어야 한다.
	 * 동기화: 응답 버퍼는 공유되지 않는다. */
	struct dasd_pprc_dev_info dev_info[5];
/* [한국어] 이 묶음 역시 전송 형식이라 `__packed` 다. */
} __packed;

/* [한국어] 장치를 가리키는 버스 ID 문자열의 길이. 아래 복사 항목이 장치를
 * 포인터가 아니라 **문자열로도** 붙들어 두기 때문에 필요하다. */
#define DASD_BUS_ID_SIZE 20
/* [한국어] 복사 관계 하나에 들어갈 수 있는 항목 수. 아래 배열의 크기다. */
#define DASD_CP_ENTRIES 5

struct dasd_copy_entry {
	/* [한국어] 이 항목이 가리키는 장치의 버스 ID 문자열.
	 * 설정자: 복사 관계를 구성할 때 devmap 코드가 채운다.
	 * 읽는 자: 아직 붙지 않은 장치를 이름으로 대조할 때.
	 * 값 범위: 널 종료 문자열, 최대 위 상수 길이.
	 * 동기화: 복사 관계 구성 경로에서만 다뤄진다.
	 * **포인터와 문자열을 함께 두는 이유** 는 관계를 설정하는 시점에 상대
	 * 장치가 아직 붙어 있지 않을 수 있어서다 — 그때는 이름만 아는 상태로
	 * 두었다가 나중에 아래 device 를 채운다. */
	char busid[DASD_BUS_ID_SIZE];
	/* [한국어] 이 항목에 대응하는 실제 장치.
	 * 설정자: 그 장치가 붙을 때 채워진다.
	 * 읽는 자: 복사 관계를 따라 상대 장치에 닿을 때.
	 * 값 범위: 유효한 포인터, 또는 아직 붙지 않았으면 NULL.
	 * 동기화: 위와 같다. */
	struct dasd_device *device;
	/* [한국어] 이 항목이 주 장치인지.
	 * 설정자: 복사 관계 구성 시.
	 * 읽는 자: 어느 쪽이 원본인지 가릴 때.
	 * 값 범위: 참/거짓.
	 * 동기화: 위와 같다. */
	bool primary;
	/* [한국어] 이 항목이 구성 완료 상태인지.
	 * 설정자: 복사 관계 구성 시.
	 * 읽는 자: 아직 준비되지 않은 항목을 건너뛸 때.
	 * 값 범위: 참/거짓.
	 * 동기화: 위와 같다. */
	bool configured;
};

struct dasd_copy_relation {
	/* [한국어] 이 관계에 속한 항목들.
	 * 설정자·읽는 자: 복사 관계를 구성하고 따라가는 코드.
	 * 값 범위: 다섯 개 고정 배열. 쓰이지 않는 자리는 0 으로 남는다.
	 * 동기화: 복사 관계 구성 경로에서만 다뤄진다. */
	struct dasd_copy_entry entry[DASD_CP_ENTRIES];
	/* [한국어] 지금 **활성** 인 항목 — 즉 실제로 I/O 를 받는 쪽이다.
	 * 설정자: 복사 관계를 전환할 때 바뀐다.
	 * 읽는 자: I/O 를 어느 장치로 보낼지 정할 때.
	 * 값 범위: 위 배열의 한 원소를 가리키거나 NULL.
	 * 동기화: 위와 같다.
	 * **이 포인터 하나를 옮기는 것이 곧 주-부 전환** 이라는 점이 이 구조의
	 * 요점이다. */
	struct dasd_copy_entry *active;
};

/* [한국어] 복사 관계를 설정하는 진입점. 구현은 dasd_devmap.c 에 있고,
 * PPRC 활성 여부를 함께 받는다. */
int dasd_devmap_set_device_copy_relation(struct ccw_device *,
					 bool pprc_enabled);

/*
 * the struct dasd_discipline is
 * sth like a table of virtual functions, if you think of dasd_eckd
 * inheriting dasd...
 * no, currently we are not planning to reimplement the driver in C++
 */
struct dasd_discipline {
	/* [한국어] 이 디시플린을 구현한 모듈.
	 * 설정자: 각 디시플린이 자기 정적 표에서 THIS_MODULE 로 채운다.
	 * 읽는 자: 장치가 이 디시플린을 쓰는 동안 모듈이 내려가지 않게 참조를 잡는다.
	 * 값 범위: 유효한 모듈 포인터.
	 * 동기화: 표가 상수라 바뀌지 않는다. */
	struct module *owner;
	/* [한국어] EBCDIC 로 표기한 디시플린 이름.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 메인프레임 쪽 규약에 맞춰 이름을 찍어야 하는 자리.
	 * 값 범위: 8바이트 고정. 널 종료를 보장하지 않는다.
	 * 동기화: 상수.
	 * **ASCII 판(name)과 따로 두는 이유** 는 s390 이 EBCDIC 을 쓰는 하드웨어
	 * 인터페이스와 ASCII 를 쓰는 리눅스 양쪽에 이름을 보여야 하기 때문이다. */
	char ebcname[8];	/* a name used for tagging and printks */
	/* [한국어] ASCII 로 표기한 디시플린 이름 — "ECKD", "FBA", "DIAG" 같은 값이다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 커널 로그와 sysfs 출력.
	 * 값 범위: 8바이트 고정.
	 * 동기화: 상수. */
	char name[8];		/* a name used for tagging and printks */
	/* [한국어] 이 디시플린이 discard(블록 해제) 요청을 지원하는지.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 블록 큐 한계를 세울 때 discard 를 알릴지 정한다.
	 * 값 범위: 참/거짓. ESE(Extent Space Efficient) 볼륨처럼 실제로 공간을
	 * 돌려줄 수 있는 장치에서만 참이다.
	 * 동기화: 상수. */
	bool has_discard;

	/* [한국어] 등록된 디시플린 목록에 매다는 고리.
	 * 설정자·읽는 자: 디시플린 등록·해제 코드가 목록에 붙이고 뗀다.
	 * 값 범위: 목록에 붙어 있는 동안 유효.
	 * 동기화: 디시플린 목록을 지키는 잠금이 따로 있으며, 그 잠금은 dasd.c
	 * 소관이라 이 헤더에서는 확인 못 함. */
	struct list_head list;	/* used for list of disciplines */

	/*
	 * Device recognition functions. check_device is used to verify
	 * the sense data and the information returned by read device
	 * characteristics. It returns 0 if the discipline can be used
	 * for the device in question. uncheck_device is called during
	 * device shutdown to deregister a device from its discipline.
	 */
	/* [한국어] 이 디시플린이 그 장치를 맡을 수 있는지 판정한다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 장치 인식 단계가 디시플린을 하나씩 시도하며 부른다.
	 * 값 범위: 0 이면 맡을 수 있다는 뜻이고, 그 밖이면 다음 디시플린으로 넘어간다.
	 * 옆의 상류 주석이 판정 근거(센스 데이터와 장치 특성 읽기 결과)를 밝힌다.
	 * 동기화: 상수 포인터. */
	int (*check_device) (struct dasd_device *);
	/* [한국어] 장치를 내릴 때 디시플린에서 등록을 푼다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 장치 종료 경로.
	 * 값 범위: 반환값이 없어 실패를 알릴 수 없다.
	 * 동기화: 상수 포인터. */
	void (*uncheck_device) (struct dasd_device *);

	/*
	 * do_analysis is used in the step from device state "basic" to
	 * state "accept". It returns 0 if the device can be made ready,
	 * it returns -EMEDIUMTYPE if the device can't be made ready or
	 * -EAGAIN if do_analysis started a ccw that needs to complete
	 * before the analysis may be repeated.
	 */
	/* [한국어] 장치를 "basic" 에서 "accept" 상태로 올릴 때 볼륨을 분석한다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 장치 상태 기계.
	 * 값 범위: 옆의 상류 주석대로 셋으로 갈린다 — 0(준비 가능),
	 * -EMEDIUMTYPE(준비 불가), 그리고 **-EAGAIN(아직 판단 못 함)** 이다.
	 * 마지막 값이 특징적인데, 분석을 위해 CCW 를 띄웠으니 그것이 끝난 뒤
	 * 다시 물어보라는 뜻이다. 즉 이 콜백은 여러 번 불릴 수 있다.
	 * 동기화: 상수 포인터. */
	int (*do_analysis) (struct dasd_block *);

	/*
	 * This function is called, when new paths become available.
	 * Disciplins may use this callback to do necessary setup work,
	 * e.g. verify that new path is compatible with the current
	 * configuration.
	 */
	/* [한국어] 새 경로가 생겼을 때 디시플린이 준비 작업을 하게 한다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 경로 상태가 바뀔 때.
	 * 값 범위: 0 = 성공. 두 __u8 인자는 새로 생긴 경로와 관련된 마스크로
	 * 보이나, 각 인자의 정확한 의미는 이 트리에서 확인 못 함.
	 * 동기화: 상수 포인터.
	 * 이름의 pe 는 Path Event 의 줄임으로 보인다. */
	int (*pe_handler)(struct dasd_device *, __u8, __u8);

	/*
	 * Last things to do when a device is set online, and first things
	 * when it is set offline.
	 */
	/* [한국어] 온라인으로 올릴 때의 마지막 단계.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 장치 상태 기계.
	 * 값 범위: 0 = 성공.
	 * 동기화: 상수 포인터. */
	int (*basic_to_ready) (struct dasd_device *);
	/* [한국어] 온라인에서 내릴 때의 첫 단계. 위 basic_to_ready 의 짝이다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*online_to_ready) (struct dasd_device *);
	/* [한국어] 더 내려 "known" 상태로 갈 때 부른다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공.
	 * 상류 주석이 앞의 둘만 설명하고 이것은 나중에 더해진 것으로 보인다. */
	int (*basic_to_known)(struct dasd_device *);

	/* [한국어] 이 장치가 한 요청에 받을 수 있는 최대 섹터 수.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 블록 큐 한계를 세울 때.
	 * 값 범위: 섹터 수. 디시플린마다 CCW 사슬 길이 제약이 달라 값이 다르다.
	 * 동기화: 상수 포인터.
	 * [상류 코드 관찰] 바로 아래 상류 주석 블록이 `/* (struct dasd_device *);`
	 * 로 시작한다 — 주석 여는 기호 뒤에 함수 인자 목록의 잔해가 남아 있어,
	 * 이 필드의 옛 선언 일부가 주석 안으로 딸려 들어간 것으로 보인다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	unsigned int (*max_sectors)(struct dasd_block *);
	/* (struct dasd_device *);
	 * Device operation functions. build_cp creates a ccw chain for
	 * a block device request, start_io starts the request and
	 * term_IO cancels it (e.g. in case of a timeout). format_device
	 * formats the device and check_device_format compares the format of
	 * a device with the expected format_data.
	 * handle_terminated_request allows to examine a cqr and prepare
	 * it for retry.
	 */
	/* [한국어] 블록 계층 요청 하나를 **CCW 사슬로 번역한다.** 이 표에서 가장 중요한
	 * 콜백이며, 디시플린이 존재하는 이유 그 자체다 — ECKD 와 FBA 가 같은
	 * 읽기 요청을 전혀 다른 채널 프로그램으로 바꾼다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 블록 I/O 경로가 요청마다 부른다.
	 * 값 범위: 만들어진 요청 구조체, 또는 오류 포인터.
	 * 동기화: 상수 포인터. */
	struct dasd_ccw_req *(*build_cp) (struct dasd_device *,
					  struct dasd_block *,
					  struct request *);
	/* [한국어] 만들어 둔 요청을 하드웨어에 띄운다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: I/O 제출 경로.
	 * 값 범위: 0 = 성공.
	 * 동기화: 상수 포인터. */
	int (*start_IO) (struct dasd_ccw_req *);
	/* [한국어] 띄운 요청을 취소한다. 옆의 상류 주석대로 시간 초과 같은 상황에서 쓴다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*term_IO) (struct dasd_ccw_req *);
	/* [한국어] 취소된 요청을 살펴 다시 시도할 수 있게 다듬는다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*handle_terminated_request) (struct dasd_ccw_req *);
	/* [한국어] 볼륨을 포맷한다. DASD 는 트랙 단위 포맷이 필요한 장치라 이 동작이
	 * 드라이버에 있다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*format_device) (struct dasd_device *,
			      struct format_data_t *, int);
	/* [한국어] 장치의 현재 포맷이 기대한 것과 맞는지 확인한다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 맞음. */
	int (*check_device_format)(struct dasd_device *,
				   struct format_check_t *, int);
	/* [한국어] build_cp 이 만든 것을 푼다. 그 짝이며, 요청이 끝난 뒤 불린다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*free_cp) (struct dasd_ccw_req *, struct request *);

	/*
	 * Error recovery functions. examine_error() returns a value that
	 * indicates what to do for an error condition. If examine_error()
	 * returns 'dasd_era_recover' erp_action() is called to create a
	 * special error recovery ccw. erp_postaction() is called after
	 * an error recovery ccw has finished its execution. dump_sense
	 * is called for every error condition to print the sense data
	 * to the console.
	 */
	/* [한국어] 오류를 어떻게 복구할지 정해 복구용 CCW 를 만든다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 오류 복구(ERP) 경로.
	 * 값 범위: 함수 포인터를 돌려준다 — 즉 **다음에 부를 함수를 반환하는**
	 * 형태라, 복구가 여러 단계로 이어질 수 있다.
	 * 동기화: 상수 포인터. */
	dasd_erp_fn_t(*erp_action) (struct dasd_ccw_req *);
	/* [한국어] 복구용 CCW 가 끝난 뒤 뒷정리를 한다. 위와 같은 방식으로 다음 단계
	 * 함수를 돌려준다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	dasd_erp_fn_t(*erp_postaction) (struct dasd_ccw_req *);
	/* [한국어] 센스 데이터를 콘솔에 찍는다. 옆의 상류 주석대로 **모든 오류 상황에서**
	 * 불린다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*dump_sense) (struct dasd_device *, struct dasd_ccw_req *,
			    struct irb *);
	/* [한국어] 같은 일을 debug 기능 쪽에 남기는 판. 콘솔이 아니라 순환 버퍼로 가므로
	 * 훨씬 자주 불려도 부담이 적다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	void (*dump_sense_dbf) (struct dasd_device *, struct irb *, char *);
	/* [한국어] 장치 상태가 바뀌었는지 인터럽트 응답 블록에서 살핀다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*check_for_device_change) (struct dasd_device *,
					 struct dasd_ccw_req *,
					 struct irb *);

        /* i/o control functions. */
	/* [한국어] 디스크 기하 정보(실린더/헤드/섹터)를 채운다. DASD 는 실제로
	 * 그 구조를 갖는 장치라 이 값이 꾸며 낸 것이 아니다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: HDIO_GETGEO 계열 ioctl.
	 * 값 범위: 0 = 성공.
	 * 동기화: 상수 포인터. */
	int (*fill_geometry) (struct dasd_block *, struct hd_geometry *);
	/* [한국어] 장치 정보를 사용자 공간 구조체에 채운다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*fill_info) (struct dasd_device *, struct dasd_information2_t *);
	/* [한국어] 디시플린 고유 ioctl 을 처리한다. 공통 코드가 모르는 명령이 여기로 온다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공, -ENOTTY 계열이면 알 수 없는 명령이다. */
	int (*ioctl) (struct dasd_block *, unsigned int, void __user *);

	/* reload device after state change */
	/* [한국어] 상태가 바뀐 뒤 장치를 다시 읽어 들인다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*reload) (struct dasd_device *);

	/* [한국어] 이 장치의 UID 를 읽어 위 struct dasd_uid 를 채운다. 별칭 처리가
	 * 그 결과로 기본 장치와 별칭을 짝짓는다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*get_uid) (struct dasd_device *, struct dasd_uid *);
	/* [한국어] UID 검증 작업을 작업 큐에 올린다. 인터럽트 문맥에서 곧바로 할 수
	 * 없는 일을 미루는 통로다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*kick_validate) (struct dasd_device *);
	/* [한국어] 주의(attention) 인터럽트를 살핀다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*check_attention)(struct dasd_device *, __u8);
	/* [한국어] 이 볼륨에 접근 중인 호스트 수를 센다. 메인프레임에서는 여러 LPAR 이
	 * 같은 볼륨을 공유할 수 있어 의미가 있는 값이다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 호스트 수. */
	int (*host_access_count)(struct dasd_device *);
	/* [한국어] 그 호스트 목록을 seq_file 로 찍는다. 위 개수의 상세판이며 procfs
	 * 계열 출력에 쓴다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*hosts_print)(struct dasd_device *, struct seq_file *);
	/* [한국어] HPF(High Performance FICON) 오류를 처리한다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*handle_hpf_error)(struct dasd_device *, struct irb *);
	/* [한국어] HPF 를 끈다. 위에서 오류가 반복되면 성능 모드를 포기하고 물러나는
	 * 경로다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	void (*disable_hpf)(struct dasd_device *);
	/* [한국어] HPF 가 켜져 있는지 묻는다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 이 아니면 켜져 있다. */
	int (*hpf_enabled)(struct dasd_device *);
	/* [한국어] 지정한 경로를 초기화한다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 반환값이 없다. */
	void (*reset_path)(struct dasd_device *, __u8);

	/*
	 * Extent Space Efficient (ESE) relevant functions
	 */
	/* [한국어] 이 볼륨이 ESE(Extent Space Efficient) 인지 묻는다. ESE 는 실제로
	 * 쓴 만큼만 물리 공간을 차지하는 씬 프로비저닝 볼륨이라, 아래 용량
	 * 관련 콜백들이 의미를 갖는 것도 이 경우다.
	 * 설정자: 각 디시플린의 정적 표.
	 * 읽는 자: 용량 보고와 discard 처리 경로.
	 * 값 범위: 0 이 아니면 ESE 다.
	 * 동기화: 상수 포인터. */
	int (*is_ese)(struct dasd_device *);
	/* Capacity */
	/* [한국어] 실제로 할당된 공간.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 음수면 오류. ESE 가 아닌 볼륨에서는 의미가 없다. */
	int (*space_allocated)(struct dasd_device *);
	/* [한국어] 구성된 공간 — 즉 논리적으로 약속한 크기다. 위 space_allocated 와의
	 * 차이가 곧 아직 물리 공간을 차지하지 않은 부분이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*space_configured)(struct dasd_device *);
	/* [한국어] 논리 용량.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*logical_capacity)(struct dasd_device *);
	/* [한국어] 공간을 돌려준다. discard 요청이 실제로 물리 공간을 해제하는 통로다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*release_space)(struct dasd_device *, struct format_data_t *);
	/* Extent Pool */
	/* [한국어] 이 볼륨이 속한 익스텐트 풀의 ID.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*ext_pool_id)(struct dasd_device *);
	/* [한국어] 익스텐트 하나의 크기.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*ext_size)(struct dasd_device *);
	/* [한국어] 익스텐트 풀의 용량이 경고 수위에 닿았는지.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 이 아니면 경고 수위다. **ESE 볼륨의 진짜 위험** 은 논리
	 * 용량이 아니라 뒤를 받치는 풀이 마르는 것이라, 이런 질의가 필요하다. */
	int (*ext_pool_cap_at_warnlevel)(struct dasd_device *);
	/* [한국어] 그 경고 수위 자체(임계값)를 묻는다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*ext_pool_warn_thrshld)(struct dasd_device *);
	/* [한국어] 풀이 공간 부족(out of space) 상태인지.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*ext_pool_oos)(struct dasd_device *);
	/* [한국어] 풀이 실제로 말라 요청을 처리할 수 없을 때 부른다. 위 질의들과 달리
	 * **요청을 함께 받는다** — 어느 요청이 걸렸는지 알아야 처리할 수 있기
	 * 때문이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	int (*ext_pool_exhaust)(struct dasd_device *, struct dasd_ccw_req *);
	/* [한국어] ESE 볼륨에서 아직 할당되지 않은 트랙에 쓰기가 닿았을 때, 그 자리를
	 * 포맷하는 요청을 만든다. 씬 프로비저닝의 핵심 동작이며 — 쓰기가
	 * 공간 할당을 유발한다 — 그래서 요청 하나가 두 단계로 나뉜다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 만들어진 요청, 또는 오류 포인터. */
	struct dasd_ccw_req *(*ese_format)(struct dasd_device *,
					   struct dasd_ccw_req *, struct irb *);
	/* [한국어] ESE 볼륨에서 **아직 할당되지 않은 자리를 읽었을 때** 처리한다.
	 * 그런 자리에는 실제 데이터가 없으므로 0 을 채워 돌려주는 식이 된다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*ese_read)(struct dasd_ccw_req *, struct irb *);
	/* [한국어] PPRC 구성 정보를 읽어 위 dasd_pprc_data_sc4 구조체를 채운다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*pprc_status)(struct dasd_device *, struct	dasd_pprc_data_sc4 *);
	/* [한국어] 이 장치에서 PPRC 가 켜져 있는지.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 참/거짓. 이 표에서 bool 을 돌려주는 몇 안 되는 콜백이다. */
	bool (*pprc_enabled)(struct dasd_device *);
	/* [한국어] 복사 쌍의 주-부를 맞바꾼다. 두 문자열 인자는 버스 ID 로 보이며,
	 * 위 dasd_copy_entry 가 장치를 문자열로도 붙들어 두는 것과 맞아떨어진다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*copy_pair_swap)(struct dasd_device *, char *, char *);
	/* [한국어] 장치가 살아 있는지 확인하는 가벼운 질의.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 = 성공. */
	int (*device_ping)(struct dasd_device *);
};

/* [한국어] DIAG 디시플린을 가리키는 전역 포인터.
 * 설정자: DIAG 디시플린 모듈이 올라올 때 자기를 등록한다.
 * 읽는 자: 장치 인식 코드가 DIAG 를 특별히 먼저 시도해야 할 때.
 * 값 범위: 유효한 포인터, 또는 모듈이 없으면 NULL.
 * 동기화: 모듈 적재 시점에 한 번 정해진다.
 * **디시플린 중 이것만 전역으로 노출된다** — 나머지는 등록 목록을 통해
 * 찾는데, DIAG 만 이름으로 직접 가리켜야 하는 자리가 있다는 뜻이다. */
extern struct dasd_discipline *dasd_diag_discipline_pointer;

/* Trigger IDs for extended error reporting DASD EER and autoquiesce */
enum eer_trigger {
	/* [한국어] 치명적 오류. 아래 값들이 1 부터 이어지는 것이 중요한데,
	 * DASD_EER_VALID 매크로가 그 연속성을 전제로 마스크를 만든다. */
	DASD_EER_FATALERROR = 1,
	/* [한국어] 쓸 수 있는 경로가 하나도 남지 않았다. */
	DASD_EER_NOPATH,
	/* [한국어] 장치 상태가 바뀌었다. */
	DASD_EER_STATECHANGE,
	/* [한국어] PPRC 복제가 중단됐다. */
	DASD_EER_PPRCSUSPEND,
	/* [한국어] 공간이 모자란다. 위 ESE 익스텐트 풀 고갈이 이 트리거로 올라온다. */
	DASD_EER_NOSPC,
	/* [한국어] 시간 초과가 거듭됐다. */
	DASD_EER_TIMEOUTS,
	/* [한국어] I/O 시작 단계에서 문제가 생겼다. */
	DASD_EER_STARTIO,

	/* enum end marker, only add new trigger above */
	/* [한국어] 열거의 끝 표시. **값이 아니라 개수** 로 쓰이며, 바로 아래
	 * DASD_EER_VALID 가 이것으로 마스크를 만든다. 그래서 새 트리거는
	 * 반드시 이 줄 위에 더해야 한다 — 옆의 상류 주석이 그것을 못박고 있다. */
	DASD_EER_MAX,
	/* [한국어] 자동 정지(autoquiesce)용 내부 트리거. **31 로 건너뛰어 있다** —
	 * 위 연속 구간과 떨어뜨려 두어야 DASD_EER_VALID 마스크에 섞이지 않기
	 * 때문이다. 옆의 상류 주석대로 사용자에게 노출되지 않는 값이다. */
	DASD_EER_AUTOQUIESCE = 31, /* internal only */
};

/* [한국어] 사용자에게 노출되는 트리거들의 비트 마스크.
 * DASD_EER_MAX 만큼 1 을 밀어 올린 뒤 1 을 빼, 0 번부터 그 앞까지가
 * 모두 1 인 값을 만든다. 위 AUTOQUIESCE(31)가 이 범위 밖에 있는 이유가
 * 바로 이 계산이다. */
#define DASD_EER_VALID ((1U << DASD_EER_MAX) - 1)

/* DASD path handling */

/* [한국어] 이 경로를 쓸 수 있다. */
#define DASD_PATH_OPERATIONAL  1
/* [한국어] 이 경로를 검증해야 한다(To Be Verified). */
#define DASD_PATH_TBV	       2
/* [한국어] 선호 경로(Preferred Path). */
#define DASD_PATH_PP	       3
/* [한국어] 비선호 경로(Non-Preferred Path). 위와 짝을 이룬다. */
#define DASD_PATH_NPP	       4
/* [한국어] 배선이 잘못된 경로 — 기대한 장치가 아닌 곳에 닿는다. */
#define DASD_PATH_MISCABLED    5
/* [한국어] 이 경로에서는 HPF(High Performance FICON)를 쓸 수 없다. */
#define DASD_PATH_NOHPF        6
/* [한국어] CUIR(Control Unit Initiated Reconfiguration)로 인해 잠긴 경로다. */
#define DASD_PATH_CUIR	       7
/* [한국어] 인터페이스 제어 검사(IFCC) 오류가 난 경로다. */
#define DASD_PATH_IFCC	       8
/* [한국어] FC 종단 보안(FC Endpoint Security) 관련 상태를 나타내는 경로다. */
#define DASD_PATH_FCSEC	       9

/* [한국어] 임계값의 최댓값. 32비트 부호 없는 정수의 상한을 그대로 적었다. */
#define DASD_THRHLD_MAX		4294967295U
/* [한국어] 간격의 최댓값. 위와 같은 값이며, 사실상 "제한 없음" 을 뜻한다. */
#define DASD_INTERVAL_MAX	4294967295U

/* FC Endpoint Security Capabilities */
/* [한국어] 이 경로는 FC 종단 보안을 지원하지 않는다. */
#define DASD_FC_SECURITY_UNSUP		0
/* [한국어] 인증만 한다 — 데이터는 평문으로 오간다. */
#define DASD_FC_SECURITY_AUTH		1
/* [한국어] FC-SP-2 규격의 암호화를 쓴다. */
#define DASD_FC_SECURITY_ENC_FCSP2	2
/* [한국어] ERAS 방식의 암호화를 쓴다. */
#define DASD_FC_SECURITY_ENC_ERAS	3

/* [한국어] 암호화를 뜻하는 표시 문자열. 아래 표에서 **서로 다른 두 값이 같은
 * 이름을 쓰기** 때문에 상수로 빼 두었다 — 사용자에게는 둘 다 그저
 * "암호화" 로 보이면 된다는 뜻이다. */
#define DASD_FC_SECURITY_ENC_STR	"Encryption"
static const struct {
	/* [한국어] 이 항목이 대응하는 보안 수준 값. */
	u8 value;
	/* [한국어] 그 값을 사람이 읽는 이름. */
	char *name;
/* [한국어] 보안 수준 값을 이름으로 옮기는 표.
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: sysfs 로 경로의 보안 수준을 보여 주는 코드.
 * 값 범위: 아래 네 항목.
 * 동기화: 상수라 필요 없다.
 * **헤더에 정의된 static 배열** 이라, 이 헤더를 포함하는 번역 단위마다
 * 사본이 하나씩 생긴다. */
} dasd_path_fcs_mnemonics[] = {
	/* [한국어] 지원하지 않음. */
	{ DASD_FC_SECURITY_UNSUP,	"Unsupported" },
	/* [한국어] 인증만. */
	{ DASD_FC_SECURITY_AUTH,	"Authentication" },
	/* [한국어] FC-SP-2 암호화 — 위 상수 문자열을 쓴다. */
	{ DASD_FC_SECURITY_ENC_FCSP2,	DASD_FC_SECURITY_ENC_STR },
	/* [한국어] ERAS 암호화 — 같은 문자열을 쓴다. */
	{ DASD_FC_SECURITY_ENC_ERAS,	DASD_FC_SECURITY_ENC_STR },
};

/* [한국어]
 * dasd_path_get_fcs_str - FC 종단 보안 수준 값을 사람이 읽는 이름으로 옮긴다
 *
 * @val: 보안 수준 값. DASD_FC_SECURITY_ 계열 중 하나를 기대한다.
 * @return: 그 수준의 이름 문자열. 모르는 값이면 "Unsupported".
 *
 * 위 표를 앞에서부터 훑어 값이 맞는 항목의 이름을 돌려주는 선형 탐색이다.
 * 항목이 넷뿐이라 표를 순회하는 비용이 문제가 되지 않는다.
 *
 * **모르는 값을 만났을 때 0번 항목의 이름을 돌려주는 것** 이 이 함수의
 * 요점이다. NULL 을 돌려주지 않으므로 호출자가 검사할 필요가 없고,
 * 결과적으로 알 수 없는 값은 "지원하지 않음" 으로 표시된다.
 *
 * 값과 이름이 일대일이 아니라는 점도 눈여겨볼 만하다 — 표의 마지막 두
 * 항목이 같은 문자열을 쓰므로, 암호화 방식이 둘이어도 사용자에게는
 * 하나로 보인다.
 *
 * 실행 컨텍스트: sysfs 출력. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 어떤 입력에도 유효한 문자열을 돌려준다.
 *
 * 호출 체인:
 *   sysfs 의 경로 보안 수준 표시 → [이 함수]
 */
static inline char *dasd_path_get_fcs_str(int val)
{
	/* [한국어] 루프 첨자. */
	int i;

	/* [한국어] 표를 앞에서부터 훑는다. 항목이 넷뿐이라 선형 탐색으로 충분하다. */
	for (i = 0; i < ARRAY_SIZE(dasd_path_fcs_mnemonics); i++) {
		/* [한국어] 값이 맞는 항목을 찾았다. */
		if (dasd_path_fcs_mnemonics[i].value == val)
			/* [한국어] 그 이름을 돌려준다. */
			return dasd_path_fcs_mnemonics[i].name;
	}

	/* [한국어] 찾지 못했다. **0번 항목("Unsupported")을 돌려주므로** 호출자가
	 * NULL 을 검사할 필요가 없다. */
	return dasd_path_fcs_mnemonics[0].name;
}

struct dasd_path {
	/* [한국어] 이 경로의 상태 플래그 모음.
	 * 설정자·읽는 자: 아래에 이어지는 dasd_path_ 계열 접근자들이 비트
	 * 연산으로 세우고 지우고 확인한다.
	 * 값 범위: 위 DASD_PATH_ 계열 비트 번호들.
	 * 타입이 unsigned long 인 것은 비트 연산 계열이 그것을 요구하기 때문이다.
	 * 동기화: [상류 코드 관찰] **대부분 원자적이지 않다.** 이 헤더에서
	 * 이 필드를 고치는 자리 18곳 중 16곳이 비원자 판(__set_bit/__clear_bit)을
	 * 쓰고, 원자 판(set_bit/clear_bit)을 쓰는 것은 DASD_PATH_IFCC 를 다루는
	 * 두 곳(원본 :1109, :1119)뿐이다. 읽는 쪽은 아홉 곳 모두 test_bit 이다.
	 * 즉 IFCC 플래그만 다른 문맥과 경쟁할 수 있다고 보고 원자 판을 쓴 셈인데,
	 * 그 구별의 근거는 코드에 적혀 있지 않다. 나머지 플래그들은 바깥의
	 * 다른 잠금이 지킨다는 전제로 보이나, 그 잠금이 무엇인지는 이 헤더에서
	 * 확인 못 함. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	unsigned long flags;
	/* [한국어] 이 경로가 속한 채널 서브시스템 ID.
	 * 설정자: 경로 검증 코드가 채운다.
	 * 읽는 자: sysfs 출력과 경로 식별.
	 * 값 범위: 8비트.
	 * 동기화: 경로 구성 이후 사실상 불변. */
	u8 cssid;
	/* [한국어] 서브시스템 ID.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 8비트. */
	u8 ssid;
	/* [한국어] 채널 경로 ID — 물리적으로 어느 채널을 타는지 가리킨다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 8비트.
	 * 위 셋(cssid/ssid/chpid)이 합쳐져 이 경로를 유일하게 식별한다. */
	u8 chpid;
	/* [한국어] 이 경로의 구성 데이터.
	 * 설정자: 경로 검증이 하드웨어에서 읽어 채운다.
	 * 읽는 자: 경로가 기대한 장치에 닿는지 대조할 때.
	 * 값 범위: 유효한 포인터, 또는 아직 읽지 않았으면 NULL.
	 * 동기화: 경로 구성 이후 불변. */
	struct dasd_conf_data *conf_data;
	/* [한국어] 이 경로에서 난 오류 횟수.
	 * 설정자·읽는 자: 오류 처리 경로가 올리고, 임계값과 견줄 때 읽는다.
	 * 값 범위: 0 이상.
	 * 동기화: **원자 변수라 잠금이 없다.** 오류가 인터럽트 문맥에서
	 * 집계될 수 있어서다. */
	atomic_t error_count;
	/* [한국어] 마지막 오류가 난 시각.
	 * 설정자: 오류 처리 경로.
	 * 읽는 자: 오류가 일정 기간 안에 몰렸는지 판단할 때 — 위 error_count 와
	 * 짝을 이뤄 "얼마 동안 몇 번" 을 본다.
	 * 값 범위: 커널 시각 값.
	 * 동기화: 없다. */
	unsigned long errorclk;
	/* [한국어] 이 경로의 FC 종단 보안 수준.
	 * 설정자: 경로 검증이 채운다.
	 * 읽는 자: 위 dasd_path_get_fcs_str() 을 거쳐 sysfs 에 표시된다.
	 * 값 범위: DASD_FC_SECURITY_ 계열 넷.
	 * 동기화: 경로 구성 이후 불변. */
	u8 fc_security;
	/* [한국어] 이 경로의 sysfs 표현.
	 * 설정자: 경로를 sysfs 에 등록할 때.
	 * 읽는 자: 위 to_dasd_path 매크로가 이것에서 경로 구조체를 되찾는다.
	 * 값 범위: 내장 kobject 이며 포인터가 아니다.
	 * 동기화: kobject 코어가 참조 계수를 관리한다.
	 * **해제 콜백이 비어 있다** — 위 dasd_path_release() 참조. */
	struct kobject kobj;
	/* [한국어] 이 경로가 지금 sysfs 에 올라와 있는지.
	 * 설정자·읽는 자: 등록과 해제 경로가 중복 처리를 막는 데 쓴다.
	 * 값 범위: 참/거짓.
	 * 동기화: 경로 구성 경로에서만 다뤄진다. */
	bool in_sysfs;
};

/* [한국어] kobject 에서 그것을 품은 경로 구조체를 되찾는다. sysfs 콜백이
 * kobject 만 받기 때문에 필요하다. */
#define to_dasd_path(path) container_of(path, struct dasd_path, kobj)

/* [한국어]
 * dasd_path_release - kobject 해제 콜백. 일부러 아무것도 하지 않는다
 *
 * @kobj: 해제되는 kobject. 쓰지 않는다.
 *
 * **본문이 비어 있는 것이 의도** 이며, 안의 상류 주석이 그 이유를 밝힌다 —
 * struct dasd_path 의 메모리는 이 kobject 와 수명이 따로이고,
 * dasd_free_device() 가 부를 때 함께 풀린다.
 *
 * kobject 규약상 release 콜백은 반드시 있어야 한다. 없으면 커널이 경고를
 * 내므로, 실제로 풀 것이 없더라도 빈 함수를 두어 규약만 만족시킨 형태다.
 *
 * 경로 구조체가 struct dasd_device 안에 **배열로 박혀 있어서** 이렇게
 * 된다 — 개별적으로 할당된 것이 아니라 장치 구조체의 일부라, 참조 계수가
 * 0 이 되어도 따로 풀 것이 없다.
 *
 * 실행 컨텍스트: kobject 참조 계수가 0 이 될 때. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   kobject 코어 → kobj_type.release == [이 함수]
 */
static inline void dasd_path_release(struct kobject *kobj)
{
/* Memory for the dasd_path kobject is freed when dasd_free_device() is called */
}


struct dasd_profile_info {
	/* [한국어] 처리한 요청 수. 아래 필드들과 함께 **옛 형식(dasd_profile_info_t)과
	 * 그대로 호환되는 부분** 이라, 순서와 크기를 바꿀 수 없다.
	 * 설정자: I/O 완료 경로가 올린다.
	 * 읽는 자: debugfs 의 프로파일 출력.
	 * 값 범위: 0 이상. 넘침을 검사하지 않는다.
	 * 동기화: 아래 struct dasd_profile 의 잠금이 지킨다. */
	/* legacy part of profile data, as in dasd_profile_info_t */
	/* [한국어] 처리한 섹터 수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_reqs;	 /* number of requests processed */
	/* [한국어] 요청 크기의 히스토그램. 칸이 32개인 것은 아래 다른 히스토그램들과
	 * 같으며, 크기를 2의 거듭제곱 구간으로 나눠 세는 방식으로 보인다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_sects;	 /* number of sectors processed */
	/* [한국어] 요청 처리 시간의 히스토그램 — 전체 소요 시간이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_secs[32];	 /* histogram of request's sizes */
	/* [한국어] 섹터당 처리 시간의 히스토그램. 위 times 를 크기로 나눈 값이라,
	 * 큰 요청과 작은 요청을 같은 잣대로 견줄 수 있다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_times[32];	 /* histogram of requests's times */
	/* [한국어] **요청을 만든 뒤 시작하기까지** 의 시간. 큐에서 기다린 시간에
	 * 해당한다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_timps[32];	 /* h. of requests's times per sector */
	/* [한국어] **시작한 뒤 인터럽트가 올 때까지** 의 시간. 실제 하드웨어 처리
	 * 시간에 해당한다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_time1[32];	 /* hist. of time from build to start */
	/* [한국어] 같은 구간을 섹터당으로 나눈 히스토그램.
	 * [상류 코드 관찰] 오른쪽 상류 주석이 바로 위 time2 와 **똑같이**
	 * "time from start to irq" 라고 적혀 있어, 섹터당이라는 차이가 주석에
	 * 드러나지 않는다. 필드 이름의 ps 접미사만이 그것을 알려 준다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_time2[32];	 /* hist. of time from start to irq */
	/* [한국어] **인터럽트 뒤 끝날 때까지** 의 시간. 위 셋(time1/time2/time3)이
	 * 합쳐져 요청 하나의 생애를 세 구간으로 나눈다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_time2ps[32]; /* hist. of time from start to irq */
	/* [한국어] 요청이 채널 큐에 몇 개나 쌓여 있었는지의 히스토그램. 대기열 깊이
	 * 분포를 보여 준다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_time3[32];	 /* hist. of time from irq to end */
	unsigned int dasd_io_nr_req[32]; /* hist. of # of requests in chanq */

	/* [한국어] 집계를 시작한(또는 마지막으로 초기화한) 시각. 여기서부터 아래가
	 * 옛 형식에 없던 **새 필드들** 이다.
	 * 설정자: 프로파일을 초기화할 때.
	 * 읽는 자: 출력에서 집계 구간을 표시할 때.
	 * 값 범위: 커널 시각.
	 * 동기화: 아래 struct dasd_profile 의 잠금이 지킨다. */
	/* new data */
	/* [한국어] 별칭 장치를 거쳐 나간 요청 수. PAV 가 실제로 얼마나 쓰이는지
	 * 보여 주는 지표다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct timespec64 starttod;	   /* time of start or last reset */
	/* [한국어] 전송 모드(HPF)로 나간 요청 수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_alias;	   /* requests using an alias */
	/* [한국어] 읽기 요청 수. 여기서부터 읽기만 따로 집계하는 묶음이 이어진다 —
	 * 위쪽 묶음이 읽기와 쓰기를 합친 값이므로, 이 둘의 차가 곧 쓰기다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_io_tpm;	   /* requests using transport mode */
	/* [한국어] 읽은 섹터 수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_reqs;	   /* total number of read  requests */
	/* [한국어] 별칭을 거친 읽기 요청 수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_sects;	   /* total number read sectors */
	/* [한국어] 전송 모드로 나간 읽기 요청 수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_alias;	   /* read request using an alias */
	/* [한국어] 읽기 요청 크기의 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_tpm;	   /* read requests in transport mode */
	/* [한국어] 읽기 요청 처리 시간의 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_secs[32];   /* histogram of request's sizes */
	/* [한국어] 읽기의 큐 대기 시간 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_times[32];  /* histogram of requests's times */
	/* [한국어] 읽기의 하드웨어 처리 시간 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_time1[32];  /* hist. time from build to start */
	/* [한국어] 읽기의 완료 처리 시간 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_time2[32];  /* hist. of time from start to irq */
	/* [한국어] 읽기 시점의 큐 깊이 히스토그램.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned int dasd_read_time3[32];  /* hist. of time from irq to end */
	/* [한국어] 요청 시간의 총합. 히스토그램과 달리 **평균을 낼 수 있게** 합계를
	 * 따로 둔다 — 요청 수로 나누면 평균이 나온다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: unsigned long 이라 위 unsigned int 필드들보다 넓다. */
	unsigned int dasd_read_nr_req[32]; /* hist. of # of requests in chanq */
	/* [한국어] 큐 대기 시간의 총합.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned long dasd_sum_times;	   /* sum of request times */
	/* [한국어] 하드웨어 처리 시간의 총합.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned long dasd_sum_time_str;   /* sum of time from build to start */
	/* [한국어] 완료 처리 시간의 총합.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned long dasd_sum_time_irq;   /* sum of time from start to irq */
	unsigned long dasd_sum_time_end;   /* sum of time from irq to end */
};

/* [한국어] 이 프로파일의 debugfs 항목.
 * 설정자: 프로파일을 만들 때.
 * 읽는 자: 해제할 때 이 항목을 없앤다.
 * 값 범위: 유효한 dentry, 또는 debugfs 가 없으면 NULL.
 * 동기화: 생성 이후 불변. */
struct dasd_profile {
	/* [한국어] 실제 집계 자료.
	 * 설정자: 프로파일을 켤 때 할당하고, 끌 때 풀어 NULL 로 둔다.
	 * 읽는 자: 집계와 출력 양쪽.
	 * 값 범위: 유효한 포인터, 또는 **프로파일이 꺼져 있으면 NULL.**
	 * 그 NULL 여부가 곧 켜짐/꺼짐 표시라, 집계 경로가 매번 확인한다.
	 * 동기화: 아래 잠금이 지킨다. */
	struct dentry *dentry;
	/* [한국어] 위 data 를 지키는 스핀락.
	 * 설정자: 프로파일을 만들 때 초기화한다.
	 * 읽는 자: 집계와 출력, 그리고 켜고 끄는 경로.
	 * 값 범위: 스핀락.
	 * 동기화: **I/O 완료 경로에서 집계가 일어나므로** 인터럽트를 막는 판으로
	 * 잡아야 한다. */
	struct dasd_profile_info *data;
	spinlock_t lock;
};

/* [한국어] 포맷 중인 트랙 목록에 매다는 고리.
 * 설정자·읽는 자: 포맷 진행 상황을 추적하는 코드.
 * 값 범위: 목록에 붙어 있는 동안 유효.
 * 동기화: 그 목록을 지키는 잠금은 dasd.c 소관이라 이 헤더에서는
 * 확인 못 함. */
struct dasd_format_entry {
	/* [한국어] 포맷 중인 트랙 번호.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 섹터 번호 타입이지만 트랙을 담는다.
	 * 동기화: 위와 같다.
	 * **같은 트랙을 두 번 포맷하지 않도록** 진행 중인 것을 기록해 두는
	 * 용도로 보인다. */
	struct list_head list;
	sector_t track;
};

struct dasd_device {
	/* Block device stuff. */
	/* [한국어] 이 장치에 딸린 블록 장치 부분.
	 * 설정자: 장치가 블록 계층에 올라올 때 만들어 붙인다.
	 * 읽는 자: I/O 경로 전반.
	 * 값 범위: 유효한 포인터, 또는 **블록 장치가 없으면 NULL.**
	 * 동기화: 생성 이후 불변.
	 * **device 와 block 이 나뉜 것이 이 드라이버의 뼈대** 다 — 별칭 장치처럼
	 * 블록 장치 없이 존재하는 것이 있기 때문이며, 그래서 이 필드가 NULL 일
	 * 수 있다. */
	struct dasd_block *block;

        /* [한국어] 이 장치의 색인 번호. 부번호 배분과 이름 짓기에 쓴다.
         * 설정자: 장치를 등록할 때.
         * 읽는 자: 이름과 부번호를 만들 때.
         * 값 범위: 0 이상.
         * 동기화: 등록 이후 불변. */
        unsigned int devindex;
	/* [한국어] 장치별 상태 플래그.
	 * 설정자·읽는 자: DASD_FLAG_ 계열 상수와 함께 원자적 비트 연산으로 다룬다.
	 * 값 범위: 그 상수들의 조합.
	 * 동기화: 원자적 비트 연산이라 잠금이 없다 — 타입이 unsigned long 인 것이
	 * 그 때문이다. */
	unsigned long flags;	   /* per device flags */
	/* [한국어] devmap 이 정한 기능 비트의 **사본.**
	 * 설정자: 장치를 만들 때 devmap 에서 복사해 온다.
	 * 읽는 자: 읽기 전용 여부 같은 동작 결정.
	 * 값 범위: DASD_FEATURE_ 계열의 조합.
	 * 동기화: 옆의 상류 주석이 **읽기 전용** 이라고 못박고 있다 — 바꾸려면
	 * devmap 쪽 원본을 고쳐야 한다. */
	unsigned short features;   /* copy of devmap-features (read-only!) */

	/* extended error reporting stuff (eer) */
	/* [한국어] 확장 오류 보고(EER)용으로 미리 잡아 둔 요청.
	 * 설정자: EER 을 켤 때 할당해 붙인다.
	 * 읽는 자: 오류를 보고해야 할 때.
	 * 값 범위: 유효한 포인터, 또는 EER 이 꺼져 있으면 NULL.
	 * 동기화: EER 설정 경로에서만 다뤄진다.
	 * **미리 잡아 두는 이유** 는 오류가 난 상황에서 새로 할당하려다 실패하면
	 * 보고 자체를 못 하기 때문이다. */
	struct dasd_ccw_req *eer_cqr;

	/* Device discipline stuff. */
	/* [한국어] 지금 쓰는 디시플린.
	 * 설정자: 장치 인식이 맞는 디시플린을 찾아 붙인다.
	 * 읽는 자: 위 vtable 의 모든 호출.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 인식 이후 불변. */
	struct dasd_discipline *discipline;
	/* [한국어] **기반** 디시플린. 별칭 장치가 자기 디시플린과 별개로 기본 장치의
	 * 것을 함께 알아야 할 때 쓴다.
	 * 설정자·읽는 자: 별칭 처리 코드.
	 * 값 범위: 유효한 포인터, 또는 해당 없으면 NULL.
	 * 동기화: 인식 이후 불변. */
	struct dasd_discipline *base_discipline;
	/* [한국어] 디시플린 고유의 사적 자료.
	 * 설정자: 디시플린의 check_device 가 자기 구조체를 할당해 붙인다.
	 * 읽는 자: 그 디시플린의 콜백들만.
	 * 값 범위: 디시플린마다 다른 타입이라 void 로 두었다 — 공통 코드는
	 * 이것을 절대 들여다보지 않는다.
	 * 동기화: 디시플린이 스스로 관리한다. */
	void *private;
	/* [한국어] 이 장치로 가는 경로 여덟 개.
	 * 설정자·읽는 자: 경로 검증과 선택 코드.
	 * 값 범위: **배열이라 개수가 여덟로 고정** 이다. 채널 경로 마스크가
	 * 8비트인 것과 짝을 이룬다.
	 * 동기화: 각 경로의 flags 가 원자적 비트 연산으로 다뤄진다. */
	struct dasd_path path[8];
	/* [한국어] 온라인 경로 마스크(Online Path Mask) — 지금 쓸 수 있는 경로들의 비트다.
	 * 설정자: 경로 상태가 바뀔 때 갱신된다.
	 * 읽는 자: I/O 를 띄울 때 어느 경로로 보낼지 정하는 코드.
	 * 값 범위: 8비트. 비트 하나가 위 배열의 한 칸에 대응한다.
	 * 동기화: 경로 처리 경로에서 갱신된다. */
	__u8 opm;

	/* Device state and target state. */
	/* [한국어] 현재 상태와 목표 상태.
	 * 설정자·읽는 자: 장치 상태 기계. 목표를 정해 두면 상태 기계가 한
	 * 단계씩 그쪽으로 옮긴다.
	 * 값 범위: DASD_STATE_ 계열.
	 * 동기화: 아래 state_mutex 가 지킨다.
	 * **두 값을 한 줄에 선언한** 것이 그 짝 관계를 드러낸다. */
	int state, target;
	/* [한국어] 위 두 상태를 지키는 뮤텍스.
	 * 설정자: 장치를 만들 때 초기화한다.
	 * 읽는 자: 상태를 옮기는 모든 경로.
	 * 값 범위: 뮤텍스.
	 * 동기화: **뮤텍스라 인터럽트 문맥에서 쓸 수 없다** — 상태 전이가
	 * 언제나 프로세스 컨텍스트에서 일어난다는 뜻이다. */
	struct mutex state_mutex;
	/* [한국어] 장치가 정지된 이유의 비트 모음.
	 * 설정자·읽는 자: 정지와 재개를 다루는 코드.
	 * 값 범위: DASD_STOPPED_ 계열의 조합. **0 이 아니면 정지 상태** 이므로,
	 * 여러 이유가 겹쳐도 마지막 하나가 풀릴 때까지 재개되지 않는다.
	 * 동기화: 장치 잠금이 지킨다. */
	int stopped;		/* device (ccw_device_start) was stopped */

	/* reference count. */
        /* [한국어] 이 장치의 참조 계수.
         * 설정자·읽는 자: 참조를 잡고 놓는 코드.
         * 값 범위: 0 이상. 0 이 되면 해제된다.
         * 동기화: 원자 변수라 잠금이 없다. */
        atomic_t ref_count;

	/* ccw queue and memory for static ccw/erp buffers. */
	/* [한국어] 이 장치로 나갈 요청들의 큐.
	 * 설정자·읽는 자: I/O 제출과 완료 경로.
	 * 값 범위: 목록.
	 * 동기화: 장치 잠금이 지킨다. */
	struct list_head ccw_queue;
	/* [한국어] 아래 세 메모리 풀과 그 청크 목록을 지키는 스핀락.
	 * 설정자: 장치를 만들 때 초기화.
	 * 읽는 자: 요청 메모리를 잡고 놓는 코드.
	 * 값 범위: 스핀락.
	 * 동기화: I/O 경로에서 쓰이므로 인터럽트를 막는 판으로 잡는다. */
	spinlock_t mem_lock;
	/* [한국어] 일반 CCW 요청용으로 **미리 잡아 둔** 메모리 덩어리.
	 * 설정자: 장치를 만들 때 한 번 할당한다.
	 * 읽는 자: 요청을 만들 때 여기서 잘라 쓴다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 위 mem_lock 이 지킨다.
	 * **미리 잡아 두는 이유** 는 I/O 경로에서 메모리 할당이 실패하면 그
	 * 자체가 I/O 실패가 되기 때문이다 — 특히 메모리 압박을 푸는 스왑 I/O 가
	 * 이 장치로 나갈 수 있어 순환이 생긴다. */
	void *ccw_mem;
	/* [한국어] 오류 복구(ERP) 요청용 메모리 덩어리. 일반 요청과 **따로 두는 이유** 는
	 * 오류 복구가 일반 풀이 마른 상황에서도 동작해야 하기 때문이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	void *erp_mem;
	/* [한국어] ESE 볼륨 처리용 메모리 덩어리. 씬 프로비저닝 볼륨에서 쓰기가
	 * 공간 할당을 유발할 때 쓴다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	void *ese_mem;
	/* [한국어] 위 ccw_mem 을 잘라 쓴 청크들의 목록.
	 * 설정자·읽는 자: 메모리를 잡고 놓는 코드.
	 * 값 범위: 목록.
	 * 동기화: 위 mem_lock 이 지킨다. */
	struct list_head ccw_chunks;
	/* [한국어] erp_mem 의 청크 목록.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct list_head erp_chunks;
	/* [한국어] ese_mem 의 청크 목록.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct list_head ese_chunks;

	/* [한국어] 태스클릿이 이미 예약돼 있는지.
	 * 설정자·읽는 자: 인터럽트 처리기가 태스클릿을 예약하기 전에 확인한다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 원자 변수라 잠금이 없다 — **인터럽트 문맥에서 검사하고 세워야**
	 * 하기 때문이며, 이것으로 같은 태스클릿을 중복 예약하지 않는다. */
	atomic_t tasklet_scheduled;
        /* [한국어] 완료 처리를 미뤄 두는 태스클릿.
         * 설정자: 장치를 만들 때 초기화.
         * 읽는 자: 인터럽트 처리기가 예약한다.
         * 값 범위: 태스클릿.
         * 동기화: 위 원자 변수가 중복 예약을 막는다.
         * **인터럽트 문맥에서 할 수 없는 일** — 잠들 수 있는 처리 — 을 여기로
         * 넘기는 구조다. */
        struct tasklet_struct tasklet;
	/* [한국어] 일반 지연 작업.
	 * 설정자·읽는 자: 작업을 미룰 때 큐에 올린다.
	 * 값 범위: 작업 항목.
	 * 동기화: 작업 큐 계층이 관리한다. */
	struct work_struct kick_work;
	/* [한국어] 장치를 다시 읽어 들이는 작업.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct work_struct reload_device;
	/* [한국어] UID 검증을 미루는 작업. 위 디시플린의 kick_validate 콜백과 짝이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct work_struct kick_validate;
	/* [한국어] SUC(Summary Unit Check) 처리 작업. 제어 장치가 여러 장치의 상태
	 * 변화를 한 번에 알릴 때 쓰는 메커니즘으로 보이나, 자세한 규약은
	 * 이 트리에서 확인 못 함.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct work_struct suc_work;
	/* [한국어] 요청들을 다시 큐에 넣는 작업. 경로가 모두 끊겼다가 돌아온 뒤
	 * 같은 상황에서 쓴다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct work_struct requeue_requests;
	/* [한국어] 시간 초과 감시 타이머.
	 * 설정자: 요청을 띄울 때 건다.
	 * 읽는 자: 만료되면 시간 초과 처리로 들어간다.
	 * 값 범위: 타이머.
	 * 동기화: 타이머 계층이 관리한다. */
	struct timer_list timer;

	/* [한국어] 이 장치의 debug 기능 영역.
	 * 설정자: 장치를 만들 때.
	 * 읽는 자: 이 장치에 관한 모든 debug 기록.
	 * 값 범위: 유효한 핸들.
	 * 동기화: debug 계층이 관리한다.
	 * **장치마다 따로 두므로** 한 장치가 기록을 쏟아 내도 다른 장치의 기록이
	 * 밀려나지 않는다. */
	debug_info_t *debug_area;

	/* [한국어] 이 장치의 CCW 장치 — 채널 서브시스템 쪽 표현이다.
	 * 설정자: 장치를 만들 때 인자로 받는다.
	 * 읽는 자: 하드웨어에 I/O 를 띄우는 모든 자리.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 생성 이후 불변.
	 * **이 파일과 arch/s390 을 잇는 지점** 이며, struct ccw_device 의 내용은
	 * 이 트리에서 확인 못 함. */
	struct ccw_device *cdev;

	/* hook for alias management */
	/* [한국어] 별칭 관리용 목록 고리.
	 * 설정자·읽는 자: dasd_alias.c 의 별칭 관리 코드.
	 * 값 범위: 목록에 붙어 있는 동안 유효.
	 * 동기화: 별칭 관리 쪽 잠금이 지킨다. */
	struct list_head alias_list;

	/* default expiration time in s */
	/* [한국어] 요청의 기본 만료 시간(초).
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned long default_expires;
	/* [한국어] 요청의 기본 재시도 횟수.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	unsigned long default_retries;

	/* [한국어] 블록 계층 쪽 시간 초과.
	 * 설정자: sysfs 로 조정할 수 있다.
	 * 읽는 자: 블록 큐를 세울 때.
	 * 값 범위: 시간 값.
	 * 동기화: 없다. */
	unsigned long blk_timeout;

	/* [한국어] 경로 오류 임계값 — 몇 번 이상 오류가 나면 경로를 내릴지.
	 * 설정자: sysfs.
	 * 읽는 자: 경로 오류 처리.
	 * 값 범위: 0 부터 DASD_THRHLD_MAX 까지.
	 * 동기화: 없다. */
	unsigned long path_thrhld;
	/* [한국어] 그 임계값을 재는 시간 구간. 위 값과 짝을 이뤄 "이 구간 안에 이만큼"
	 * 을 판정한다.
	 * 설정자·읽는 자·동기화: 위와 같다.
	 * 값 범위: 0 부터 DASD_INTERVAL_MAX 까지. */
	unsigned long path_interval;

	/* [한국어] 이 장치의 debugfs 디렉터리.
	 * 설정자: 장치를 등록할 때.
	 * 읽는 자: 아래 항목들을 그 밑에 만들 때.
	 * 값 범위: 유효한 dentry, 또는 debugfs 가 없으면 NULL.
	 * 동기화: 생성 이후 불변. */
	struct dentry *debugfs_dentry;
	/* [한국어] 접근 중인 호스트 목록을 보여 주는 debugfs 항목. 위 디시플린의
	 * hosts_print 콜백이 그 내용을 채운다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct dentry *hosts_dentry;
	/* [한국어] 이 장치의 I/O 프로파일. **포인터가 아니라 값으로 품는다** —
	 * 수명이 장치와 같기 때문이다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct dasd_profile profile;
	/* [한국어] 포맷 진행 상황을 기록하는 항목. 역시 값으로 품는다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct dasd_format_entry format_entry;
	/* [한국어] 경로 정보를 담는 sysfs kset. 위 dasd_path 의 kobject 들이 이 아래에
	 * 모인다.
	 * 설정자·읽는 자·동기화: 위와 같다. */
	struct kset *paths_info;
	/* [한국어] 이 장치가 속한 복사 관계.
	 * 설정자: 복사 관계를 설정할 때.
	 * 읽는 자: I/O 를 어느 쪽으로 보낼지 정할 때.
	 * 값 범위: 유효한 포인터, 또는 복사 관계가 없으면 NULL.
	 * 동기화: 복사 관계 구성 경로에서 다뤄진다. */
	struct dasd_copy_relation *copy;
	/* [한국어] 자동 정지(autoquiesce)를 유발할 트리거들의 마스크.
	 * 설정자: sysfs 로 조정한다.
	 * 읽는 자: 오류가 났을 때 자동 정지할지 판단하는 코드.
	 * 값 범위: 위 eer_trigger 값들의 비트 마스크.
	 * 동기화: 없다. */
	unsigned long aq_mask;
	/* [한국어] 자동 정지 판정에 쓰는 시간 초과 누적 횟수.
	 * 설정자·읽는 자: 시간 초과 처리 경로.
	 * 값 범위: 0 이상.
	 * 동기화: 없다. */
	unsigned int aq_timeouts;
};

struct dasd_block {
	/* Block device stuff. */
	/* [한국어] 사용자 공간에 보이는 블록 장치 그 자체.
	 * 설정자: 블록 장치를 등록할 때 만든다.
	 * 읽는 자: 이름·용량을 다루는 자리와 해제 경로.
	 * 값 범위: 유효한 gendisk 포인터.
	 * 동기화: 등록 이후 불변. */
	struct gendisk *gdp;
	/* [한국어] 요청 큐를 지키는 스핀락.
	 * 설정자: 블록 장치를 세울 때 초기화.
	 * 읽는 자: 블록 계층이 요청 큐를 다룰 때.
	 * 값 범위: 스핀락.
	 * 동기화: 아래 queue_lock 과 **이름이 비슷하지만 다른 잠금** 이다 —
	 * 이쪽은 블록 계층 쪽, 아래는 이 드라이버의 CCW 큐 쪽이다. */
	spinlock_t request_queue_lock;
	/* [한국어] blk-mq 태그 집합.
	 * 설정자: 블록 장치를 세울 때 채운다.
	 * 읽는 자: 큐를 만들 때.
	 * 값 범위: 값으로 품으므로 수명이 이 구조체와 같다.
	 * 동기화: 초기화 이후 불변. */
	struct blk_mq_tag_set tag_set;
	/* [한국어] 열려 있는 블록 장치 파일.
	 * 설정자: 장치를 열 때.
	 * 읽는 자: 닫을 때.
	 * 값 범위: 유효한 포인터, 또는 열려 있지 않으면 NULL.
	 * 동기화: 열고 닫는 경로에서 다뤄진다. */
	struct file *bdev_file;
	/* [한국어] 이 장치가 열린 횟수.
	 * 설정자·읽는 자: open/release 경로가 올리고 내린다.
	 * 값 범위: 0 이상. 0 이 아니면 내릴 수 없다.
	 * 동기화: 원자 변수라 잠금이 없다. */
	atomic_t open_count;

	/* [한국어] 볼륨의 크기(블록 수).
	 * 설정자: 볼륨 분석이 채운다.
	 * 읽는 자: 용량을 보고할 때.
	 * 값 범위: 0 이상.
	 * 동기화: 분석 이후 불변. */
	unsigned long blocks;	   /* size of volume in blocks */
	/* [한국어] 블록 하나의 바이트 수.
	 * 설정자: 볼륨 분석이 채운다.
	 * 읽는 자: 주소 계산과 큐 한계 설정.
	 * 값 범위: 512/1024/2048/4096 — 위 dasd_check_blocksize() 가 그것을 강제한다.
	 * 동기화: 분석 이후 불변. */
	unsigned int bp_block;	   /* bytes per block */
	/* [한국어] 블록 번호를 512바이트 섹터 번호로 옮길 때 쓸 시프트 폭.
	 * 설정자: 볼륨 분석이 위 bp_block 에서 계산해 둔다.
	 * 읽는 자: 블록과 섹터를 오가는 모든 계산.
	 * 값 범위: 0~3 (블록 크기 512~4096 에 대응).
	 * 동기화: 분석 이후 불변.
	 * **나눗셈 대신 시프트를 쓰려고 미리 계산해 두는** 값이며, 블록 크기가
	 * 언제나 2의 거듭제곱이라 성립한다. */
	unsigned int s2b_shift;	   /* log2 (bp_block/512) */

	/* [한국어] 이 블록 장치의 바탕이 되는 장치.
	 * 설정자: 블록 장치를 만들 때.
	 * 읽는 자: I/O 를 실제 장치로 내려보내는 모든 자리.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 생성 이후 불변.
	 * **struct dasd_device 의 block 필드와 서로를 가리키는 짝** 이다. */
	struct dasd_device *base;
	/* [한국어] 이 블록 장치에서 만들어진 요청들의 큐.
	 * 설정자·읽는 자: I/O 제출과 완료 경로.
	 * 값 범위: 목록.
	 * 동기화: 아래 queue_lock 이 지킨다.
	 * **struct dasd_device 에도 같은 이름의 큐가 있다** — 이쪽은 블록 요청
	 * 단위, 그쪽은 장치로 나가는 CCW 요청 단위다. */
	struct list_head ccw_queue;
	/* [한국어] 위 ccw_queue 를 지키는 스핀락.
	 * 설정자: 블록 장치를 세울 때 초기화.
	 * 읽는 자: 큐를 다루는 모든 자리.
	 * 값 범위: 스핀락.
	 * 동기화: I/O 완료가 인터럽트 문맥에서 오므로 인터럽트를 막는 판으로 잡는다. */
	spinlock_t queue_lock;

	/* [한국어] 태스클릿이 이미 예약돼 있는지.
	 * 설정자·읽는 자: 태스클릿을 예약하기 전에 확인한다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 원자 변수. 중복 예약을 막는다. */
	atomic_t tasklet_scheduled;
	/* [한국어] 블록 계층 쪽 완료 처리를 미루는 태스클릿.
	 * 설정자: 블록 장치를 세울 때 초기화.
	 * 읽는 자: 완료 경로가 예약한다.
	 * 값 범위: 태스클릿.
	 * 동기화: 위 원자 변수가 지킨다. */
	struct tasklet_struct tasklet;
	/* [한국어] 블록 계층 쪽 시간 감시 타이머.
	 * 설정자·읽는 자: 요청을 띄우고 거둘 때.
	 * 값 범위: 타이머.
	 * 동기화: 타이머 계층이 관리한다. */
	struct timer_list timer;

	/* [한국어] 이 블록 장치의 debugfs 디렉터리.
	 * 설정자: 등록할 때.
	 * 읽는 자: 아래 프로파일 항목을 그 밑에 만들 때.
	 * 값 범위: 유효한 dentry 또는 NULL.
	 * 동기화: 생성 이후 불변. */
	struct dentry *debugfs_dentry;
	/* [한국어] 블록 장치 단위의 I/O 프로파일. 장치 쪽에도 같은 것이 있어,
	 * **둘을 따로 집계한다** — 별칭을 거친 I/O 가 어느 쪽에 잡히는지가
	 * 달라지기 때문이다.
	 * 설정자·읽는 자: 집계와 출력 경로.
	 * 값 범위: 값으로 품는다.
	 * 동기화: 그 안의 잠금이 지킨다. */
	struct dasd_profile profile;

	/* [한국어] 포맷 중인 트랙들의 목록.
	 * 설정자·읽는 자: 포맷 경로가 붙이고 뗀다.
	 * 값 범위: 목록.
	 * 동기화: 아래 format_lock 이 지킨다. */
	struct list_head format_list;
	/* [한국어] 위 목록과 아래 trkcount 를 지키는 스핀락.
	 * 설정자: 블록 장치를 세울 때 초기화.
	 * 읽는 자: 포맷 경로.
	 * 값 범위: 스핀락.
	 * 동기화: 포맷은 프로세스 컨텍스트에서 일어난다. */
	spinlock_t format_lock;
	/* [한국어] 포맷 중인 트랙 수.
	 * 설정자·읽는 자: 포맷 경로.
	 * 값 범위: 0 이상.
	 * 동기화: 원자 변수라 위 잠금 없이도 셀 수 있다. */
	atomic_t trkcount;
};

struct dasd_attention_data {
	/* [한국어] 주의 인터럽트를 낸 장치.
	 * 설정자: 주의 인터럽트를 받았을 때 채운다.
	 * 읽는 자: 지연 처리 코드.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 이 구조체는 처리 하나에 딸린 것이라 공유되지 않는다. */
	struct dasd_device *device;
	/* [한국어] 그 인터럽트가 올라온 경로 마스크(Last Path Used Mask).
	 * 설정자: 위와 같다.
	 * 읽는 자: 어느 경로에서 온 주의인지 가릴 때.
	 * 값 범위: 8비트 경로 마스크.
	 * 동기화: 위와 같다. */
	__u8 lpum;
};

struct dasd_queue {
	/* [한국어] 요청 큐를 지키는 스핀락.
	 * 설정자: 큐를 만들 때 초기화.
	 * 읽는 자: 큐를 다루는 자리.
	 * 값 범위: 스핀락.
	 * 동기화: **구조체에 필드가 이것 하나뿐** 이라, 사실상 잠금 하나를
	 * blk-mq 쪽에 넘기기 위한 껍데기다. */
	spinlock_t lock;
};

/* reasons why device (ccw_device_start) was stopped */
/* [한국어] 접근할 수 없어 정지됐다. */
#define DASD_STOPPED_NOT_ACC 1         /* not accessible */
/* [한국어] 정지(quiesce) 요청으로 정지됐다. */
#define DASD_STOPPED_QUIESCE 2         /* Quiesced */
/* [한국어] 장치가 오래 바쁜(long busy) 상태라 정지됐다.
 * **값이 1, 2, 4 로 비트 자리** 이므로 여러 이유가 동시에 설 수 있고,
 * struct dasd_device 의 stopped 필드가 그 조합을 담는다. */
#define DASD_STOPPED_PENDING 4         /* long busy */
#define DASD_STOPPED_DC_WAIT 8         /* disconnected, wait */
#define DASD_STOPPED_SU      16        /* summary unit check handling */
#define DASD_STOPPED_PPRC    32        /* PPRC swap */
#define DASD_STOPPED_NOSPC   128       /* no space left */

/* per device flags */
#define DASD_FLAG_OFFLINE	3	/* device is in offline processing */
#define DASD_FLAG_EER_SNSS	4	/* A SNSS is required */
#define DASD_FLAG_EER_IN_USE	5	/* A SNSS request is running */
#define DASD_FLAG_DEVICE_RO	6	/* The device itself is read-only. Don't
					 * confuse this with the user specified
					 * read-only feature.
					 */
#define DASD_FLAG_IS_RESERVED	7	/* The device is reserved */
#define DASD_FLAG_LOCK_STOLEN	8	/* The device lock was stolen */
#define DASD_FLAG_SUSPENDED	9	/* The device was suspended */
#define DASD_FLAG_SAFE_OFFLINE	10	/* safe offline processing requested*/
#define DASD_FLAG_SAFE_OFFLINE_RUNNING	11	/* safe offline running */
#define DASD_FLAG_ABORTALL	12	/* Abort all noretry requests */
#define DASD_FLAG_PATH_VERIFY	13	/* Path verification worker running */
#define DASD_FLAG_SUC		14	/* unhandled summary unit check */

#define DASD_SLEEPON_START_TAG	((void *) 1)
#define DASD_SLEEPON_END_TAG	((void *) 2)

void dasd_put_device_wake(struct dasd_device *);

/*
 * return values to be returned from the copy pair swap function
 * 0x00: swap successful
 * 0x01: swap data invalid
 * 0x02: no active device found
 * 0x03: wrong primary specified
 * 0x04: secondary device not found
 * 0x05: swap already running
 */
#define DASD_COPYPAIRSWAP_SUCCESS	0
#define DASD_COPYPAIRSWAP_INVALID	1
#define DASD_COPYPAIRSWAP_NOACTIVE	2
#define DASD_COPYPAIRSWAP_PRIMARY	3
#define DASD_COPYPAIRSWAP_SECONDARY	4
#define DASD_COPYPAIRSWAP_MULTIPLE	5

/* [한국어]
 * dasd_get_device - 장치의 참조 계수를 하나 올린다
 *
 * @device: 대상 장치.
 *
 * 장치를 쓰는 동안 해제되지 않게 붙드는 함수다. 원자적 증가 한 줄이라
 * 잠금이 필요 없다.
 *
 * **반환값이 없다** — 참조를 잡는 데 실패할 수 없다는 뜻이며, 그것이
 * 성립하려면 호출자가 이미 유효한 참조를 하나 쥐고 있어야 한다.
 *
 * 실행 컨텍스트: 어디서든. 원자 연산 하나라 인터럽트 문맥에서도 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   장치를 붙드는 모든 코드 → [이 함수] → atomic_inc()
 */
/*
 * Reference count inliners
 */
static inline void
dasd_get_device(struct dasd_device *device)
{
	atomic_inc(&device->ref_count);
}

/* [한국어]
 * dasd_put_device - 참조 계수를 내리고 0 이 되면 기다리는 쪽을 깨운다
 *
 * @device: 대상 장치.
 *
 * dasd_get_device() 의 짝이다. 내린 결과가 0 이면 더는 이 장치를 쓰는
 * 곳이 없다는 뜻이므로, 해제를 기다리며 잠들어 있는 쪽을 깨운다.
 *
 * **감소와 결과 확인이 한 연산(atomic_dec_return)으로 묶여 있는 것** 이
 * 요점이다. 따로 했다면 두 스레드가 동시에 0 을 보고 둘 다 깨우기를
 * 시도할 수 있다.
 *
 * 여기서 장치를 곧바로 풀지 않고 깨우기만 한다는 점도 중요하다 — 실제
 * 해제는 기다리던 쪽이 하며, 그래서 이 함수는 어떤 문맥에서 불려도
 * 안전하다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥에서도 불릴 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   장치를 놓는 모든 코드 → [이 함수]
 *     → atomic_dec_return() → dasd_put_device_wake()
 */
static inline void
dasd_put_device(struct dasd_device *device)
{
	if (atomic_dec_return(&device->ref_count) == 0)
		dasd_put_device_wake(device);
}

/*
 * The static memory in ccw_mem and erp_mem is managed by a sorted
 * list of free memory chunks.
 */
struct dasd_mchunk
{
	/* [한국어] 빈 청크 목록에 매다는 고리.
	 * 설정자·읽는 자: 아래 청크 할당기 세 함수.
	 * 값 범위: 목록에 붙어 있으면 빈 청크, 떼어져 있으면 쓰이는 중이다.
	 * 동기화: 호출부가 mem_lock 을 잡는다.
	 * **이 고리가 목록에 있는지 없는지가 곧 빈/쓰임 상태** 라, 별도의 표시가
	 * 필요 없다. */
	struct list_head list;
	/* [한국어] 이 청크가 담을 수 있는 바이트 수. **머리 자신의 크기는 빼고 센다** —
	 * init 이 그렇게 초기화하고, alloc/free 의 계산도 모두 그 전제를 따른다.
	 * 설정자·읽는 자: 아래 청크 할당기 세 함수.
	 * 값 범위: 8의 배수. alloc 이 요청을 8바이트로 올림하기 때문이다.
	 * 동기화: 호출부가 mem_lock 을 잡는다. */
	unsigned long size;
/* [한국어] 구조체를 8바이트 경계에 맞춘다. 이것이 있어야 머리 뒤에 오는
 * 사용자 메모리도 8바이트 정렬을 보장받는다 — alloc 이 요청 크기를
 * 8로 올림하는 것과 짝을 이루는 장치다. */
} __attribute__ ((aligned(8)));

/* [한국어]
 * dasd_init_chunklist - 미리 잡아 둔 메모리를 청크 할당기의 초기 상태로 만든다
 *
 * @chunk_list: 초기화할 청크 목록.
 * @mem: 관리할 메모리 덩어리의 시작 주소.
 * @size: 그 덩어리의 전체 바이트 수.
 *
 * struct dasd_device 의 ccw_mem/erp_mem/ese_mem 을 관리하는 **아주 단순한
 * 전용 할당기** 의 초기화다. 일반 커널 할당기를 쓰지 않는 이유는 I/O
 * 경로에서 할당이 실패하면 그 자체가 I/O 실패가 되기 때문이며, 특히
 * 스왑 I/O 가 이 장치로 나가면 순환이 생긴다.
 *
 * 하는 일은 셋이다.
 * 1. 목록을 빈 상태로 초기화한다.
 * 2. 덩어리의 맨 앞을 청크 머리로 삼는다.
 * 3. **그 머리 크기를 뺀 나머지** 를 쓸 수 있는 크기로 기록한다.
 *
 * 3번이 이 할당기의 규약을 드러낸다 — 청크마다 머리가 앞에 붙고,
 * 사용자에게 돌려주는 주소는 그 머리 **뒤** 다. 아래 alloc/free 가
 * 그 규약 위에서 움직인다.
 *
 * 즉 이 함수가 끝나면 덩어리 전체가 '빈 청크 하나' 인 상태가 된다.
 *
 * 실행 컨텍스트: 장치를 만들 때. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. size 가 머리 크기보다 작은 경우를 검사하지 않는다.
 *
 * 호출 체인:
 *   장치 초기화 코드 → [이 함수] → INIT_LIST_HEAD() → list_add()
 */
static inline void
dasd_init_chunklist(struct list_head *chunk_list, void *mem,
		    unsigned long size)
{
	/* [한국어] 쪼개거나 매달 청크를 가리킬 지역 변수. */
	struct dasd_mchunk *chunk;

	/* [한국어] 목록을 빈 상태로 만든다. */
	INIT_LIST_HEAD(chunk_list);
	/* [한국어] 덩어리의 **맨 앞** 을 청크 머리로 삼는다. 별도의 머리 저장소가
	 * 없으므로 관리 정보를 관리 대상 안에 두는 방식이다. */
	chunk = (struct dasd_mchunk *) mem;
	/* [한국어] 쓸 수 있는 크기는 전체에서 머리 크기를 뺀 만큼이다. 이 한 줄이
	 * 이 할당기의 크기 규약을 정한다. */
	chunk->size = size - sizeof(struct dasd_mchunk);
	/* [한국어] 그 하나뿐인 청크를 목록에 올린다. 이제 덩어리 전체가 '빈 청크 하나' 다. */
	list_add(&chunk->list, chunk_list);
}

/* [한국어]
 * dasd_alloc_chunk - 청크 목록에서 요청한 크기를 잘라 준다
 *
 * @chunk_list: 대상 청크 목록.
 * @size: 필요한 바이트 수.
 * @return: 쓸 수 있는 메모리의 주소, 또는 자리가 없으면 NULL.
 *
 * **최초 적합(first fit)** 방식이다. 목록을 앞에서부터 훑어 요청을 담을
 * 수 있는 첫 청크를 쓴다.
 *
 * 먼저 크기를 8바이트 경계로 올림한다. `(size + 7L) & -8L` 이 그 계산이며,
 * 7 을 더한 뒤 아래 세 비트를 지우는 관용적인 올림이다. 정렬을 맞춰야
 * 뒤따르는 청크 머리도 정렬된 자리에 놓인다.
 *
 * 찾은 청크를 처리하는 방식이 둘로 갈린다.
 *   - **남는 자리가 넉넉하면**(요청 + 머리 하나보다 크면) 청크를 쪼갠다.
 *     이때 **뒤쪽을 떼어 준다** — 앞부분을 목록에 그대로 남겨 두고 끝에서
 *     잘라 내므로, 목록의 연결을 고칠 필요가 없다. `endaddr` 에서 크기만큼
 *     물러난 뒤 머리 하나만큼 더 물러나 새 머리 자리를 잡는 계산이 그것이다.
 *   - **남는 자리가 빠듯하면** 쪼개지 않고 청크를 통째로 목록에서 뗀다.
 *     쪼개 봐야 머리도 못 들어갈 자투리만 남기 때문이다.
 *
 * 돌려주는 주소가 `chunk + 1` — 즉 머리 **바로 뒤** 라는 점이 위 init 이
 * 정한 규약과 맞물린다.
 *
 * 실행 컨텍스트: I/O 요청을 만들 때. 호출부가 mem_lock 을 잡고 부른다.
 *
 * 에러 경로: 맞는 청크가 없으면 NULL. 호출자가 요청 생성을 미루거나
 * 실패로 처리한다.
 *
 * 호출 체인:
 *   요청 생성 코드 → [이 함수] → list_for_each_entry() → list_del()
 */
static inline void *
dasd_alloc_chunk(struct list_head *chunk_list, unsigned long size)
{
	/* [한국어] 훑을 청크와, 쪼갤 때 쓸 임시 포인터. */
	struct dasd_mchunk *chunk, *tmp;

	/* [한국어] 요청 크기를 8바이트 경계로 올린다. 7 을 더한 뒤 아래 세 비트를
	 * 지우는 관용적인 올림이며, `-8L` 이 비트로는 ...11111000 이다. */
	size = (size + 7L) & -8L;
	/* [한국어] 목록을 앞에서부터 훑는다 — 최초 적합 방식이다. */
	list_for_each_entry(chunk, chunk_list, list) {
		/* [한국어] 이 청크로는 요청을 담을 수 없다. */
		if (chunk->size < size)
			/* [한국어] 다음 청크를 본다. */
			continue;
		/* [한국어] 담을 수 있는데 **머리 하나가 더 들어갈 만큼 넉넉한지** 본다.
		 * 그만큼 남지 않으면 쪼개 봐야 쓸 수 없는 자투리만 생긴다. */
		if (chunk->size > size + sizeof(struct dasd_mchunk)) {
			/* [한국어] 이 청크가 담당하는 메모리의 끝 주소를 구한다. */
			char *endaddr = (char *) (chunk + 1) + chunk->size;
			/* [한국어] 끝에서 요청 크기만큼 물러나고, 다시 머리 하나만큼 더 물러난 자리를
			 * 새 청크의 머리로 삼는다. `- 1` 이 포인터 산술이라 머리 크기만큼
			 * 빼진다는 점에 주의. */
			tmp = (struct dasd_mchunk *) (endaddr - size) - 1;
			/* [한국어] 떼어 낸 뒤쪽 조각의 크기를 요청 크기로 적는다. */
			tmp->size = size;
			/* [한국어] 앞쪽에 남는 청크의 크기를 줄인다. **떼어 준 크기에 새 머리 크기까지
			 * 더해 빼는** 것이 핵심 — 새로 생긴 머리도 이 청크의 몫에서 나왔기 때문이다. */
			chunk->size -= size + sizeof(struct dasd_mchunk);
			/* [한국어] 이제 돌려줄 것은 떼어 낸 뒤쪽이다. 앞쪽은 목록에 그대로 남으므로
			 * 연결을 고칠 필요가 없다. */
			chunk = tmp;
		/* [한국어] 쪼갤 만큼 넉넉하지 않은 경우다. */
		} else
			/* [한국어] 통째로 쓰므로 목록에서 뗀다. */
			list_del(&chunk->list);
		/* [한국어] 머리 **바로 뒤** 주소를 돌려준다. init 이 정한 규약이며,
		 * free 가 이 주소에서 머리 크기만큼 물러나 머리를 되찾는다. */
		return (void *) (chunk + 1);
	}
	/* [한국어] 맞는 청크를 찾지 못했다. 호출자가 요청 생성을 포기한다. */
	return NULL;
}

/* [한국어]
 * dasd_free_chunk - 청크를 목록에 돌려주며 이웃과 합친다
 *
 * @chunk_list: 대상 청크 목록.
 * @mem: dasd_alloc_chunk() 가 돌려주었던 주소.
 *
 * 돌려받은 주소에서 머리 크기만큼 물러나 청크 머리를 되찾는 것으로
 * 시작한다 — alloc 이 머리 뒤를 돌려주었으므로 그 역산이다.
 *
 * **단편화를 막는 병합이 이 함수의 핵심** 이며, 세 단계로 이뤄진다.
 *
 * 1. **왼쪽 이웃 찾기.** 목록을 훑어 자기보다 주소가 큰 첫 원소 앞에서
 *    멈춘다. 즉 이 목록은 **주소 순으로 정렬돼 있다는 전제** 이며, 마지막
 *    줄의 삽입이 그 정렬을 유지한다.
 * 2. **오른쪽과 합치기.** 왼쪽 이웃의 다음 원소가 자기 끝에 딱 붙어 있으면
 *    그것을 목록에서 떼어 자기 크기에 흡수한다. 이때 사라진 머리의 크기도
 *    함께 더해진다 — 그래서 `+ sizeof(struct dasd_mchunk)` 가 붙는다.
 * 3. **왼쪽과 합치기.** 왼쪽 이웃의 끝이 자기 시작과 맞닿으면 반대로
 *    자기가 그쪽에 흡수된다. 이 경우 **자기는 목록에 들어가지 않으므로
 *    곧바로 반환한다** — 마지막 줄에 닿지 않는 유일한 경로다.
 *
 * 3번에서 합쳐지지 않았을 때만 마지막 줄이 실행돼, 찾아 둔 자리에 자기를
 * 끼워 넣는다. `__list_add` 로 앞뒤를 직접 지정하는 것이 정렬을 지키는
 * 방법이다.
 *
 * 2번과 3번의 순서 덕분에 **양쪽이 모두 인접한 경우 셋이 하나로 합쳐진다** —
 * 먼저 오른쪽을 흡수한 뒤, 그 커진 자기가 다시 왼쪽에 흡수되기 때문이다.
 *
 * 실행 컨텍스트: 요청을 놓을 때. 호출부가 mem_lock 을 잡고 부른다.
 *
 * 에러 경로: 없다. 주소가 이 목록에서 나온 것인지 검사하지 않는다.
 *
 * 호출 체인:
 *   요청 해제 코드 → [이 함수]
 *     → list_for_each() → list_del() → __list_add()
 */
static inline void
dasd_free_chunk(struct list_head *chunk_list, void *mem)
{
	/* [한국어] 되찾을 청크와, 이웃과 합칠 때 쓸 임시 포인터. */
	struct dasd_mchunk *chunk, *tmp;
	/* [한국어] 목록 순회용 커서와, 찾아낸 왼쪽 이웃. */
	struct list_head *p, *left;

	/* [한국어] 돌려받은 주소에서 머리 크기만큼 물러나 청크 머리를 되찾는다 —
	 * alloc 이 머리 뒤를 돌려주었으므로 그 역산이다. */
	chunk = (struct dasd_mchunk *)
		((char *) mem - sizeof(struct dasd_mchunk));
	/* Find out the left neighbour in chunk_list. */
	/* [한국어] 왼쪽 이웃의 초기값을 목록 머리로 둔다. 자기가 맨 앞에 오는 경우
	 * 그대로 남아, 아래 삽입이 목록 맨 앞에 넣게 된다. */
	left = chunk_list;
	/* [한국어] 목록을 훑어 자기가 들어갈 자리를 찾는다. */
	list_for_each(p, chunk_list) {
		/* [한국어] 자기보다 주소가 큰 첫 원소를 만나면 멈춘다. **목록이 주소 순으로
		 * 정렬돼 있다는 전제** 이며, 아래 삽입이 그 정렬을 지킨다. */
		if (list_entry(p, struct dasd_mchunk, list) > chunk)
			/* [한국어] 여기가 자기 자리다. */
			break;
		/* [한국어] 아직이면 이것을 왼쪽 이웃 후보로 두고 계속 간다. */
		left = p;
	}
	/* Try to merge with right neighbour = next element from left. */
	/* [한국어] 왼쪽 이웃의 다음이 목록 머리가 아니라면 — 즉 오른쪽에 실제 원소가
	 * 있다면 합쳐 볼 수 있다. */
	if (left->next != chunk_list) {
		/* [한국어] 그 오른쪽 이웃을 꺼낸다. */
		tmp = list_entry(left->next, struct dasd_mchunk, list);
		/* [한국어] 자기 메모리의 끝과 그 이웃의 시작이 정확히 맞닿는지 본다.
		 * 맞닿아야만 하나의 연속 영역이 된다. */
		if ((char *) (chunk + 1) + chunk->size == (char *) tmp) {
			/* [한국어] 이웃을 목록에서 뗀다. 이제 자기에게 흡수된다. */
			list_del(&tmp->list);
			/* [한국어] 이웃의 크기에 **사라진 머리 크기까지 더해** 흡수한다 — 그 머리가
			 * 있던 자리도 이제 쓸 수 있는 공간이기 때문이다. */
			chunk->size += tmp->size + sizeof(struct dasd_mchunk);
		}
	}
	/* Try to merge with left neighbour. */
	/* [한국어] 왼쪽에 실제 원소가 있는지 본다. */
	if (left != chunk_list) {
		/* [한국어] 그 왼쪽 이웃을 꺼낸다. */
		tmp = list_entry(left, struct dasd_mchunk, list);
		/* [한국어] 이번에는 이웃의 끝과 자기 시작이 맞닿는지 본다. */
		if ((char *) (tmp + 1) + tmp->size == (char *) chunk) {
			/* [한국어] 이쪽은 반대로 **자기가 흡수된다.** 역시 자기 머리 크기까지 더해진다. */
			tmp->size += chunk->size + sizeof(struct dasd_mchunk);
			/* [한국어] **여기서 곧바로 반환한다** — 자기는 목록에 들어가지 않으므로
			 * 아래 삽입을 건너뛰어야 한다. 이 함수에서 마지막 줄에 닿지 않는
			 * 유일한 경로다. */
			return;
		}
	}
	/* [한국어] 합쳐지지 않았으므로 찾아 둔 자리에 끼워 넣는다. 앞뒤를 직접
	 * 지정하는 판을 써서 주소 순 정렬을 지킨다. */
	__list_add(&chunk->list, left, left->next);
}

/* [한국어]
 * dasd_check_blocksize - 블록 크기가 지원 범위 안의 2의 거듭제곱인지 확인한다
 *
 * @bsize: 검사할 블록 크기(바이트).
 * @return: 0 = 유효, -EMEDIUMTYPE = 유효하지 않음.
 *
 * 위 상류 주석이 밝히듯 512, 1024, 2048, 4096 넷만 허용한다. 그것을 값
 * 목록으로 나열하지 않고 **범위 검사와 2의 거듭제곱 검사의 조합** 으로
 * 표현했다 — 세 조건이 합쳐지면 정확히 그 넷만 남는다.
 *
 * 반환 오류가 -EINVAL 이 아니라 **-EMEDIUMTYPE** 인 것이 눈에 띈다.
 * 인자가 잘못됐다기보다 '이 매체를 다룰 수 없다' 는 뜻이며, 위 디시플린
 * vtable 의 do_analysis 가 같은 값을 같은 의미로 쓴다.
 *
 * 실행 컨텍스트: 볼륨 분석과 포맷 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 위 셋 중 하나라도 어긋나면 -EMEDIUMTYPE.
 *
 * 호출 체인:
 *   디시플린의 볼륨 분석·포맷 코드 → [이 함수] → is_power_of_2()
 */
/*
 * Check if bsize is in { 512, 1024, 2048, 4096 }
 */
static inline int
dasd_check_blocksize(int bsize)
{
	/* [한국어] 세 조건을 한 줄에 묶었다 — 512 이상, 4096 이하, 그리고 2의
	 * 거듭제곱이다. 셋이 합쳐지면 정확히 512/1024/2048/4096 넷만 남는다. */
	if (bsize < 512 || bsize > 4096 || !is_power_of_2(bsize))
		/* [한국어] -EMEDIUMTYPE 을 쓴다. '인자가 잘못됨' 이 아니라 '이 매체를 다룰 수
		 * 없음' 이라는 뜻이며, 디시플린의 do_analysis 가 같은 값을 같은 의미로 쓴다. */
		return -EMEDIUMTYPE;
	/* [한국어] 네 값 중 하나이므로 유효하다. */
	return 0;
}

/* [한국어]
 * dasd_get_callback_data - 오류 복구 요청을 거슬러 올라가 원래 요청의 콜백 자료를 찾는다
 *
 * @cqr: 요청. 오류 복구로 만들어진 것일 수 있다.
 * @return: 그 사슬의 맨 아래에 있는 원래 요청의 콜백 자료.
 *
 * 위 상류 주석이 배경을 밝힌다 — 오류 복구(ERP)는 실패한 요청 **위에**
 * 새 요청을 쌓아 올리는 방식이라, 요청이 여러 겹이 될 수 있다.
 *
 * 그래서 refers 포인터를 따라 더는 가리키는 것이 없을 때까지 거슬러
 * 올라간다. 그 끝이 사용자가 원래 낸 요청이며, 완료를 알릴 콜백 자료도
 * 거기 있다.
 *
 * **루프인 이유** 는 복구가 여러 단계로 이어질 수 있어 겹이 둘 이상일 수
 * 있기 때문이다. 한 단계만 거슬러서는 부족하다.
 *
 * 인자를 그대로 재사용해 거슬러 오르므로 별도의 지역 변수가 없다.
 *
 * 실행 컨텍스트: 요청 완료 처리. 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다. 사슬에 고리가 생기면 무한 루프가 되지만, 복구 요청이
 * 자기 아래쪽만 가리키므로 그런 일은 생기지 않는다는 전제다.
 *
 * 호출 체인:
 *   요청 완료 코드 → [이 함수]
 */
/*
 * return the callback data of the original request in case there are
 * ERP requests build on top of it
 */
static inline void *dasd_get_callback_data(struct dasd_ccw_req *cqr)
{
	/* [한국어] 가리키는 것이 없을 때까지 거슬러 오른다. 복구가 여러 겹이면
	 * 여러 번 돈다. */
	while (cqr->refers)
		cqr = cqr->refers;

	/* [한국어] 사슬 맨 아래, 즉 사용자가 원래 낸 요청의 콜백 자료를 돌려준다. */
	return cqr->callback_data;
}

/* externals in dasd.c */
/* [한국어] 프로파일 집계를 끈 상태. */
#define DASD_PROFILE_OFF	 0
/* [한국어] 장치별·블록별 프로파일까지 모두 켠 상태. */
#define DASD_PROFILE_ON 	 1
/* [한국어] 전역 프로파일만 켠 상태. 장치가 많을 때 장치별 집계 비용을 피하면서
 * 전체 경향만 보고 싶을 때 쓴다. */
#define DASD_PROFILE_GLOBAL_ONLY 2

/* [한국어] DASD 공통 코드의 debug 영역. 장치별 영역과 별개로, 장치에 매이지
 * 않는 기록이 여기 남는다. */
extern debug_info_t *dasd_debug_area;
/* [한국어] 모든 장치의 I/O 를 합쳐 세는 전역 프로파일. */
extern struct dasd_profile dasd_global_profile;
/* [한국어] 위 DASD_PROFILE_ 계열 중 지금 어느 수준인지. */
extern unsigned int dasd_global_profile_level;
/* [한국어] 블록 장치의 open/release/ioctl 진입점 표. */
extern const struct block_device_operations dasd_device_operations;
/* [한국어] blk-mq 큐 콜백 표. 요청이 이 표를 거쳐 드라이버로 들어온다. */
extern struct blk_mq_ops dasd_mq_ops;

/* [한국어] 요청에 딸린 페이지를 잡는 전용 캐시. 자주 같은 크기를 잡고 놓으므로
 * 슬랩 캐시를 따로 두는 편이 낫다. */
extern struct kmem_cache *dasd_page_cache;

/* [한국어] 일반 요청을 장치의 미리 잡아 둔 메모리에서 만든다. s 는 small 또는
 * static 을 뜻하는 것으로 보이며, 앞의 청크 할당기를 쓴다. */
struct dasd_ccw_req *
dasd_smalloc_request(int, int, int, struct dasd_device *, struct dasd_ccw_req *);
/* [한국어] 포맷용 요청을 만든다. f 가 format 을 가리킨다. */
struct dasd_ccw_req *dasd_fmalloc_request(int, int, int, struct dasd_device *);
/* [한국어] 위 smalloc 판으로 만든 요청을 푼다. */
void dasd_sfree_request(struct dasd_ccw_req *, struct dasd_device *);
/* [한국어] 위 fmalloc 판으로 만든 요청을 푼다. 짝을 맞춰 불러야 한다. */
void dasd_ffree_request(struct dasd_ccw_req *, struct dasd_device *);
/* [한국어] 요청이 끝나면 기다리는 쪽을 깨우는 공용 콜백. 아래 sleep_on 계열이
 * 이것을 걸어 두고 잠든다. */
void dasd_wakeup_cb(struct dasd_ccw_req *, void *);

/* [한국어] 장치 구조체를 할당한다. */
struct dasd_device *dasd_alloc_device(void);
/* [한국어] 그 짝. 참조 계수가 0 이 된 뒤에 불린다. */
void dasd_free_device(struct dasd_device *);

/* [한국어] 블록 장치 구조체를 할당한다. 장치와 따로 잡는다. */
struct dasd_block *dasd_alloc_block(void);
/* [한국어] 그 짝. */
void dasd_free_block(struct dasd_block *);

/* [한국어] blk-mq 의 시간 초과 콜백. 요청이 제때 끝나지 않았을 때 어떻게 할지를
 * 블록 계층에 알려 준다. */
enum blk_eh_timer_return dasd_times_out(struct request *req);

/* [한국어] 장치를 사용 가능한 상태로 올린다. */
void dasd_enable_device(struct dasd_device *);
/* [한국어] 장치의 목표 상태를 정한다. 상태 기계가 그쪽으로 한 단계씩 옮긴다. */
void dasd_set_target_state(struct dasd_device *, int);
/* [한국어] 미뤄 둔 장치 작업을 작업 큐에 올린다. */
void dasd_kick_device(struct dasd_device *);
/* [한국어] 장치를 다시 읽어 들이게 한다. */
void dasd_reload_device(struct dasd_device *);
/* [한국어] 요청들을 다시 큐에 넣는 작업을 예약한다. */
void dasd_schedule_requeue(struct dasd_device *);

/* [한국어] 요청을 큐 **앞** 에 넣는다. 오류 복구 요청처럼 먼저 처리해야 하는
 * 것에 쓴다. */
void dasd_add_request_head(struct dasd_ccw_req *);
/* [한국어] 요청을 큐 뒤에 넣는다. 보통의 경로다. */
void dasd_add_request_tail(struct dasd_ccw_req *);
/* [한국어] 요청을 하드웨어에 띄운다. */
int  dasd_start_IO(struct dasd_ccw_req *);
/* [한국어] 띄운 요청을 취소한다. */
int  dasd_term_IO(struct dasd_ccw_req *);
/* [한국어] 장치 태스클릿을 예약한다. bh 는 bottom half 를 가리킨다. */
void dasd_schedule_device_bh(struct dasd_device *);
/* [한국어] 블록 장치 태스클릿을 예약한다. */
void dasd_schedule_block_bh(struct dasd_block *);
/* [한국어] 요청을 내고 끝날 때까지 잠들어 기다린다. */
int  dasd_sleep_on(struct dasd_ccw_req *);
/* [한국어] 요청 여러 개를 한꺼번에 내고 모두 끝날 때까지 기다린다. */
int  dasd_sleep_on_queue(struct list_head *);
/* [한국어] 기다리되 큐 앞에 넣어 곧바로 처리되게 한다. 이름의 철자가
 * immediately 가 아니라 immediatly 인 점에 주의 — 상류의 표기 그대로다. */
int  dasd_sleep_on_immediatly(struct dasd_ccw_req *);
/* [한국어] 위 큐 판의 인터럽트 가능(시그널로 깨울 수 있는) 형태. */
int  dasd_sleep_on_queue_interruptible(struct list_head *);
/* [한국어] 위 단일 요청 판의 인터럽트 가능 형태. */
int  dasd_sleep_on_interruptible(struct dasd_ccw_req *);
/* [한국어] 장치 타이머를 건다. */
void dasd_device_set_timer(struct dasd_device *, int);
/* [한국어] 장치 타이머를 거둔다. */
void dasd_device_clear_timer(struct dasd_device *);
/* [한국어] 블록 장치 타이머를 건다. */
void dasd_block_set_timer(struct dasd_block *, int);
/* [한국어] 블록 장치 타이머를 거둔다. */
void dasd_block_clear_timer(struct dasd_block *);
/* [한국어] 요청 하나를 취소한다. */
int  dasd_cancel_req(struct dasd_ccw_req *);
/* [한국어] 장치 큐에 쌓인 요청을 모두 비운다. */
int dasd_flush_device_queue(struct dasd_device *);
/* [한국어] CCW 버스가 장치를 찾았을 때의 진입점. generic 이 붙은 아래 함수들이
 * 디시플린과 무관한 공통 처리를 맡는다. */
int dasd_generic_probe(struct ccw_device *);
/* [한국어] 장치에서 디시플린을 떼고 참조를 놓는다. */
void dasd_generic_free_discipline(struct dasd_device *);
/* [한국어] CCW 버스에서 장치가 떨어질 때의 진입점. */
void dasd_generic_remove (struct ccw_device *cdev);
/* [한국어] 장치를 온라인으로 올린다. 쓸 디시플린을 함께 받는다. */
int dasd_generic_set_online(struct ccw_device *, struct dasd_discipline *);
/* [한국어] 그 짝. 장치를 오프라인으로 내린다. */
int dasd_generic_set_offline (struct ccw_device *cdev);
/* [한국어] CCW 버스의 알림을 받는다. */
int dasd_generic_notify(struct ccw_device *, int);
/* [한국어] 마지막 경로가 사라졌을 때 부른다. */
int dasd_generic_last_path_gone(struct dasd_device *);
/* [한국어] 경로가 다시 살아났을 때 부른다. */
int dasd_generic_path_operational(struct dasd_device *);
/* [한국어] 시스템이 내려갈 때 부른다. */
void dasd_generic_shutdown(struct ccw_device *);

/* [한국어] 장치 상태 변화를 공통 코드가 처리한다. */
void dasd_generic_handle_state_change(struct dasd_device *);
/* [한국어] 단위 검사(unit check)를 처리하고 무엇을 할지 돌려준다.
 * enum uc_todo 의 값 목록은 arch/s390 소관이라 이 트리에서 확인 못 함. */
enum uc_todo dasd_generic_uc_handler(struct ccw_device *, struct irb *);
/* [한국어] 경로 이벤트를 처리한다. int 배열로 경로별 정보를 받는다. */
void dasd_generic_path_event(struct ccw_device *, int *);
/* [한국어] 경로 하나를 검증한다. 위 TBV 표시가 선 경로가 대상이다. */
int dasd_generic_verify_path(struct dasd_device *, __u8);
/* [한국어] 익스텐트 풀이 말랐을 때 부른다. ESE 볼륨에서만 의미가 있다. */
void dasd_generic_space_exhaust(struct dasd_device *, struct dasd_ccw_req *);
/* [한국어] 그 공간이 다시 생겼을 때 부른다. */
void dasd_generic_space_avail(struct dasd_device *);

/* [한국어] 장치의 모든 요청을 다시 큐에 넣는다. 경로가 전부 끊겼다 돌아온
 * 뒤처럼 이미 낸 요청을 되살려야 할 때 쓴다. */
int dasd_generic_requeue_all_requests(struct dasd_device *);

/* [한국어] 장치 특성을 읽는다. 디시플린이 장치를 식별할 때 쓰는 기본 질의다. */
int dasd_generic_read_dev_chars(struct dasd_device *, int, void *, int);
/* [한국어] 인터럽트 응답 블록에서 센스 데이터 부분을 꺼낸다. */
char *dasd_get_sense(struct irb *);

/* [한국어] 정지 이유 비트를 세운다. 위 DASD_STOPPED_ 계열이 그 값이다. */
void dasd_device_set_stop_bits(struct dasd_device *, int);
/* [한국어] 그 비트를 거둔다. **모든 비트가 사라져야** 장치가 다시 움직인다. */
void dasd_device_remove_stop_bits(struct dasd_device *, int);

/* [한국어] 이 장치가 읽기 전용인지 묻는다. */
int dasd_device_is_ro(struct dasd_device *);

/* [한국어] 프로파일 집계를 0 으로 되돌린다. */
void dasd_profile_reset(struct dasd_profile *);
/* [한국어] 프로파일 집계를 켠다. 자료 구조를 할당한다. */
int dasd_profile_on(struct dasd_profile *);
/* [한국어] 끈다. 자료 구조를 풀고 포인터를 NULL 로 둔다. */
void dasd_profile_off(struct dasd_profile *);
/* [한국어] 사용자 공간에서 문자열을 복사해 온다. sysfs 쓰기 처리에 쓴다. */
char *dasd_get_user_string(const char __user *, size_t);

/* externals in dasd_devmap.c */
/* [한국어] 지금까지 배분된 장치 색인의 최댓값. */
extern int dasd_max_devindex;
/* [한국어] 탐색만 하고 실제로 붙이지는 않는 모드인지. */
extern int dasd_probeonly;
/* [한국어] 장치를 자동으로 찾아 붙일지. */
extern int dasd_autodetect;
/* [한국어] PAV(별칭)를 쓰지 않을지. 문제가 있을 때 끄는 안전판이다. */
extern int dasd_nopav;
/* [한국어] FCX(전송 모드)를 쓰지 않을지. 역시 안전판이다. */
extern int dasd_nofcx;

/* [한국어] devmap 을 초기화한다. */
int dasd_devmap_init(void);
/* [한국어] 그 짝. */
void dasd_devmap_exit(void);

/* [한국어] CCW 장치에 대응하는 DASD 장치를 만든다. */
struct dasd_device *dasd_create_device(struct ccw_device *);
/* [한국어] 그 짝. */
void dasd_delete_device(struct dasd_device *);

/* [한국어] 이 장치의 기능 비트 하나를 읽는다. */
int dasd_get_feature(struct ccw_device *, int);
/* [한국어] 그 비트를 정한다. */
int dasd_set_feature(struct ccw_device *, int, int);

/* [한국어] 장치의 sysfs 속성 묶음. */
extern const struct attribute_group *dasd_dev_groups[];
/* [한국어] 경로 하나의 sysfs 표현을 만든다. */
void dasd_path_create_kobj(struct dasd_device *, int);
/* [한국어] 여덟 경로 전부의 sysfs 표현을 만든다. */
void dasd_path_create_kobjects(struct dasd_device *);
/* [한국어] 그 짝. 모두 거둔다. */
void dasd_path_remove_kobjects(struct dasd_device *);

/* [한국어] CCW 장치에서 DASD 장치를 되찾는다. **참조를 하나 올려 돌려주므로**
 * 쓰고 나서 dasd_put_device() 를 불러야 한다. */
struct dasd_device *dasd_device_from_cdev(struct ccw_device *);
/* [한국어] 위와 같되 이미 잠금을 쥔 채로 부르는 판이다. */
struct dasd_device *dasd_device_from_cdev_locked(struct ccw_device *);
/* [한국어] 장치 색인 번호로 장치를 찾는다. 역시 참조를 올려 돌려준다. */
struct dasd_device *dasd_device_from_devindex(int);

/* [한국어] gendisk 에서 장치를 되찾을 수 있도록 연결을 심는다. */
void dasd_add_link_to_gendisk(struct gendisk *, struct dasd_device *);
/* [한국어] 그 연결을 따라 장치를 되찾는다. */
struct dasd_device *dasd_device_from_gendisk(struct gendisk *);

/* [한국어] 커널 부팅 매개변수의 dasd= 항목을 해석한다. __init 이 뒤에 붙는
 * 드문 표기이며, 초기화가 끝나면 이 코드는 버려진다. */
int dasd_parse(void) __init;
/* [한국어] 이 버스 ID 가 설정에 등장한 것인지 묻는다. */
int dasd_busid_known(const char *);

/* externals in dasd_gendisk.c */
/* [한국어] gendisk 계층을 초기화한다. 주번호를 잡는 일이 여기 든다. */
int  dasd_gendisk_init(void);
/* [한국어] 그 짝. */
void dasd_gendisk_exit(void);
/* [한국어] 이 블록 장치의 gendisk 를 만든다. */
int dasd_gendisk_alloc(struct dasd_block *);
/* [한국어] 그 짝. */
void dasd_gendisk_free(struct dasd_block *);
/* [한국어] 파티션 표를 읽어 파티션을 만든다. */
int dasd_scan_partitions(struct dasd_block *);
/* [한국어] 그 파티션들을 없앤다. */
void dasd_destroy_partitions(struct dasd_block *);

/* externals in dasd_ioctl.c */
/* [한국어] DASD 고유 ioctl 진입점. 공통 처리 뒤 디시플린 콜백으로 넘긴다. */
int dasd_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned int cmd,
		unsigned long arg);
/* [한국어] 블록 장치를 읽기 전용으로 바꾼다. */
int dasd_set_read_only(struct block_device *bdev, bool ro);

/* externals in dasd_proc.c */
/* [한국어] procfs 항목을 만든다. */
int dasd_proc_init(void);
/* [한국어] 그 짝. */
void dasd_proc_exit(void);

/* externals in dasd_erp.c */
/* [한국어] 오류 복구의 기본 동작. 디시플린이 자기 것을 두지 않으면 이것이 쓰인다. */
struct dasd_ccw_req *dasd_default_erp_action(struct dasd_ccw_req *);
/* [한국어] 그 뒷정리의 기본 동작. */
struct dasd_ccw_req *dasd_default_erp_postaction(struct dasd_ccw_req *);
/* [한국어] 복구용 요청을 만든다. **장치의 erp_mem 풀에서 잡으므로** 일반 요청
 * 풀이 말라도 복구는 진행된다. */
struct dasd_ccw_req *dasd_alloc_erp_request(unsigned int, int, int,
					    struct dasd_device *);
/* [한국어] 그 짝. */
void dasd_free_erp_request(struct dasd_ccw_req *, struct dasd_device *);
/* [한국어] 센스 데이터를 콘솔 로그에 남긴다. */
void dasd_log_sense(struct dasd_ccw_req *, struct irb *);
/* [한국어] 같은 것을 debug 기능 쪽에 남긴다. 훨씬 자주 불려도 부담이 적다. */
void dasd_log_sense_dbf(struct dasd_ccw_req *cqr, struct irb *irb);

/* externals in dasd_3990_erp.c */
/* [한국어] 3990 제어 장치 계열의 오류 복구. DASD 오류 처리의 대부분이 이
 * 파일에 있으며, 규격이 정한 센스 바이트 해석이 방대하다. */
struct dasd_ccw_req *dasd_3990_erp_action(struct dasd_ccw_req *);
/* [한국어] SIM(Service Information Message)을 처리한다. 하드웨어가 서비스가
 * 필요하다고 알리는 메시지다. */
void dasd_3990_erp_handle_sim(struct dasd_device *, char *);

/* externals in dasd_eer.c */
#ifdef CONFIG_DASD_EER
/* [한국어] EER 계층을 초기화한다. 아래 #else 가 꺼진 빌드용 대체 정의를 준다. */
int dasd_eer_init(void);
/* [한국어] 그 짝. */
void dasd_eer_exit(void);
/* [한국어] 이 장치에서 EER 을 켠다. 미리 잡아 둘 요청을 할당한다. */
int dasd_eer_enable(struct dasd_device *);
/* [한국어] 그 짝. 요청을 풀고 포인터를 NULL 로 둔다. */
void dasd_eer_disable(struct dasd_device *);
/* [한국어] 오류 사건을 사용자 공간으로 내보낸다. */
void dasd_eer_write(struct dasd_device *, struct dasd_ccw_req *cqr,
		    unsigned int id);
/* [한국어] SNSS(Sense Subsystem Status)를 요청한다. 제어 장치의 상태를
 * 자세히 받아 오는 질의다. */
void dasd_eer_snss(struct dasd_device *);

/* [한국어]
 * dasd_eer_enabled - 이 장치에서 확장 오류 보고가 켜져 있는지 묻는다
 *
 * @device: 대상 장치.
 * @return: 0 이 아니면 켜져 있다.
 *
 * **미리 잡아 둔 요청의 존재 여부** 로 판단한다. 별도의 켜짐 플래그를 두지
 * 않고, EER 을 켤 때 할당하는 eer_cqr 이 NULL 인지만 본다.
 *
 * 그렇게 해도 되는 이유는 그 요청이 EER 전용이라 다른 목적으로 잡히는
 * 일이 없기 때문이다. 상태를 두 곳에 두지 않으니 어긋날 여지도 없다.
 *
 * 이 함수는 CONFIG_DASD_ERR 이 켜졌을 때의 판이며, 꺼진 빌드에서는 바로
 * 아래 #else 가 같은 이름을 언제나 0 인 매크로로 정의한다. 즉 호출부는
 * 설정에 관계없이 같은 이름을 쓸 수 있다.
 *
 * 실행 컨텍스트: 오류 처리 경로. 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   오류 처리 코드 → [이 함수]
 */
static inline int dasd_eer_enabled(struct dasd_device *device)
{
	return device->eer_cqr != NULL;
}
#else
/* [한국어] 설정이 꺼진 빌드용 대체 정의. **성공(0)을 돌려주는 것** 이 요점 —
 * EER 이 없다고 초기화가 실패해서는 안 되기 때문이다. */
#define dasd_eer_init()		(0)
/* [한국어] 아무것도 하지 않는다. do-while(0) 로 감싸 if 문에서도 안전하다. */
#define dasd_eer_exit()		do { } while (0)
/* [한국어] 역시 성공을 돌려준다. */
#define dasd_eer_enable(d)	(0)
/* [한국어] 아무것도 하지 않는다. */
#define dasd_eer_disable(d)	do { } while (0)
/* [한국어] 아무것도 하지 않는다. */
#define dasd_eer_write(d,c,i)	do { } while (0)
/* [한국어] 아무것도 하지 않는다. */
#define dasd_eer_snss(d)	do { } while (0)
/* [한국어] 언제나 0 — 즉 '꺼져 있음' 이다. 호출부는 설정에 관계없이 같은
 * 이름을 쓸 수 있다. */
#define dasd_eer_enabled(d)	(0)
/* [한국어] [상류 코드 관찰] 이 #endif 의 주석이 **CONFIG_DASD_ERR** 인데,
 * 정작 위 #ifdef 가 검사하는 것은 CONFIG_DASD_EER 이다. 주석의 철자가
 * 한 글자 빠져 있으며, 컴파일에는 영향이 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#endif	/* CONFIG_DASD_ERR */


/* DASD path handling functions */

/* [한국어]
 * dasd_path_is_operational - 이 경로에 OPERATIONAL 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 쓸 수 있는 경로다.
 *
 * 경로별 플래그를 직접 읽는다. **device->opm 마스크를 보지 않는다** 는
 * 점에 주의 — 둘은 dasd_path_operational()/clear_oper() 가 함께 갱신하므로
 * 평소에는 같은 답을 주지만, 이 함수가 보는 것은 플래그 쪽이다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드.
 *
 * 에러 경로: 없다. chp 범위를 검사하지 않는다.
 *
 * 호출 체인:
 *   경로 처리 코드 → [이 함수] → test_bit()
 */
/*
 * helper functions to modify bit masks for a given channel path for a device
 */
static inline int dasd_path_is_operational(struct dasd_device *device, int chp)
{
	/* [한국어] 경로별 플래그를 직접 읽는다. device->opm 마스크가 아니라 이쪽이
	 * 이 함수가 보는 값이다. */
	return test_bit(DASD_PATH_OPERATIONAL, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_need_verify - 이 경로를 다시 검증해야 하는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 검증이 필요하다.
 *
 * TBV(To Be Verified) 표시를 읽는다. 경로가 새로 생겼거나 상태가
 * 의심스러울 때 세워지며, 검증이 끝나면 지워진다.
 *
 * 실행 컨텍스트: 경로 검증 코드.
 *
 * 에러 경로: 없다. chp 범위를 검사하지 않는다.
 *
 * 호출 체인:
 *   경로 검증 코드 → [이 함수] → test_bit()
 */
static inline int dasd_path_need_verify(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_TBV, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_verify - 이 경로에 TBV 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * TBV(To Be Verified)는 이 경로를 다시 검증해야 한다는 표시다. 경로가 새로
 * 생겼거나 상태가 의심스러울 때 세워 두면, 검증 경로가 그것을 보고 처리한다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_verify(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_TBV, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_verify - 이 경로의 TBV 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 검증이 끝났으므로 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_verify(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_TBV, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_all_verify - 여덟 경로 모두의 TBV 표시를 지운다
 *
 * @device: 대상 장치.
 *
 * 모든 경로의 검증 대기 표시를 한 번에 거둔다. 검증을 처음부터 다시
 * 시작하거나 포기할 때 쓴다.
 *
 * 경로 여덟 개를 도는 단순 루프이며, 상한 8 이 코드에 직접 적혀 있다 —
 * struct dasd_device 의 path 배열 크기와 같아야 하는 값인데 상수로
 * 묶여 있지는 않다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_all_verify(struct dasd_device *device)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 경로마다 검증 대기 표시를 지운다. */
		dasd_path_clear_verify(device, chp);
}

/* [한국어]
 * dasd_path_fcsec - 이 경로에 FCSEC 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로의 FC 종단 보안 정보를 갱신해야 한다는 표시다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_fcsec(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_FCSEC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_fcsec - 이 경로의 FCSEC 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 보안 정보 갱신이 끝났으므로 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_fcsec(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_FCSEC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_need_fcsec - 이 경로에 FCSEC 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * 보안 정보를 갱신해야 하는 경로인지 확인한다.
 *
 * 읽기는 test_bit 이라 이 계열 전부가 같은 방식이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_need_fcsec(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_FCSEC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_all_fcsec - 여덟 경로 모두의 FCSEC 표시를 지운다
 *
 * @device: 대상 장치.
 *
 * 모든 경로의 보안 갱신 대기 표시를 한 번에 거둔다.
 *
 * 경로 여덟 개를 도는 단순 루프이며, 상한 8 이 코드에 직접 적혀 있다 —
 * struct dasd_device 의 path 배열 크기와 같아야 하는 값인데 상수로
 * 묶여 있지는 않다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_all_fcsec(struct dasd_device *device)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 경로마다 보안 갱신 대기 표시를 지운다. */
		dasd_path_clear_fcsec(device, chp);
}

/* [한국어]
 * dasd_path_operational - 이 경로에 OPERATIONAL 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로를 쓸 수 있게 되었음을 표시한다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * [이 함수의 특징] 플래그만 세우지 않고 **device->opm 마스크도 함께 고친다.**
 * 그 마스크가 I/O 를 띄울 때 실제로 쓰이는 값이라, 둘이 어긋나면 쓸 수 없는
 * 경로로 요청이 나간다. 비트 자리가 `0x80 >> chp` 인 것에 주의 — 경로 0 이
 * 최상위 비트이며, 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_operational(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_OPERATIONAL, &device->path[chp].flags);
	device->opm |= (0x80 >> chp);
}

/* [한국어]
 * dasd_path_nonpreferred - 이 경로에 NPP 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로가 비선호 경로임을 표시한다. 쓸 수는 있지만 선호 경로가 있으면
 * 그쪽을 먼저 쓴다는 뜻이다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_nonpreferred(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_NPP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_nonpreferred - 이 경로에 NPP 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * 비선호 경로인지 확인한다.
 *
 * 읽기는 test_bit 이라 이 계열 전부가 같은 방식이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_nonpreferred(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_NPP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_nonpreferred - 이 경로의 NPP 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 비선호 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_nonpreferred(struct dasd_device *device,
						int chp)
{
	__clear_bit(DASD_PATH_NPP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_preferred - 이 경로에 PP 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로가 선호 경로임을 표시한다. 위 비선호와 짝을 이루며, 둘 다 서지
 * 않은 경로는 어느 쪽도 아닌 보통 경로다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_preferred(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_PP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_preferred - 이 경로에 PP 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * 선호 경로인지 확인한다.
 *
 * 읽기는 test_bit 이라 이 계열 전부가 같은 방식이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_preferred(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_PP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_preferred - 이 경로의 PP 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 선호 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_preferred(struct dasd_device *device,
					     int chp)
{
	__clear_bit(DASD_PATH_PP, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_oper - 이 경로의 OPERATIONAL 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로를 더는 쓸 수 없음을 표시한다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * [이 함수의 특징] 플래그만 세우지 않고 **device->opm 마스크도 함께 고친다(여기서는 비트를 지운다).**
 * 그 마스크가 I/O 를 띄울 때 실제로 쓰이는 값이라, 둘이 어긋나면 쓸 수 없는
 * 경로로 요청이 나간다. 비트 자리가 `0x80 >> chp` 인 것에 주의 — 경로 0 이
 * 최상위 비트이며, 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_oper(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_OPERATIONAL, &device->path[chp].flags);
	device->opm &= ~(0x80 >> chp);
}

/* [한국어]
 * dasd_path_clear_cable - 이 경로의 MISCABLED 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 배선 오류 표시를 거둔다.
 * [상류 코드 관찰] 이 플래그는 지우는 판과 아래의 마스크 적용 판은 있는데,
 * 경로 하나에 표시를 세우는 dasd_path_ 접근자가 이 헤더에 없다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_cable(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_MISCABLED, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_cuir - 이 경로에 CUIR 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * CUIR(Control Unit Initiated Reconfiguration)로 인해 이 경로가 잠겼음을
 * 표시한다. 제어 장치 쪽에서 구성을 바꾸려고 경로를 거둬 갈 때 쓴다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_cuir(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_CUIR, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_cuir - 이 경로에 CUIR 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * CUIR 로 잠긴 경로인지 확인한다.
 *
 * 읽기는 test_bit 이라 이 계열 전부가 같은 방식이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_cuir(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_CUIR, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_cuir - 이 경로의 CUIR 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * CUIR 잠금 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_cuir(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_CUIR, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_ifcc - 이 경로에 IFCC 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * IFCC(Interface Control Check) 오류가 난 경로임을 표시한다. 인터페이스
 * 수준의 검사 오류라 경로 자체를 의심해야 하는 상황이다.
 *
 * 이 함수는 __ 없는 **원자 판** 을 쓴다. 같은 플래그 묶음을 다루는 다른
 * 접근자들이 모두 비원자 판인데 IFCC 만 원자 판이라, 이 플래그는 다른
 * 문맥과 경쟁할 수 있다고 본 셈이다. 그 구별의 근거는 코드에 없다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_ifcc(struct dasd_device *device, int chp)
{
	set_bit(DASD_PATH_IFCC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_ifcc - 이 경로에 IFCC 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * IFCC 오류가 난 경로인지 확인한다.
 *
 * 읽기는 test_bit 이라 이 계열 전부가 같은 방식이다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_ifcc(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_IFCC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_ifcc - 이 경로의 IFCC 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * IFCC 오류 표시를 거둔다.
 *
 * 이 함수는 __ 없는 **원자 판** 을 쓴다. 같은 플래그 묶음을 다루는 다른
 * 접근자들이 모두 비원자 판인데 IFCC 만 원자 판이라, 이 플래그는 다른
 * 문맥과 경쟁할 수 있다고 본 셈이다. 그 구별의 근거는 코드에 없다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_ifcc(struct dasd_device *device, int chp)
{
	clear_bit(DASD_PATH_IFCC, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_clear_nohpf - 이 경로의 NOHPF 표시를 지운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로에서 HPF 를 쓸 수 없다는 표시를 거둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_clear_nohpf(struct dasd_device *device, int chp)
{
	__clear_bit(DASD_PATH_NOHPF, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_miscabled - 이 경로의 MISCABLED 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로의 배선이 잘못되었음을 표시한다 — 기대한 장치가 아닌 곳에 닿는
 * 상태이며, 그대로 쓰면 엉뚱한 볼륨에 I/O 를 보내게 된다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_miscabled(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_MISCABLED, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_miscabled - 이 경로에 MISCABLED 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * 배선이 잘못된 경로인지 확인한다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_miscabled(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_MISCABLED, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_nohpf - 이 경로의 NOHPF 표시를 세운다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 이 경로에서는 HPF(High Performance FICON)를 쓸 수 없음을 표시한다.
 * 경로마다 지원 여부가 다를 수 있어 경로 단위로 둔다.
 *
 * 이 함수가 쓰는 __ 접두사 판은 **원자적이지 않다** — 바깥의 다른 잠금이
 * 경쟁을 막는다는 전제이며, 그 잠금이 무엇인지는 이 헤더에서 확인 못 함.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline void dasd_path_nohpf(struct dasd_device *device, int chp)
{
	__set_bit(DASD_PATH_NOHPF, &device->path[chp].flags);
}

/* [한국어]
 * dasd_path_is_nohpf - 이 경로에 NOHPF 표시가 서 있는지 묻는다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: 0 이 아니면 표시가 서 있다.
 *
 * HPF 를 쓸 수 없는 경로인지 확인한다.
 *
 * 실행 컨텍스트: 경로 상태를 다루는 코드. 한 줄짜리 inline 이라 호출부의
 * 문맥을 그대로 따른다.
 *
 * 에러 경로: 없다. **chp 가 0~7 범위인지 검사하지 않으므로**, 범위를 벗어난
 * 값이 오면 배열 밖을 건드린다. 호출부가 그것을 보장한다는 전제다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 */
static inline int dasd_path_is_nohpf(struct dasd_device *device, int chp)
{
	return test_bit(DASD_PATH_NOHPF, &device->path[chp].flags);
}

/*
 * get functions for path masks
 * will return a path masks for the given device
 */

/* [한국어]
 * dasd_path_get_opm - 쓸 수 있는 경로들의 마스크를 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 온라인 경로 마스크.
 *
 * **이 계열에서 유일하게 루프가 없는 함수** 다. 다른 마스크들은 플래그를
 * 훑어 그때그때 만드는데, 이것만은 device->opm 에 보관된 값을 그대로
 * 돌려준다.
 *
 * 보관하는 이유는 I/O 를 띄울 때마다 쓰이는 값이라 매번 여덟 번을 도는
 * 비용이 아깝기 때문으로 보인다. 그 대가로 dasd_path_operational() 과
 * dasd_path_clear_oper() 가 플래그와 마스크를 **함께** 고쳐야 하며,
 * 둘이 어긋나면 쓸 수 없는 경로로 요청이 나간다.
 *
 * 실행 컨텍스트: I/O 제출 경로를 포함해 어디서든. 한 줄이라 호출부의
 * 문맥을 따른다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   I/O 제출·경로 처리 코드 → [이 함수]
 */
static inline __u8 dasd_path_get_opm(struct dasd_device *device)
{
	return device->opm;
}

/* [한국어]
 * dasd_path_get_tbvpm - 검증이 필요한 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 다시 검증해야 하는 경로들을 모은다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_tbvpm(struct dasd_device *device)
{
	int chp;
	__u8 tbvpm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로가 검증 대기 상태인지 본다. */
		if (dasd_path_need_verify(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			tbvpm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return tbvpm;
}

/* [한국어]
 * dasd_path_get_fcsecpm - 보안 정보 갱신이 필요한 경로가 하나라도 있는지 묻는다
 *
 * @device: 대상 장치.
 * @return: 1 이면 그런 경로가 있고, 0 이면 없다.
 *
 * **이름이 마스크(pm)를 돌려줄 것처럼 보이지만 실제로는 참/거짓이다.**
 * 같은 계열의 다른 _get_..pm 함수들이 모두 8비트 마스크를 만드는 것과
 * 달리, 이것만 조건에 맞는 경로를 처음 만나는 순간 1 을 돌려주고 끝난다.
 * 반환형이 __u8 이 아니라 int 인 것도 그 차이를 드러낸다.
 *
 * 즉 어느 경로인지는 알려 주지 않고 있는지 없는지만 답한다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증 코드 → [이 함수] → dasd_path_need_fcsec()
 */
static inline int dasd_path_get_fcsecpm(struct dasd_device *device)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로가 보안 갱신 대기 상태인지 본다. */
		if (dasd_path_need_fcsec(device, chp))
			/* [한국어] **하나라도 있으면 곧바로 1 을 돌려준다** — 마스크를 만들지 않는다. */
			return 1;

	return 0;
}

/* [한국어]
 * dasd_path_get_nppm - 비선호 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 비선호로 표시된 경로들을 모은다. 선호 경로가 모두 막혔을 때 대신 쓸
 * 후보를 고르는 데 쓰인다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_nppm(struct dasd_device *device)
{
	int chp;
	__u8 npm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++) {
		/* [한국어] 이 경로가 비선호인지 본다. */
		if (dasd_path_is_nonpreferred(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			npm |= 0x80 >> chp;
	}
	return npm;
}

/* [한국어]
 * dasd_path_get_ppm - 선호 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 선호로 표시된 경로들을 모은다. 위 비선호 마스크와 짝을 이룬다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_ppm(struct dasd_device *device)
{
	int chp;
	__u8 ppm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로가 선호인지 본다. */
		if (dasd_path_is_preferred(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			ppm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return ppm;
}

/* [한국어]
 * dasd_path_get_cablepm - 배선이 잘못된 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 배선 오류로 표시된 경로들을 모은다. 이 마스크에 든 경로는 쓰면 안 된다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_cablepm(struct dasd_device *device)
{
	int chp;
	__u8 cablepm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로의 배선이 잘못됐는지 본다. */
		if (dasd_path_is_miscabled(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			cablepm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return cablepm;
}

/* [한국어]
 * dasd_path_get_cuirpm - CUIR 로 잠긴 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 제어 장치가 구성 변경을 위해 거둬 간 경로들을 모은다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_cuirpm(struct dasd_device *device)
{
	int chp;
	__u8 cuirpm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로가 CUIR 로 잠겼는지 본다. */
		if (dasd_path_is_cuir(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			cuirpm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return cuirpm;
}

/* [한국어]
 * dasd_path_get_ifccpm - IFCC 오류가 난 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * 인터페이스 제어 검사 오류가 난 경로들을 모은다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_ifccpm(struct dasd_device *device)
{
	int chp;
	__u8 ifccpm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로에 IFCC 오류가 났는지 본다. */
		if (dasd_path_is_ifcc(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			ifccpm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return ifccpm;
}

/* [한국어]
 * dasd_path_get_hpfpm - HPF 를 쓸 수 없는 경로들의 마스크를 만들어 돌려준다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * HPF 를 쓸 수 없다고 표시된 경로들을 모은다.
 * [상류 코드 관찰] 이름은 hpfpm 인데 **모으는 것은 NOHPF 표시가 선 경로들**
 * 이다. 즉 이름만 보면 HPF 를 쓸 수 있는 경로로 읽히지만 실제로는 그
 * 반대이며, 원본(1f0e418bb6)에서 확인했고 코드는 고치지 않았다.
 *
 * 여덟 경로를 돌며 조건에 맞는 것마다 비트를 세워 **경로 마스크** 를
 * 만든다. 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트이며,
 * 채널 경로 마스크의 관례를 따른 것이다.
 *
 * 이렇게 그때그때 만드는 이유는 마스크를 따로 보관하지 않기 때문이다 —
 * 진실은 경로별 플래그에 있고 마스크는 그 파생값이라, 둘이 어긋날
 * 여지를 아예 없앤 설계다. device->opm 만 예외로 보관된다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않으므로, 도는 동안
 * 다른 문맥이 플래그를 바꾸면 **일관되지 않은 스냅숏** 이 나올 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → 해당 dasd_path_is_/need_ 판정 함수
 */
static inline __u8 dasd_path_get_hpfpm(struct dasd_device *device)
{
	int chp;
	__u8 hpfpm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 이 경로에서 HPF 를 쓸 수 없는지 본다. */
		if (dasd_path_is_nohpf(device, chp))
			/* [한국어] 맞으면 마스크에 그 비트를 세운다. */
			hpfpm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return hpfpm;
}

/* [한국어]
 * dasd_path_get_fcs_path - 이 경로의 FC 종단 보안 수준을 돌려준다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 * @return: DASD_FC_SECURITY_ 계열 값.
 *
 * 플래그가 아니라 경로 구조체에 보관된 값을 그대로 읽는다. 보안 수준은
 * 켜짐/꺼짐이 아니라 네 단계 중 하나라 비트로 표현할 수 없기 때문이다.
 *
 * 실행 컨텍스트: sysfs 출력과 경로 검증.
 *
 * 에러 경로: 없다. 여기서도 chp 범위를 검사하지 않는다.
 *
 * 호출 체인:
 *   sysfs·경로 검증 코드 → [이 함수]
 */
static inline u8 dasd_path_get_fcs_path(struct dasd_device *device, int chp)
{
	return device->path[chp].fc_security;
}

/* [한국어]
 * dasd_path_get_fcs_device - 쓸 수 있는 경로들의 보안 수준이 하나로 일치하는지 확인한다
 *
 * @device: 대상 장치.
 * @return: 모든 경로가 같으면 그 보안 수준, 하나라도 다르면 -EINVAL.
 *
 * 장치 **전체** 의 보안 수준을 말하려면 모든 경로가 같은 수준이어야 한다.
 * 경로마다 다르면 어느 것을 장치의 수준이라 할 수 없으므로 오류로 답한다.
 *
 * 루프가 둘로 나뉜 것이 이 함수의 구조다.
 *   1) 첫 루프: 쓸 수 있는(opm 에 든) 경로 중 **첫 번째** 를 찾아 그 수준을
 *      기준값으로 삼고 곧바로 빠져나온다.
 *   2) 둘째 루프: `chp` 를 **초기화하지 않고 이어받아** 그 뒤의 경로들만
 *      훑으며 기준값과 다른 것이 있는지 본다.
 * 둘째 루프의 `for (; chp < 8; chp++)` 에서 초기식이 비어 있는 것이 그
 * 이어받기이며, 덕분에 이미 본 경로를 다시 보지 않는다.
 *
 * [상류 코드 관찰] 쓸 수 있는 경로가 **하나도 없으면** 첫 루프가 끝까지
 * 돌아 chp 가 8 이 되고 fc_sec 은 초기값 0 인 채로 남는다. 둘째 루프는
 * 돌지 않으므로 0 — 즉 DASD_FC_SECURITY_UNSUP — 이 그대로 반환된다.
 * '경로가 없음' 과 '보안을 지원하지 않음' 이 같은 값으로 답해지는 셈인데,
 * 호출부가 그 둘을 구별할 방법은 이 함수 안에 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 로 장치 단위 보안 수준을 보여 줄 때.
 *
 * 에러 경로: 경로마다 수준이 다르면 -EINVAL.
 *
 * 호출 체인:
 *   sysfs 의 장치 보안 수준 표시 → [이 함수]
 */
static inline int dasd_path_get_fcs_device(struct dasd_device *device)
{
	u8 fc_sec = 0;
	int chp;
/* [한국어] 먼저 기준값을 정하기 위해 훑는다. */

	for (chp = 0; chp < 8; chp++) {
		/* [한국어] 쓸 수 있는 경로인지 본다. 여기서는 플래그가 아니라 opm 마스크를 쓴다. */
		if (device->opm & (0x80 >> chp)) {
			/* [한국어] 첫 번째로 찾은 경로의 보안 수준을 기준값으로 삼는다. */
			fc_sec = device->path[chp].fc_security;
			/* [한국어] 기준값을 잡았으므로 곧바로 빠져나온다. chp 가 그 자리에 남는다. */
			break;
		}
	}
	for (; chp < 8; chp++) {
		/* [한국어] 이번에는 그 뒤의 쓸 수 있는 경로들만 본다. */
		if (device->opm & (0x80 >> chp))
			/* [한국어] 기준값과 다른 수준을 가진 경로가 있는지 확인한다. */
			if (device->path[chp].fc_security != fc_sec)
				/* [한국어] 하나라도 다르면 장치 단위 수준을 말할 수 없으므로 오류로 답한다. */
				return -EINVAL;
	}

	return fc_sec;
}

/* [한국어]
 * dasd_path_add_tbvpm - 마스크에 든 경로들에 검증 대기 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 다시 검증해야 할 대상으로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_verify()
 */
/*
 * add functions for path masks
 * the existing path mask will be extended by the given path mask
 */
static inline void dasd_path_add_tbvpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 검증 대기로 표시한다. */
			dasd_path_verify(device, chp);
}

/* [한국어]
 * dasd_path_get_notoperpm - 쓸 수 없는 이유가 하나라도 있는 경로들의 마스크를 만든다
 *
 * @device: 대상 장치.
 * @return: 8비트 경로 마스크.
 *
 * **이 계열에서 유일하게 조건이 여럿인 함수** 다. 다른 마스크 함수들이
 * 플래그 하나씩만 보는 데 비해, 이것은 네 가지 — HPF 불가, IFCC 오류,
 * CUIR 잠금, 배선 오류 — 중 **하나라도** 해당하면 비트를 세운다.
 *
 * 즉 '쓸 수 없는 이유' 를 한데 모은 마스크이며, 이유가 무엇인지는
 * 구별하지 않는다. 이유별로 알고 싶으면 각각의 _get_..pm 을 써야 한다.
 *
 * 이름의 notoper 가 DASD_PATH_OPERATIONAL 플래그와 헷갈리기 쉬운데,
 * **그 플래그는 보지 않는다** — 위 네 이유만으로 판정한다.
 *
 * 실행 컨텍스트: 경로 상태를 살피는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 처리 코드 → [이 함수]
 *     → dasd_path_is_nohpf() / _is_ifcc() / _is_cuir() / _is_miscabled()
 */
static inline __u8 dasd_path_get_notoperpm(struct dasd_device *device)
{
	int chp;
	__u8 nopm = 0x00;
/* [한국어] 여덟 경로를 훑는다. */

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 네 가지 이유 중 **하나라도** 해당하는지 본다. */
		if (dasd_path_is_nohpf(device, chp) ||
		    /* [한국어] 이유들이 OR 로 이어진다. */
		    dasd_path_is_ifcc(device, chp) ||
		    dasd_path_is_cuir(device, chp) ||
		    dasd_path_is_miscabled(device, chp))
			nopm |= 0x80 >> chp;
	/* [한국어] 만든 마스크를 돌려준다. */
	return nopm;
}

/* [한국어]
 * dasd_path_add_opm - 마스크에 든 경로들을 쓸 수 있게 만들고 부정적 표시를 모두 거둔다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * **이 _add_ 계열에서 유일하게 표시를 세우기만 하지 않는 함수** 다.
 * 경로를 쓸 수 있게 만들면서, 그 경로에 붙어 있던 부정적 표시 넷을
 * 함께 지운다 — 안의 상류 주석이 그 이유를 밝힌다: 쓰는 경로가
 * 부정적 목록에 남아 있어서는 안 된다는 것이다.
 *
 * 지우는 넷은 HPF 불가, CUIR 잠금, 배선 오류, IFCC 오류이며, 이는
 * 위 dasd_path_get_notoperpm() 이 보는 넷과 정확히 같다. 즉 이 함수를
 * 거친 경로는 그 마스크에서 반드시 빠진다.
 *
 * 첫 호출인 dasd_path_operational() 이 device->opm 도 함께 고치므로,
 * 플래그와 마스크가 같이 갱신된다.
 *
 * 실행 컨텍스트: 경로가 살아났을 때. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증 코드 → [이 함수]
 *     → dasd_path_operational() → dasd_path_clear_nohpf()
 *     → dasd_path_clear_cuir() → dasd_path_clear_cable()
 *     → dasd_path_clear_ifcc()
 */
static inline void dasd_path_add_opm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp)) {
			/* [한국어] 해당 경로를 쓸 수 있게 만든다. 이 안에서 opm 도 함께 갱신된다. */
			dasd_path_operational(device, chp);
			/*
			 * if the path is used
			 * it should not be in one of the negative lists
			 */
			dasd_path_clear_nohpf(device, chp);
			dasd_path_clear_cuir(device, chp);
			/* [한국어] 배선 오류 표시도 거둔다. */
			dasd_path_clear_cable(device, chp);
			/* [한국어] IFCC 오류 표시도 거둔다. 이로써 부정적 표시 넷이 모두 지워진다. */
			dasd_path_clear_ifcc(device, chp);
		}
}

/* [한국어]
 * dasd_path_add_cablepm - 마스크에 든 경로들에 배선 오류 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 배선 오류로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_miscabled()
 */
static inline void dasd_path_add_cablepm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 배선 오류로 표시한다. */
			dasd_path_miscabled(device, chp);
}

/* [한국어]
 * dasd_path_add_cuirpm - 마스크에 든 경로들에 CUIR 잠금 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 CUIR 로 잠긴 것으로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_cuir()
 */
static inline void dasd_path_add_cuirpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 CUIR 잠금으로 표시한다. */
			dasd_path_cuir(device, chp);
}

/* [한국어]
 * dasd_path_add_ifccpm - 마스크에 든 경로들에 IFCC 오류 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 IFCC 오류가 난 것으로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_ifcc()
 */
static inline void dasd_path_add_ifccpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 IFCC 오류로 표시한다. */
			dasd_path_ifcc(device, chp);
}

/* [한국어]
 * dasd_path_add_nppm - 마스크에 든 경로들에 비선호 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 비선호로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_nonpreferred()
 */
static inline void dasd_path_add_nppm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 비선호로 표시한다. */
			dasd_path_nonpreferred(device, chp);
}

/* [한국어]
 * dasd_path_add_nohpfpm - 마스크에 든 경로들에 HPF 불가 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들에서 HPF 를 쓸 수 없다고 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_nohpf()
 */
static inline void dasd_path_add_nohpfpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 HPF 불가로 표시한다. */
			dasd_path_nohpf(device, chp);
}

/* [한국어]
 * dasd_path_add_ppm - 마스크에 든 경로들에 선호 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들을 선호로 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_preferred()
 */
static inline void dasd_path_add_ppm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 선호로 표시한다. */
			dasd_path_preferred(device, chp);
}

/* [한국어]
 * dasd_path_add_fcsecpm - 마스크에 든 경로들에 보안 갱신 대기 표시를 더한다
 *
 * @device: 대상 장치.
 * @pm: 대상 경로들의 8비트 마스크.
 *
 * 마스크에 든 경로들의 보안 정보를 갱신해야 한다고 표시한다.
 *
 * 주어진 마스크의 비트가 선 경로마다 해당 표시를 세운다. 위 상류 주석이
 * 밝히듯 **기존 표시를 지우지 않고 더하기만** 하므로, 마스크에 없는 경로의
 * 표시는 그대로 남는다. 지우는 것까지 하려면 아래 _set_ 계열을 써야 한다.
 *
 * 마스크의 비트 자리가 `0x80 >> chp` 이라 경로 0 이 최상위 비트다.
 *
 * 실행 컨텍스트: 경로 상태를 바꾸는 코드. 잠금을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증·오류 처리 코드(dasd.c, dasd_eckd.c) → [이 함수]
 *     → dasd_path_fcsec()
 */
static inline void dasd_path_add_fcsecpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로를 보안 갱신 대기로 표시한다. */
			dasd_path_fcsec(device, chp);
}

/* [한국어]
 * dasd_path_set_tbvpm - 검증 대기 표시를 주어진 마스크와 정확히 일치시킨다
 *
 * @device: 대상 장치.
 * @pm: 원하는 상태를 나타내는 8비트 마스크.
 *
 * 위 상류 주석이 _add_ 계열과의 차이를 밝힌다 — **기존 표시를 대체한다.**
 * 마스크에 든 경로는 표시를 세우고, **들지 않은 경로는 지운다.** 그래서
 * 여덟 경로 전부가 이 마스크가 말하는 상태로 맞춰진다.
 *
 * if/else 로 두 갈래를 모두 처리하는 것이 _add_ 판(if 만 있는)과 눈에
 * 띄게 다른 점이다.
 *
 * 실행 컨텍스트: 경로 검증 상태를 통째로 다시 정할 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증 코드 → [이 함수]
 *     → dasd_path_verify() / dasd_path_clear_verify()
 */
/*
 * set functions for path masks
 * the existing path mask will be replaced by the given path mask
 */
static inline void dasd_path_set_tbvpm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 마스크에 들었으면 검증 대기로 표시하고, */
			dasd_path_verify(device, chp);
		/* [한국어] 들지 않았으면 그 표시를 지운다 — 이 else 가 _add_ 판과의 차이다. */
		else
			dasd_path_clear_verify(device, chp);
}

/* [한국어]
 * dasd_path_set_opm - 쓸 수 있는 경로를 주어진 마스크와 정확히 일치시킨다
 *
 * @device: 대상 장치.
 * @pm: 원하는 상태를 나타내는 8비트 마스크.
 *
 * dasd_path_add_opm() 의 '대체' 판이다. 차이는 루프의 첫 줄에 있다 —
 * **모든 경로를 일단 쓸 수 없게 만든 뒤** 마스크에 든 것만 다시 살린다.
 * 그래서 마스크에 없는 경로는 반드시 꺼진다.
 *
 * 살리는 쪽의 처리는 add 판과 똑같다. 경로를 쓸 수 있게 만들고, 안의
 * 상류 주석대로 부정적 표시 넷(HPF 불가, CUIR 잠금, 배선 오류, IFCC
 * 오류)을 함께 거둔다.
 *
 * [상류 코드 관찰] 마스크에 든 경로도 매 반복마다 **한 번 껐다가 다시
 * 켜진다.** dasd_path_clear_oper() 와 dasd_path_operational() 이 각각
 * device->opm 을 고치므로, 그 사이 아주 짧게 opm 에서 그 경로가 빠져
 * 있는 순간이 생긴다. 이 함수가 잠금을 잡지 않으므로 그 틈에 다른 문맥이
 * opm 을 읽으면 실제보다 적은 경로를 보게 된다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 경로 구성을 통째로 다시 정할 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 검증 코드 → [이 함수]
 *     → dasd_path_clear_oper() → dasd_path_operational()
 *     → dasd_path_clear_nohpf() / _cuir() / _cable() / _ifcc()
 */
static inline void dasd_path_set_opm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++) {
		/* [한국어] **먼저 무조건 끈다.** 마스크에 없는 경로가 확실히 꺼지게 하는 것이
		 * 이 줄의 목적이며, _add_ 판에는 없다. */
		dasd_path_clear_oper(device, chp);
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp)) {
			/* [한국어] 마스크에 들었으면 다시 켠다. 방금 끈 경로도 여기서 되살아난다. */
			dasd_path_operational(device, chp);
			/*
			 * if the path is used
			 * it should not be in one of the negative lists
			 */
			dasd_path_clear_nohpf(device, chp);
			dasd_path_clear_cuir(device, chp);
			/* [한국어] 배선 오류 표시를 거둔다. */
			dasd_path_clear_cable(device, chp);
			/* [한국어] IFCC 오류 표시를 거둔다. */
			dasd_path_clear_ifcc(device, chp);
		}
	}
}

/* [한국어]
 * dasd_path_remove_opm - 마스크에 든 경로들만 쓸 수 없게 만든다
 *
 * @device: 대상 장치.
 * @pm: 제거할 경로들의 8비트 마스크.
 *
 * 위 상류 주석이 밝히는 '제거' 계열이며, 이 헤더에서 그 계열은 이것
 * 하나뿐이다. 마스크에 든 경로만 끄고 **나머지는 건드리지 않는다** —
 * 전부를 다시 정하는 _set_ 판과 그 점이 다르다.
 *
 * 경로 하나가 죽었을 때 그것만 빼는 용도이며, 살아 있는 다른 경로의
 * 상태를 보존해야 하므로 이 형태가 필요하다.
 *
 * dasd_path_clear_oper() 가 device->opm 도 함께 고치므로 플래그와
 * 마스크가 같이 갱신된다.
 *
 * 실행 컨텍스트: 경로가 죽었을 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 오류 처리 코드 → [이 함수] → dasd_path_clear_oper()
 */
/*
 * remove functions for path masks
 * the existing path mask will be cleared with the given path mask
 */
static inline void dasd_path_remove_opm(struct dasd_device *device, __u8 pm)
{
	int chp;

	for (chp = 0; chp < 8; chp++) {
		/* [한국어] 마스크의 이 경로 비트가 서 있는지 본다. 경로 0 이 최상위 비트다. */
		if (pm & (0x80 >> chp))
			/* [한국어] 해당 경로만 끈다. 나머지는 건드리지 않는다. */
			dasd_path_clear_oper(device, chp);
	}
}

/* [한국어]
 * dasd_path_available - 새로 생긴 경로를 검증 대기로 돌린다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 위 상류 주석이 의도를 그대로 밝힌다 — 새로 쓸 수 있게 된 경로를
 * **곧바로 쓰지 않고** 검증 대기 목록에 넣는다.
 *
 * 두 줄의 순서가 그 뜻이다. 먼저 쓸 수 있는 상태에서 빼고, 그다음
 * 검증이 필요하다고 표시한다. 그래서 검증이 끝나기 전까지는 이 경로로
 * I/O 가 나가지 않는다.
 *
 * '경로가 생겼다' 와 '경로를 쓴다' 를 분리하는 것이 이 드라이버의 안전
 * 장치다 — 배선이 잘못됐거나 다른 볼륨에 닿는 경로일 수 있기 때문이다.
 *
 * 실행 컨텍스트: 경로가 새로 생겼을 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 상태 변경 처리 → [이 함수]
 *     → dasd_path_clear_oper() → dasd_path_verify()
 */
/*
 * add the newly available path to the to be verified pm and remove it from
 * normal operation until it is verified
 */
static inline void dasd_path_available(struct dasd_device *device, int chp)
{
	dasd_path_clear_oper(device, chp);
	dasd_path_verify(device, chp);
}

/* [한국어]
 * dasd_path_notoper - 경로를 쓸 수 없게 만들고 선호도 표시도 거둔다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * 경로가 죽었을 때 부르며, 세 가지를 지운다 — 쓸 수 있음, 선호, 비선호다.
 *
 * **선호도 표시까지 지우는 이유** 는 그 값이 살아 있는 경로에 대해서만
 * 뜻을 갖기 때문이다. 죽은 경로에 '선호' 가 남아 있으면 나중에 되살아날
 * 때 검증 없이 우선 선택될 여지가 생긴다.
 *
 * 반대로 부정적 표시들(IFCC, CUIR 등)은 지우지 않는다 — 그것들은 왜
 * 죽었는지를 알려 주는 정보라 남겨 두어야 한다.
 *
 * 실행 컨텍스트: 경로가 죽었을 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 오류 처리 → [이 함수]
 *     → dasd_path_clear_oper() → dasd_path_clear_preferred()
 *     → dasd_path_clear_nonpreferred()
 */
static inline void dasd_path_notoper(struct dasd_device *device, int chp)
{
	dasd_path_clear_oper(device, chp);
	dasd_path_clear_preferred(device, chp);
	/* [한국어] 비선호 표시도 거둔다. 죽은 경로에는 선호도가 의미를 잃기 때문이다. */
	dasd_path_clear_nonpreferred(device, chp);
}

/* [한국어]
 * dasd_path_fcsec_update - 이 경로의 보안 정보를 갱신 대상으로 표시한다
 *
 * @device: 대상 장치.
 * @chp: 경로 번호(0~7).
 *
 * dasd_path_fcsec() 을 그대로 부르는 **한 줄짜리 껍데기** 다. 하는 일이
 * 완전히 같아 이름만 다르다.
 *
 * [상류 코드 관찰] 감싸는 함수를 따로 둔 이유가 코드에 드러나 있지 않다.
 * 호출부에서 '갱신이 필요하다' 는 의도를 이름으로 드러내려는 것으로
 * 보이나, 근거는 이 트리에서 확인 못 함.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 보안 정보가 바뀌었을 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 처리 코드 → [이 함수] → dasd_path_fcsec()
 */
static inline void dasd_path_fcsec_update(struct dasd_device *device, int chp)
{
	dasd_path_fcsec(device, chp);
}

/* [한국어]
 * dasd_path_no_path - 모든 경로를 쓸 수 없게 만들고 검증 대기도 모두 거둔다
 *
 * @device: 대상 장치.
 *
 * 위 상류 주석대로 **경로를 전부 정상 동작에서 뺀다.** 장치에 닿을 길이
 * 완전히 끊겼을 때 부른다.
 *
 * 두 부분이다.
 * 1. 여덟 경로 각각에 dasd_path_notoper() 를 적용한다 — 즉 쓸 수 있음과
 *    선호도 표시가 모두 지워진다.
 * 2. **검증 대기 표시까지 한 번에 거둔다.** 경로가 하나도 없는 마당에
 *    검증할 것도 없기 때문이며, 남겨 두면 나중에 헛되이 검증을 시도한다.
 *
 * 2번이 1번의 루프 밖에 있는 것은 dasd_path_clear_all_verify() 가 자기
 * 안에서 이미 여덟 경로를 돌기 때문이다.
 *
 * 실행 컨텍스트: 모든 경로가 끊겼을 때.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   경로 오류 처리 → [이 함수]
 *     → dasd_path_notoper() ×8 → dasd_path_clear_all_verify()
 */
/*
 * remove all paths from normal operation
 */
static inline void dasd_path_no_path(struct dasd_device *device)
{
	int chp;

	for (chp = 0; chp < 8; chp++)
		/* [한국어] 경로마다 쓸 수 없음과 선호도 표시를 지운다. */
		dasd_path_notoper(device, chp);

	dasd_path_clear_all_verify(device);
}

/* end - path handling */

#endif				/* DASD_H */
