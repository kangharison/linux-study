// SPDX-License-Identifier: GPL-2.0
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2009
 */


/* [한국어 설명] FBA 디시플린 본체 — 고정 블록 장치용 DASD 디시플린 (dasd_fba.c)
 *
 * === 파일의 역할 ===
 * DASD 드라이버는 장치 종류마다 디시플린(discipline)이라는 가상 함수표를 갈아
 * 끼우는 구조이고, 이 파일은 그중 **FBA(Fixed Block Architecture) 판** 이다.
 * FBA 볼륨은 크기가 같은 블록이 0번부터 번호순으로 늘어선 디스크이므로,
 * 이 파일이 하는 일은 결국 하나로 요약된다 — 블록 계층이 준 struct request 를
 * 받아 **Define Extent 하나 + Locate Record + 블록별 읽기/쓰기 CCW** 라는
 * 짧은 채널 프로그램으로 옮기는 것이다.
 *
 * 파일은 다섯 덩어리로 나뉜다. 첫째, CCW 드라이버 등록(dasd_fba_ids 표와
 * dasd_fba_driver). 둘째, 채널 프로그램 조각을 만드는 도우미 넷
 * (define_extent, locate_record, ccw_write_no_data, ccw_write_zero). 셋째,
 * 장치를 알아보고 볼륨을 분석하는 코드(dasd_fba_check_characteristics,
 * dasd_fba_do_analysis, dasd_fba_fill_geometry). 넷째, I/O 경로의 핵심인
 * build_cp 세 개와 free_cp. 다섯째, 오류 처리와 디버그 출력(erp_action,
 * erp_postaction, dump_sense, dump_sense_dbf).
 *
 * 맨 끝의 struct dasd_discipline dasd_fba_discipline 이 이 모든 것을 묶는
 * 결선판이다. **여기에 무엇이 있고 무엇이 없는지를 보는 것이 이 파일을 읽는
 * 가장 빠른 길** 이다 — 채워진 칸이 20개, 비어 있는 칸이 서른 개 남짓이다.
 *
 * 이 파일은 커널 모듈 하나를 이룬다. dasd_fba_init() 이 CCW 드라이버를
 * 등록하고 0 으로 채운 페이지 하나를 잡아 두며, dasd_fba_cleanup() 이 그
 * 둘을 되돌린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 블록 계층 아래, DASD 코어(dasd.c) 옆, s390 채널 서브시스템 위에 있다.
 * 이 파일의 함수는 **직접 불리는 것이 거의 없고 거의 전부 vtable 을 통해**
 * 불린다. 그 호출 지점을 모으면 다음과 같다.
 *
 *   [장치를 올릴 때 — 프로세스 컨텍스트]
 *     ccw 버스가 dasd_fba_set_online() 을 부른다
 *       -> dasd.c:3451 dasd_generic_set_online(cdev, &dasd_fba_discipline)
 *         -> dasd.c:3502 discipline->check_device
 *            == dasd_fba_check_characteristics()   [장치 특성 32바이트 읽기]
 *         -> 상태 기계가 basic -> ready 로 올라가며
 *            dasd.c:310 discipline->do_analysis == dasd_fba_do_analysis()
 *            dasd.c:321 discipline->max_sectors  == dasd_fba_max_sectors()
 *
 *   [I/O 를 낼 때 — blk-mq 제출 경로, dq->lock 을 쥔 채]
 *     dasd.c:3018 do_dasd_request()
 *       -> dasd.c:3067 discipline->build_cp == dasd_fba_build_cp()
 *         -> discard/write-zeroes 면 dasd_fba_build_cp_discard()
 *         -> 그 밖이면          dasd_fba_build_cp_regular()
 *
 *   [완료·오류를 처리할 때 — 인터럽트와 softirq]
 *     dasd.c:1584 dasd_int_handler()                     [인터럽트 컨텍스트]
 *       -> dasd.c:1664 discipline->dump_sense_dbf == dasd_fba_dump_sense_dbf()
 *       -> dasd.c:1667 discipline->dump_sense     == dasd_fba_dump_sense()
 *       -> dasd.c:1668 discipline->check_for_device_change
 *                                    == dasd_fba_check_for_device_change()
 *     dasd.c 의 두 tasklet                                [softirq 컨텍스트]
 *       -> dasd.c:2243, 2766 discipline->handle_terminated_request
 *                                    == dasd_fba_handle_terminated_request()
 *       -> dasd.c:2247, 2772 discipline->erp_action == dasd_fba_erp_action()
 *       -> dasd.c:2679       discipline->erp_postaction
 *                                    == dasd_fba_erp_postaction()
 *       -> dasd.c:2694       discipline->free_cp   == dasd_fba_free_cp()
 *
 *   [사용자 공간에서 물을 때 — 프로세스 컨텍스트]
 *     dasd.c:3291 discipline->fill_geometry == dasd_fba_fill_geometry()
 *     dasd_ioctl.c:513 discipline->fill_info == dasd_fba_fill_info()
 *
 *   [경로 상태가 바뀔 때]
 *     dasd.c:2045 discipline->pe_handler == dasd_fba_pe_handler()
 *
 * 즉 이 파일의 코드는 **프로세스 컨텍스트, softirq, 인터럽트 세 곳 모두에서**
 * 실행된다. 그래서 어느 함수도 잠들 수 없고, 메모리를 잡는 자리는 전부
 * GFP_ATOMIC 이거나 미리 잡아 둔 정적 풀(dasd_smalloc_request)을 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위로는 dasd.c 와 dasd_int.h 에 기댄다. cqr(struct dasd_ccw_req) 할당은
 * dasd.c:1136 의 dasd_smalloc_request() 가, 해제는 dasd.c:1221 의
 * dasd_sfree_request() 가 맡는다. 둘 다 장치마다 미리 잡아 둔 정적 메모리
 * 풀(device->ccw_chunks)에서 떼어 오고 돌려주므로 **인터럽트 문맥에서도
 * 실패 없이 할당할 수 있다.** 실제로 채널에 명령을 내보내고 끊는 일은
 * 이 파일이 하지 않고 dasd.c:1328 의 dasd_start_IO() 와 dasd.c:1270 의
 * dasd_term_IO() 를 vtable 에 그대로 꽂아 쓴다.
 *
 * 오류 복구도 남의 것을 쓴다. dasd_fba_erp_action() 은 센스 데이터를 보지
 * 않고 무조건 dasd_erp.c 의 dasd_default_erp_action() 을 돌려주며, 그 함수는
 * '재시도 횟수가 남아 있으면 그냥 다시 시도' 라는 가장 단순한 복구다.
 * ECKD 가 dasd_3990_erp.c 라는 대규모 센스 해석 상태 기계를 갖는 것과
 * 대조적이다.
 *
 * 옆으로는 dasd_fba.h 에 기댄다. 채널에 나갈 두 전송 형식(struct DE_fba_data,
 * struct LO_fba_data)과 장치가 올려 보내는 특성(struct dasd_fba_characteristics),
 * 그리고 상한 DASD_FBA_MAX_BLOCKS 가 전부 그쪽 정의다. 그 헤더를 포함하는
 * 파일은 이 파일 하나뿐이다.
 *
 * 아래로는 s390 채널 서브시스템에 기댄다 — struct ccw1 과 그 flags 비트
 * (CCW_FLAG_CC, CCW_FLAG_DC, CCW_FLAG_SLI, CCW_FLAG_IDA), 주소 변환
 * (virt_to_dma32, dma32_to_virt, dma64_to_virt), IDAL 처리
 * (idal_is_needed, idal_create_words), EBCDIC 변환(ASCEBC), TOD 시계
 * (get_tod_clock), 인터럽트 응답 블록 struct irb 와 그 접근자들이 그것이다.
 * **이 트리는 sparse checkout 이라 arch/s390 이 없어** 그 계층의 규칙은
 * 확인 못 함으로 적었다. 반면 블록 계층(include/linux/blk-mq.h,
 * include/linux/blkdev.h)은 이 트리에 있으므로 blk_rq_pos(), blk_rq_sectors(),
 * rq_data_dir(), rq_for_each_segment(), blk_noretry_request() 는 실물을
 * 확인해 적었다.
 *
 * 데이터 흐름은 한 방향으로 정리된다. blk-mq 의 struct request 가
 * build_cp 에서 struct dasd_ccw_req 로 바뀌고, cqr->cpaddr 이 가리키는
 * struct ccw1 배열과 cqr->data 가 가리키는 전송 형식 버퍼가 하드웨어가 읽는
 * 최종 형태다. 반대로 완료 상태는 struct irb 로 돌아와 cqr->irb 에 복사되고,
 * 실패하면 dump_sense 가 그것을 사람이 읽을 수 있는 글로 풀어 커널 로그에 찍는다.
 *
 * === 주요 함수/구조체 요약 ===
 * dasd_fba_discipline          이 파일의 결선판. 20개 칸을 채우고 나머지는
 *                              비워 둔다. 파일 맨 끝에 있다.
 * dasd_fba_check_characteristics
 *                              장치를 알아보는 첫 관문. private 영역과
 *                              dasd_block 을 잡고, 장치 특성 32바이트를 읽고,
 *                              discard 기능 비트를 켠다.
 * dasd_fba_do_analysis         볼륨 분석. 블록 크기를 검사하고 총 블록 수와
 *                              s2b_shift 를 정한다. **본문이 열 줄뿐이다.**
 * define_extent                Define Extent CCW 와 그 자료 16바이트를 채운다.
 * locate_record                Locate Record CCW 와 그 자료 8바이트를 채운다.
 * count_ccws                   discard 요청에 CCW 가 몇 개 필요한지 미리 센다.
 * dasd_fba_build_cp            I/O 경로의 진입점. discard 인지 아닌지로 갈린다.
 * dasd_fba_build_cp_regular    일반 읽기·쓰기용 채널 프로그램을 만든다.
 *                              이 파일에서 가장 긴 함수다.
 * dasd_fba_build_cp_discard    discard/write-zeroes 용 채널 프로그램을 만든다.
 * dasd_fba_free_cp             요청을 정리하고 cqr 을 정적 풀에 돌려준다.
 * dasd_fba_dump_sense          실패한 요청의 센스 데이터와 채널 프로그램을
 *                              사람이 읽을 수 있게 커널 로그에 찍는다.
 * struct dasd_fba_private      이 디시플린이 장치마다 들고 다니는 사적 상태.
 *                              필드가 **장치 특성 하나뿐** 이다.
 * dasd_fba_zero_page           0 으로 채운 페이지 하나. discard 경로에서
 *                              '진짜 0 을 써야 하는' 조각의 데이터 원본이다.
 *
 * === ECKD 와 무엇이 다른가 — 이 파일을 dasd_eckd.c 옆에 놓고 보기 ===
 * 두 파일은 같은 vtable 을 채우지만 규모가 여덟 배 넘게 차이 난다
 * (dasd_eckd.c 6976줄 대 이 파일 811줄). 차이의 목록은 이렇다.
 *
 *   vtable 칸 수   ECKD 53칸, FBA 20칸, DIAG 16칸이다. FBA 가 비워 둔 칸에는
 *                  format_device, check_device_format, ioctl, get_uid,
 *                  check_attention, is_ese 와 씬 프로비저닝 관련 칸 전부,
 *                  pprc 관련 칸, hpf 관련 칸이 들어간다. 그 하나하나가
 *                  'FBA 에는 그 개념이 없다' 는 뜻이다.
 *   채널 프로그램  ECKD 는 Prefix(0xE7)로 DE 와 LRE 를 한 CCW 로 합치거나
 *                  전송 모드(zHPF)에서 TCW 한 덩어리로 바꾸는 길이 있다.
 *                  FBA 에는 그런 갈래가 없고 언제나 DE + LO + 데이터 CCW 다.
 *   볼륨 분석      ECKD 는 트랙 0 과 1 의 카운트 필드를 읽는 채널 프로그램을
 *                  띄우고 그 결과로 CDL/LDL 배치를 판별한다. 그래서
 *                  do_analysis 가 -EAGAIN 을 돌려주며 여러 번 불린다.
 *                  FBA 의 do_analysis 는 CCW 를 하나도 만들지 않고
 *                  **이미 읽어 둔 장치 특성만 보고 즉시 끝난다.**
 *   오류 복구      ECKD 는 dasd_3990_erp.c 의 센스 해석 상태 기계를 쓴다.
 *                  FBA 는 dasd_erp.c 의 기본 재시도만 쓴다.
 *   discard        **여기서만은 FBA 가 앞선다.** 이 디렉터리에서
 *                  has_discard 를 참으로 두는 디시플린은 FBA 하나뿐이다.
 *                  z/VM 이 FBA 볼륨을 페이지 단위로 뒷받침하기 때문에,
 *                  '데이터 없는 WRITE' 명령 하나로 페이지를 0 으로 되돌릴 수
 *                  있다는 특성을 이용한다.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 약어 ===
 * FBA   Fixed Block Architecture. 크기가 같은 블록이 번호순으로 늘어선 형식.
 * CCW   Channel Command Word. 채널이 실행하는 명령 하나. 사슬로 이어 쓴다.
 * CQR   이 드라이버가 struct dasd_ccw_req 를 부르는 이름. I/O 한 건이다.
 * DE    Define Extent. 구간과 권한을 정하는 명령(0x63).
 * LO    Locate Record. 구간 안의 위치와 개수를 지정하는 명령(0x43).
 * RDC   Read Device Characteristics. 장치가 스스로를 설명하게 하는 명령.
 * IRB   Interrupt Response Block. 완료 상태와 센스 데이터를 담아 오는 구조체.
 * ERP   Error Recovery Procedure. 실패한 cqr 을 복구용 cqr 로 감싸 다시 시도한다.
 * IDAL  Indirect Data Address List. CCW 가 직접 가리킬 수 없는 버퍼를
 *       주소 목록으로 우회해 가리키는 장치.
 * CC    Command Chaining. 앞 명령이 끝나면 다음 CCW 를 명령으로 실행한다.
 * DC    Data Chaining. 앞 명령을 이어 가되 데이터만 다음 버퍼로 옮긴다.
 * SLI   Suppress Length Indication. 전송 길이가 어긋나도 오류로 보지 않는다.
 * TOD   Time-Of-Day clock. s390 의 시스템 시계.
 */
/* [한국어] NULL, offsetof 같은 기본 정의를 위해 포함한다. 이 파일이 직접 쓰는
 * 것은 없지만 아래 헤더들이 기대하는 바탕이다. */
#include <linux/stddef.h>
/* [한국어] min() 매크로를 위해 포함한다. dasd_fba_dump_sense() 가 CCW 덤프 범위를
 * 자를 때 두 번 쓴다. */
#include <linux/kernel.h>
/* [한국어] s390 디버그 기능(debug feature)의 선언을 위해 포함한다. 이 파일이
 * DBF_DEV_EVENT 와 DBF_EVENT_DEVID 로 남기는 기록이 결국 이 계층의
 * 링버퍼로 들어간다. 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/debug.h>

/* [한국어] kzalloc_obj(), kfree(), kmem_cache_alloc(), kmem_cache_free() 를 위해
 * 포함한다. 사적 영역 할당과 고정 버퍼 모드의 바운스 버퍼가 이것을 쓴다. */
#include <linux/slab.h>
/* [한국어] struct hd_geometry 를 위해 포함한다. 옆의 상류 주석대로 HDIO_GETGEO
 * ioctl 이 그 구조체를 쓰며, dasd_fba_fill_geometry() 가 그것을 채운다. */
#include <linux/hdreg.h>	/* HDIO_GETGEO			    */
/* [한국어] struct bio_vec 와 세그먼트 훑기를 위해 포함한다. 두 build_cp 가
 * 요청의 세그먼트를 훑을 때 쓴다. */
#include <linux/bio.h>
/* [한국어] MODULE_DESCRIPTION, MODULE_LICENSE, THIS_MODULE, MODULE_DEVICE_TABLE 을
 * 위해 포함한다. 이 파일이 독립된 커널 모듈이기 때문이다. */
#include <linux/module.h>
/* [한국어] __init 과 __exit 표시를 위해 포함한다. 초기화·정리 함수를 부팅 뒤
 * 버릴 수 있는 구역에 놓게 해 준다. */
#include <linux/init.h>
/* [한국어] 가상 주소와 채널/DMA 주소를 오가는 변환들을 위해 포함한다.
 * virt_to_dma32(), dma32_to_virt(), dma64_to_virt(), dma32_to_u32() 가
 * 그것이며, s390 판 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <linux/io.h>

/* [한국어] IDAL(Indirect Data Address List) 처리를 위해 포함한다.
 * idal_is_needed() 와 idal_create_words() 가 여기서 온다. 채널이 직접
 * 가리킬 수 없는 버퍼를 주소 목록으로 우회하게 해 준다. */
#include <asm/idals.h>
/* [한국어] EBCDIC 변환을 위해 포함한다. dasd_fba_init() 이 디시플린 이름을
 * ASCEBC 로 EBCDIC 으로 바꿀 때 딱 한 번 쓴다. */
#include <asm/ebcdic.h>
/* [한국어] struct ccw_device, struct ccw_driver, struct ccw_device_id,
 * ccw_driver_register()/unregister(), CCW_DEVICE_DEVTYPE 매크로를 위해
 * 포함한다. 이 파일이 ccw 버스의 드라이버로 등록되기 때문이다. */
#include <asm/ccwdev.h>

/* [한국어] DASD 드라이버의 중앙 헤더. struct dasd_device, struct dasd_block,
 * struct dasd_ccw_req, struct dasd_discipline, DASD_FBA_MAGIC, 상태 상수,
 * 그리고 dasd.c 가 내보내는 공통 함수의 선언이 전부 여기서 온다.
 * **아래 dasd_fba.h 보다 먼저 포함해야 한다** — 그쪽은 #include 를 하나도
 * 갖지 않아 여기서 갖춰진 타입에 기대기 때문이다. */
#include "dasd_int.h"
/* [한국어] FBA 전용 전송 형식 헤더. struct DE_fba_data, struct LO_fba_data,
 * struct dasd_fba_characteristics, DASD_FBA_MAX_BLOCKS 가 여기서 온다.
 * **이 헤더를 포함하는 파일은 이 파일 하나뿐이다.** */
#include "dasd_fba.h"

/* [한국어] 요청 하나를 몇 번까지 다시 시도할지의 기본값. 장치를 올릴 때
 * device->default_retries 에 심어 두고, build_cp 가 그것을 cqr->retries 로
 * 옮긴다. 실패할 때마다 하나씩 줄어 음수가 되면 최종 실패다.
 * ECKD 도 같은 값 32 를 쓴다. 왜 32 인지의 근거는 이 트리에서 확인 못 함. */
#define FBA_DEFAULT_RETRIES 32

/* [한국어] FBA 의 **WRITE** CCW 명령 코드. 데이터 CCW 와 discard 용 CCW 가 모두
 * 이 값을 쓴다. dasd_eckd.h 의 ECKD 쪽 쓰기 명령이 0x05/0x85 인 것과 값이
 * 전혀 다르다 — 두 아키텍처의 명령 코드 체계가 따로이기 때문이다. */
#define DASD_FBA_CCW_WRITE 0x41
/* [한국어] FBA 의 **READ** CCW 명령 코드. 위 0x41 과 한 비트만 다르다(0x01 비트).
 * 읽기 데이터 CCW 에서만 쓰인다. */
#define DASD_FBA_CCW_READ 0x42
/* [한국어] FBA 의 **Locate Record** CCW 명령 코드. locate_record() 가 이 값을 넣는다.
 * 자료 블록은 struct LO_fba_data 8바이트다. */
#define DASD_FBA_CCW_LOCATE 0x43
/* [한국어] FBA 의 **Define Extent** CCW 명령 코드. define_extent() 가 이 값을 넣는다.
 * 자료 블록은 struct DE_fba_data 16바이트다.
 * **이 값 0x63 은 dasd_eckd.h 의 Define Extent 와 같다** — 같은 이름의 기능이
 * 두 아키텍처에서 같은 명령 코드를 쓴다. 위 세 값이 서로 다른 것과 대조적이며,
 * 이것이 우연인지 공통 규약인지는 이 트리에서 확인 못 함.
 * 
 * **FBA 의 CCW 명령 코드는 이 넷이 전부다.** dasd_eckd.h 가 39개를 정의하는
 * 것과 견주면 그대로 두 아키텍처의 복잡도 차이다. 포맷 명령도, 제어 장치에
 * 일을 시키는 명령도, 구성 데이터를 읽는 명령도 FBA 에는 없다. */
#define DASD_FBA_CCW_DEFINE_EXTENT 0x63

/* [한국어] modinfo 에 보일 모듈 설명. S/390 은 이 아키텍처의 옛 이름이다. */
MODULE_DESCRIPTION("S/390 DASD FBA Disks device driver");
/* [한국어] 모듈 라이선스. GPL 로 선언해야 dasd.c 가 EXPORT_SYMBOL_GPL 로 내보낸
 * 함수들(dasd_alloc_block, dasd_generic_read_dev_chars 등)을 쓸 수 있다. */
MODULE_LICENSE("GPL");

/* [한국어] **전방 선언.** 이 표의 정의는 파일 맨 끝에 있지만, 바로 아래
 * dasd_fba_set_online() 이 그 주소를 써야 하므로 여기서 이름만 먼저 알린다.
 * 정의를 끝에 두는 이유는 그 표가 이 파일의 거의 모든 함수를 가리켜야
 * 하기 때문이다. */
static struct dasd_discipline dasd_fba_discipline;
/* [한국어] **0 으로 채운 페이지 하나.** discard 경로의 ccw_write_zero() 가 데이터
 * 원본으로 쓴다.
 * 설정자: dasd_fba_init() 이 모듈 적재 때 한 번 잡고, dasd_fba_cleanup() 이
 * 반납한다.
 * 읽는 자: ccw_write_zero() 가 채널 주소로 바꿔 CCW 에 넣는다.
 * 값 범위: 유효한 페이지 주소, 또는 모듈이 올라오기 전 NULL.
 * 동기화: **한 번도 쓰이지 않고 읽히기만 하므로 잠금이 없다.** 모든 FBA
 * 장치의 모든 discard 요청이 이 한 페이지를 함께 가리켜도 안전하다. */
static void *dasd_fba_zero_page;

/* [한국어] **FBA 디시플린이 장치마다 들고 다니는 사적 상태 전부.**
 * device->private 가 이것을 가리킨다. 필드가 하나뿐이라는 점이 이 디시플린의
 * 단순함을 그대로 보여 준다 — dasd_eckd.h 의 struct dasd_eckd_private 은
 * 장치 특성 말고도 구성 데이터, PAV 별칭 상태, 전송 모드 상태, 포맷 배치
 * 정보를 수십 개 필드에 담는다. */
struct dasd_fba_private {
	/* [한국어] 장치가 RDC 로 올려 보낸 특성 32바이트.
	 * 설정자: dasd_fba_check_characteristics() 가 온라인 때 한 번 채운다.
	 * 실제로 바이트를 써 넣는 것은 **채널** 이므로 이 영역은 DMA 가능한
	 * 메모리여야 하고, 그래서 위 사적 영역을 GFP_DMA 로 잡는다.
	 * 읽는 자: dasd_fba_do_analysis()(블록 크기와 총 블록 수),
	 * dasd_fba_build_cp_regular() 와 dasd_fba_free_cp()(데이터 체이닝 비트),
	 * dasd_fba_fill_info()(통째로 사용자 공간에 복사).
	 * 값 범위: struct dasd_fba_characteristics 참고.
	 * 동기화: **온라인 때 한 번 채워지고 이후 읽기만 하므로 잠금이 없다.**
	 * 그래서 I/O 제출 경로에서 데이터 체이닝 비트를 잠금 없이 읽어도 안전하다. */
	struct dasd_fba_characteristics rdc_data;
};

/* [한국어] **이 드라이버가 맡을 장치의 목록.** ccw 버스가 새 장치를 발견하면
 * 이 표와 대조해 맞는 드라이버를 고른다. */
static struct ccw_device_id dasd_fba_ids[] = {
	/* [한국어] 제어 장치 형식 0x6310, 장치 형식 0x9336 — **9336 FBA DASD** 다.
	 * 모델은 둘 다 0 이며 '어느 모델이든' 을 뜻하는 것으로 보인다.
	 * CCW_DEVICE_DEVTYPE 매크로의 인자 순서와 의미는 arch/s390 소관이라
	 * 이 트리에서 확인 못 함.
	 * [상류 코드 관찰] .driver_info 에 0x1 을 넣지만 **이 트리 어디에서도 이
	 * 값을 읽지 않는다.** dasd_eckd.c 의 같은 표도 1~8 을 넣어 두고 읽지 않는다.
	 * 원본(1f0e418bb6) 45~46줄에서 확인했으며 코드는 고치지 않았다. */
	{ CCW_DEVICE_DEVTYPE (0x6310, 0, 0x9336, 0), .driver_info = 0x1},
	/* [한국어] 제어 장치 형식 0x3880, 장치 형식 0x3370 — **3370 FBA DASD** 다.
	 * 9336 보다 앞선 세대의 장치다. */
	{ CCW_DEVICE_DEVTYPE (0x3880, 0, 0x3370, 0), .driver_info = 0x2},
	/* [한국어] 목록의 끝을 0 으로 채운 항목으로 알린다. 옆의 상류 주석이 그 뜻을 밝힌다.
	 * 버스 코드가 이 항목을 만나면 대조를 멈춘다. */
	{ /* end of list */ },
};

/* [한국어] 모듈 자동 적재를 위해 위 표를 modules.alias 에 새겨 넣는다. 그래야
 * FBA 장치가 나타났을 때 udev 가 이 모듈을 자동으로 올릴 수 있다. */
MODULE_DEVICE_TABLE(ccw, dasd_fba_ids);

/* [한국어]
 * dasd_fba_set_online - 이 ccw 장치를 FBA 디시플린으로 온라인시킨다
 *
 * @cdev: ccw 버스가 넘겨주는 채널 장치. 아직 dasd_device 가 붙어 있지 않을 수 있다.
 * @return: dasd_generic_set_online() 의 반환값을 그대로 넘긴다. 0 이면 성공이고,
 *          음수 errno 면 온라인 실패로 sysfs 쓰기가 그 값을 사용자에게 돌려준다.
 *
 * **왜 필요한가**: 공통 함수 dasd_generic_set_online() 은 인자를 둘 받는다 —
 * 채널 장치와 '어떤 디시플린으로 다룰지' 다. 그런데 ccw 드라이버의
 * set_online 콜백은 채널 장치 하나만 받는 형태로 정해져 있다. 그 간극을
 * 메우려고 **디시플린 포인터를 붙여 주는 한 줄짜리 어댑터** 를 둔 것이다.
 * dasd_eckd.c 와 dasd_diag.c 에도 이름만 다른 같은 모양의 함수가 있다.
 *
 * 동작 과정은 한 단계뿐이다 — 아래 dasd_fba_discipline 의 주소를 함께 실어
 * 공통 함수를 부른다. 그 안에서 dasd_device 생성, 모듈 참조 잡기,
 * discipline->check_device 호출(즉 dasd_fba_check_characteristics),
 * 상태 기계를 DASD_STATE_ONLINE 까지 올리는 일이 차례로 일어난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. sysfs 의 online 속성에 1 을 쓰거나
 * 부팅 시 장치를 붙일 때 ccw 버스 코드가 부른다. 잠들 수 있다.
 *
 * caller: ccw 버스. 아래 dasd_fba_driver 의 .set_online 칸에 이 함수가 꽂혀 있다.
 * callee: dasd.c:3451 의 dasd_generic_set_online().
 *
 * 에러 경로: 이 함수 자체에는 에러 처리가 없다. 아래에서 나온 값을 그대로
 * 올려 보내며, 실패 시의 뒷정리(dasd_delete_device, 모듈 참조 반납)는
 * 전부 dasd_generic_set_online() 안에서 이루어진다.
 *
 * 호출 체인:
 *   ccw 버스의 온라인 처리 → [이 함수] → dasd_generic_set_online()
 *     → discipline->check_device == dasd_fba_check_characteristics()
 */
static int
dasd_fba_set_online(struct ccw_device *cdev)
{
	/* [한국어] 디시플린 포인터를 붙여 공통 함수에 넘긴다. 이 한 줄이 이 함수의 전부다. */
	return dasd_generic_set_online(cdev, &dasd_fba_discipline);
}

/* [한국어] **ccw 버스에 등록할 드라이버 서술자.** 여기서 지정하는 콜백 여섯 개 중
 * **다섯이 공통 구현이고 하나만 이 파일 것** 이다. 그만큼 FBA 디시플린이
 * 버스 계층에서 특별히 할 일이 없다는 뜻이다. */
static struct ccw_driver dasd_fba_driver = {
	/* [한국어] 리눅스 장치 모델 쪽 드라이버 서술자. */
	.driver = {
		/* [한국어] sysfs 에 보일 드라이버 이름. /sys/bus/ccw/drivers/dasd-fba 가 된다. */
		.name	= "dasd-fba",
		/* [한국어] 이 드라이버를 담은 모듈. 장치가 붙어 있는 동안 모듈이 내려가지 않게 한다. */
		.owner	= THIS_MODULE,
		/* [한국어] 장치마다 붙일 sysfs 속성 묶음. dasd_devmap.c:2481 에 정의된 공용
		 * 목록이며, readonly·failfast·erplog 같은 DASD 공통 속성이 전부 여기 있다.
		 * **세 디시플린이 같은 목록을 쓴다.** */
		.dev_groups = dasd_dev_groups,
	},
	/* [한국어] 위 장치 목록을 매단다. */
	.ids         = dasd_fba_ids,
	/* [한국어] 장치를 발견했을 때. dasd.c:3376 의 공통 구현이 dasd_device 를 만들고
	 * sysfs 항목을 붙인다. */
	.probe       = dasd_generic_probe,
	/* [한국어] 장치를 뗄 때. dasd.c:3412 의 공통 구현. */
	.remove      = dasd_generic_remove,
	/* [한국어] 장치를 오프라인으로 내릴 때. dasd.c:3529 의 공통 구현. */
	.set_offline = dasd_generic_set_offline,
	/* [한국어] **이 표에서 유일하게 이 파일 것인 칸.** 위 어댑터 함수가 여기 꽂힌다.
	 * 디시플린 포인터를 붙여야 해서 공통 구현을 그대로 쓸 수 없다. */
	.set_online  = dasd_fba_set_online,
	/* [한국어] 경로가 죽거나 살아났다는 통보를 받을 때. dasd.c:3694 의 공통 구현. */
	.notify      = dasd_generic_notify,
	/* [한국어] 경로 상태 변화 이벤트를 받을 때. dasd.c:3721 의 공통 구현이며,
	 * 그 안에서 discipline->pe_handler 로 이 파일의 함수를 부른다. */
	.path_event  = dasd_generic_path_event,
	/* [한국어] 이 드라이버의 인터럽트를 /proc/interrupts 에서 어느 항목으로 셀지 정한다.
	 * DAS 는 DASD 를 뜻하며, ECKD 와 DIAG 도 같은 항목을 쓴다.
	 * 상수의 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
	.int_class   = IRQIO_DAS,
};

/* [한국어]
 * define_extent - Define Extent CCW 하나와 그 자료 16바이트를 채운다
 *
 * @ccw: 채울 CCW 자리. 호출자가 cqr->cpaddr 배열에서 떼어 준 칸이며,
 *       dasd_smalloc_request() 가 이미 0 으로 지워 둔 상태다.
 * @data: 채울 struct DE_fba_data 자리. cqr->data 버퍼의 맨 앞 16바이트다.
 * @rw: 접근 방향. 호출자가 rq_data_dir(req) 또는 리터럴 WRITE 를 넘긴다.
 * @blksize: 블록 하나의 바이트 수. 두 호출자 모두 block->bp_block 을 넘긴다.
 * @beg: 익스텐트의 시작 위치.
 * @nr: 익스텐트에 들어갈 블록의 **개수**. 끝 번호가 아니라 개수다.
 * @return: 없다. ccw 와 data 두 버퍼를 채우는 것이 전부다.
 *
 * **왜 필요한가**: FBA 채널 프로그램은 언제나 Define Extent 로 시작한다.
 * 그 CCW 가 '이번 명령 사슬이 건드릴 수 있는 블록 구간과 권한' 을 제어
 * 장치에 미리 알리며, 뒤따르는 명령이 그 밖을 건드리면 하드웨어가 거부한다.
 * 두 build_cp 경로가 똑같이 필요한 일이라 함수로 떼어 두었다.
 *
 * 동작 과정:
 *   (1) CCW 의 명령 코드를 0x63 으로 놓는다.
 *   (2) CCW 의 플래그를 0 으로 놓는다 — 체이닝 여부는 호출자가 나중에
 *       정하며, 실제로 두 호출자 모두 이 CCW 를 만든 직후에
 *       `ccw[-1].flags |= CCW_FLAG_CC` 로 다음 명령과 잇는다.
 *   (3) CCW 의 전송 길이를 16 으로 놓는다. struct DE_fba_data 의 크기와 같다.
 *   (4) CCW 의 데이터 주소에 자료 버퍼의 채널 주소를 넣는다.
 *   (5) 자료 버퍼를 0 으로 지운 뒤 네 필드만 채운다 — 권한, 블록 크기,
 *       시작 위치, 끝 번호(개수 - 1)다. 나머지 필드는 0 으로 나간다.
 *
 * 실행 컨텍스트: I/O 제출 경로. do_dasd_request() 가 dq->lock 을 쥔 채
 * build_cp 를 부르므로 이 함수도 그 잠금 아래에서 실행되며 잠들 수 없다.
 * 재진입은 문제가 되지 않는다 — 다루는 버퍼가 전부 이번 요청 전용이다.
 *
 * caller: 이 파일의 dasd_fba_build_cp_discard() 와 dasd_fba_build_cp_regular().
 * callee: memset(), 그리고 arch/s390 의 주소 변환 하나. 후자는 이 트리에 없다.
 *
 * 에러 경로: 없다. 인자 검증도 하지 않는다 — 구간이 볼륨을 벗어나는지 같은
 * 검사는 상위(블록 계층의 큐 한계와 제어 장치)가 맡는다.
 *
 * 호출 체인:
 *   do_dasd_request() → dasd_fba_build_cp() → build_cp 의 두 갈래 → [이 함수]
 */
static void
define_extent(struct ccw1 * ccw, struct DE_fba_data *data, int rw,
	      int blksize, int beg, int nr)
{
	/* [한국어] CCW 의 명령 코드를 Define Extent(0x63)로 놓는다. */
	ccw->cmd_code = DASD_FBA_CCW_DEFINE_EXTENT;
	/* [한국어] 플래그를 0 으로 놓는다. **체이닝은 여기서 정하지 않는다** — 호출자가
	 * 이 CCW 를 만든 직후 `ccw[-1].flags |= CCW_FLAG_CC` 로 얹는다. */
	ccw->flags = 0;
	/* [한국어] 전송 길이를 **16** 으로 놓는다. sizeof 를 쓰지 않고 숫자를 직접 넣었지만
	 * struct DE_fba_data 의 크기가 정확히 16 이라 같은 값이다. 이 길이만큼을
	 * 채널이 아래 자료 버퍼에서 읽어 제어 장치로 보낸다. */
	ccw->count = 16;
	/* [한국어] CCW 의 데이터 주소에 자료 버퍼의 채널 주소를 넣는다. 커널 가상 주소를
	 * 채널이 이해하는 32비트 주소로 바꾸는 변환이며, 정의는 arch/s390 소관이라
	 * 이 트리에서 확인 못 함. */
	ccw->cda = virt_to_dma32(data);
	/* [한국어] 자료 버퍼 16바이트를 통째로 0 으로 지운다. **아래에서 채우지 않는 필드
	 * (mask 의 zero/da/diag/zero2, 바이트 1 의 zero, ext_beg)가 전부 이 한 줄
	 * 덕분에 0 으로 나간다.** cqr 을 정적 풀에서 떼어 올 때도 0 으로 지워지지만,
	 * 이 함수는 그것에 기대지 않고 스스로 지운다. */
	memset(data, 0, sizeof (struct DE_fba_data));
	/* [한국어] 쓰기 방향인지 본다. */
	if (rw == WRITE)
		/* [한국어] 쓰기면 권한을 **0x0** 으로 놓는다. dasd_eckd.h 의 같은 이름 필드에서
		 * 0x2 가 쓰기인 것과 다르며, FBA 에서 0x0 이 정확히 무엇을 뜻하는지는
		 * 아키텍처 문서 소관이라 이 트리에서 확인 못 함. */
		(data->mask).perm = 0x0;
	/* [한국어] 읽기 방향인지 본다. */
	else if (rw == READ)
		/* [한국어] 읽기면 권한을 0x1 로 놓는다. */
		(data->mask).perm = 0x1;
	else
		/* [한국어] 나머지 방향이면 권한을 0x2 로 놓는다.
		 * [상류 코드 관찰] **닿을 수 없는 갈래다.** 두 호출자가 넘기는 rw 는
		 * rq_data_dir() 의 결과이거나 리터럴 WRITE 인데, rq_data_dir() 은
		 * include/linux/blk-mq.h:246 의 정의상 WRITE 아니면 READ 만 돌려준다.
		 * 원본(1f0e418bb6) 83~88줄과 353, 487줄에서 확인했으며 코드는 고치지 않았다. */
		data->mask.perm = 0x2;
	/* [한국어] 블록 크기를 넣는다. 제어 장치가 이 값으로 블록 번호를 바이트 위치로 환산한다. */
	data->blk_size = blksize;
	/* [한국어] 익스텐트의 시작 위치를 넣는다. 호출자가 준 값을 그대로 쓴다. */
	data->ext_loc = beg;
	/* [한국어] 익스텐트의 마지막 블록 번호를 넣는다. 인자가 '개수' 이므로 **1 을 빼야**
	 * 포함 구간의 끝 번호가 된다. 이 -1 이 이 함수에서 유일한 산술이다. */
	data->ext_end = nr - 1;
}

/* [한국어]
 * locate_record - Locate Record CCW 하나와 그 자료 8바이트를 채운다
 *
 * @ccw: 채울 CCW 자리. 호출자가 cqr->cpaddr 배열에서 떼어 준 칸이다.
 * @data: 채울 struct LO_fba_data 자리. Define Extent 자료 바로 뒤에 놓인다.
 * @rw: 접근 방향. 호출자가 rq_data_dir(req) 또는 리터럴 WRITE 를 넘긴다.
 * @block_nr: 시작 블록 번호. **절대 번호가 아니라 위 Define Extent 가 정한
 *            구간의 시작을 0 으로 세는 상대 번호** 다.
 * @block_ct: 이번 Locate Record 가 다룰 블록 개수.
 * @return: 없다. ccw 와 data 두 버퍼를 채우는 것이 전부다.
 *
 * **왜 필요한가**: Define Extent 는 구간만 정할 뿐 어디서 시작해 몇 개를
 * 다룰지는 말하지 않는다. 그것을 정하는 것이 Locate Record 이며, 뒤따르는
 * 읽기/쓰기 CCW 는 자기 위치를 스스로 말하지 않고 이 명령이 잡아 준 자리에서
 * 이어 간다. 그래서 데이터 CCW 앞에는 언제나 이 명령이 하나 있어야 한다.
 *
 * 한 요청에 이 함수가 **몇 번 불리는지는 장치에 달렸다.** 데이터 체이닝을
 * 지원하는 장치면 요청당 한 번, 지원하지 않으면 블록마다 한 번이다.
 * discard 경로에서는 구간이 셋으로 갈릴 수 있어 최대 세 번 불린다.
 *
 * 동작 과정:
 *   (1) CCW 의 명령 코드를 0x43 으로 놓는다.
 *   (2) CCW 의 플래그를 0 으로 놓는다. 체이닝은 호출자가 나중에 얹는다.
 *   (3) CCW 의 전송 길이를 8 로 놓는다. struct LO_fba_data 의 크기와 같다.
 *   (4) CCW 의 데이터 주소에 자료 버퍼의 채널 주소를 넣는다.
 *   (5) 자료 버퍼를 0 으로 지운 뒤 세 값만 채운다 — 동작 코드, 시작 번호,
 *       개수다. auxiliary 와 operation 의 상위 니블은 0 으로 나간다.
 *
 * 실행 컨텍스트: 위 define_extent() 와 같다. I/O 제출 경로, 잠들 수 없다.
 *
 * caller: 이 파일의 dasd_fba_build_cp_discard() 와 dasd_fba_build_cp_regular().
 * callee: memset(), 그리고 arch/s390 의 주소 변환 하나.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   do_dasd_request() → dasd_fba_build_cp() → build_cp 의 두 갈래 → [이 함수]
 */
static void
locate_record(struct ccw1 * ccw, struct LO_fba_data *data, int rw,
	      int block_nr, int block_ct)
{
	/* [한국어] CCW 의 명령 코드를 Locate Record(0x43)로 놓는다. */
	ccw->cmd_code = DASD_FBA_CCW_LOCATE;
	/* [한국어] 플래그를 0 으로 놓는다. 체이닝은 호출자가 나중에 얹는다.
	 * **다만 build_cp_regular 의 한 갈래는 이 함수가 돌아온 직후 플래그를
	 * CCW_FLAG_CC 로 다시 세운다** — 그쪽은 Locate Record 뒤에 곧바로 데이터
	 * CCW 가 이어져야 하기 때문이다. */
	ccw->flags = 0;
	/* [한국어] 전송 길이를 **8** 로 놓는다. struct LO_fba_data 의 크기와 같다. */
	ccw->count = 8;
	/* [한국어] CCW 의 데이터 주소에 자료 버퍼의 채널 주소를 넣는다. */
	ccw->cda = virt_to_dma32(data);
	/* [한국어] 자료 버퍼 8바이트를 0 으로 지운다. **아래에서 채우지 않는 필드
	 * (operation 의 상위 니블, auxiliary)가 이 덕분에 0 으로 나간다.** */
	memset(data, 0, sizeof (struct LO_fba_data));
	/* [한국어] 쓰기 방향인지 본다. */
	if (rw == WRITE)
		/* [한국어] 쓰기면 동작 코드를 0x5 로 놓는다. dasd_eckd.h 의 Write Data CCW
		 * 명령 코드(0x05)와 값이 같다. */
		data->operation.cmd = 0x5;
	/* [한국어] 읽기 방향인지 본다. */
	else if (rw == READ)
		/* [한국어] 읽기면 동작 코드를 0x6 으로 놓는다. dasd_eckd.h 의 Read Data CCW
		 * 명령 코드(0x06)와 값이 같다. */
		data->operation.cmd = 0x6;
	else
		/* [한국어] 나머지 방향이면 동작 코드를 0x8 로 놓는다.
		 * [상류 코드 관찰] 위 define_extent() 와 같은 이유로 **닿을 수 없는
		 * 갈래다.** 원본(1f0e418bb6) 103~108줄에서 확인했으며 코드는 고치지 않았다. */
		data->operation.cmd = 0x8;
	/* [한국어] 시작 블록 번호를 넣는다. **익스텐트의 처음을 0 으로 세는 상대 번호** 다. */
	data->blk_nr = block_nr;
	/* [한국어] 블록 개수를 넣는다. **선언 순서상 이 필드가 위 blk_nr 보다 앞에
	 * 놓이지만**(오프셋 2 대 4), 대입 순서는 바이트 배치와 무관하다. */
	data->blk_ct = block_ct;
}

/* [한국어]
 * dasd_fba_check_characteristics - 장치를 알아보고 FBA 로 다룰 준비를 갖춘다
 *
 * @device: 이제 막 디시플린이 정해진 장치. private 와 block 이 아직 없을 수 있다.
 * @return: 0 이면 이 장치를 FBA 로 다룰 수 있다는 뜻이다. 음수 errno 면
 *          온라인이 중단되고 dasd_generic_set_online() 이 dasd_delete_device()
 *          로 장치를 지운다.
 *
 * **왜 필요한가**: 디시플린 vtable 의 check_device 칸에 꽂히는 함수이며,
 * 장치를 온라인으로 올리는 길에서 **디시플린이 처음으로 개입하는 지점** 이다.
 * 여기서 세 가지를 마련한다 — 디시플린 전용 사적 영역, 블록 장치 구조체,
 * 그리고 장치가 스스로를 설명한 32바이트다. 이 셋이 없으면 뒤따르는 볼륨
 * 분석도 I/O 도 시작할 수 없다.
 *
 * 동작 과정:
 *   (1) private 영역이 없으면 DMA 가능한 메모리로 새로 잡고, 있으면 0 으로
 *       지워 재사용한다(같은 장치를 내렸다 다시 올리는 경우).
 *   (2) struct dasd_block 을 잡아 device 와 서로 가리키게 잇는다. 이것이
 *       있어야 나중에 gendisk 와 blk-mq 큐가 붙는다.
 *   (3) RDC(Read Device Characteristics)로 장치 특성 32바이트를 읽어
 *       private->rdc_data 에 담는다. 이 호출은 **동기적** 이다 — 안에서
 *       CCW 요청을 만들어 내보내고 완료를 기다린다.
 *   (4) 기본 만료 시간(300초)과 기본 재시도 횟수(32)를 장치에 심는다.
 *   (5) 모든 경로를 쓸 수 있다고 표시한다.
 *   (6) z/VM 이 이 볼륨을 읽기 전용으로 붙였는지 확인해 플래그를 세운다.
 *   (7) discard 기능 비트를 켠다. **이 디렉터리에서 이 줄이 있는 디시플린은
 *       FBA 하나뿐이다.**
 *   (8) 사람이 읽을 온라인 메시지를 커널 로그에 찍는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 로 메모리를 잡고 (3)에서
 * 완료를 기다리며 잠들 수 있다. 장치 하나에 대해 직렬로만 불린다.
 *
 * caller: dasd.c:3502 가 discipline->check_device 로 부른다. 그 위는
 * dasd_generic_set_online(), 그 위는 이 파일의 dasd_fba_set_online() 이다.
 * callee: kzalloc_obj(), dasd.c:149 의 dasd_alloc_block(), dasd.c:3962 의
 * dasd_generic_read_dev_chars(), dasd_int.h 의 dasd_path_set_opm(),
 * dasd.c:3339 의 dasd_device_is_ro(), dasd_devmap.c:2509 의 dasd_set_feature().
 *
 * 에러 경로: 두 곳에서 실패할 수 있고 **둘 다 되돌리기를 손으로 한다.**
 * 메모리 부족이면 -ENOMEM 을 돌려주고, dasd_block 할당이 실패하면 private 를
 * 풀고 PTR_ERR 을 돌려주며, 장치 특성 읽기가 실패하면 block 과 private 를
 * 모두 풀고 그 오류를 그대로 돌려준다. goto 로 모으지 않고 각 자리에서
 * 같은 정리를 되풀이하는 방식이다.
 *
 * 호출 체인:
 *   dasd_fba_set_online() → dasd_generic_set_online()
 *     → discipline->check_device == [이 함수]
 *     → dasd_alloc_block() / dasd_generic_read_dev_chars() / dasd_set_feature()
 */
static int
dasd_fba_check_characteristics(struct dasd_device *device)
{
	/* [한국어] 디시플린 전용 사적 영역. 이 장치를 처음 올리는 것이면 아직 NULL 이고,
	 * 내렸다 다시 올리는 것이면 이전 것이 남아 있다. */
	struct dasd_fba_private *private = device->private;
	/* [한국어] 채널 장치. 아래에서 디버그 기록의 장치 식별자와 온라인 메시지의
	 * 장치 형식 번호를 꺼내는 데 쓴다. */
	struct ccw_device *cdev = device->cdev;
	/* [한국어] 새로 잡을 블록 장치 구조체. */
	struct dasd_block *block;
	/* [한국어] readonly 는 z/VM 이 이 볼륨을 읽기 전용으로 붙였는지, rc 는 아래
	 * 호출들의 결과다. */
	int readonly, rc;

	/* [한국어] 사적 영역이 아직 없으면 새로 잡는다. */
	if (!private) {
		/* [한국어] 0 으로 채워 잡는다. **GFP_DMA 를 붙이는 이유** 는 이 영역 안의
		 * rdc_data 가 RDC CCW 의 데이터 목적지가 되어 채널이 직접 써 넣기 때문이다.
		 * 채널이 닿을 수 있는 영역이 어디까지인지는 arch/s390 소관이라 이 트리에서
		 * 확인 못 함. kzalloc_obj 는 대상 객체의 크기를 타입에서 뽑아 주는 매크로다. */
		private = kzalloc_obj(*private, GFP_KERNEL | GFP_DMA);
		/* [한국어] 메모리가 모자라면 이 장치를 올릴 수 없다. */
		if (!private) {
			/* [한국어] 사용자에게 보이는 경고를 커널 로그에 남긴다. */
			dev_warn(&device->cdev->dev,
				 "Allocating memory for private DASD "
				 "data failed\n");
			/* [한국어] 온라인 실패를 알린다. 위 dasd_generic_set_online() 이 이 값을 받아
			 * 장치를 지운다. */
			return -ENOMEM;
		}
		/* [한국어] 장치에 사적 영역을 매단다. 이제부터 이 디시플린의 다른 함수들이
		 * device->private 로 여기에 닿는다. */
		device->private = private;
	} else {
		/* [한국어] 이미 있으면 새로 잡지 않고 **0 으로 지워 재사용** 한다. 장치를 내렸다
		 * 다시 올릴 때 이전 장치 특성이 남아 있으면 안 되기 때문이다. */
		memset(private, 0, sizeof(*private));
	}
	/* [한국어] 블록 장치 구조체를 잡는다. dasd.c:149 의 이 함수가 blk-mq 큐가 붙을
	 * 자리와 두 잠금, tasklet, 타이머를 초기화한다. **이것이 있어야 이 장치가
	 * 사용자에게 보이는 디스크가 된다** — PAV 별칭 장치는 이것을 갖지 않는다. */
	block = dasd_alloc_block();
	/* [한국어] 실패했는지 본다. 오류 포인터로 실패를 알리는 형태다. */
	if (IS_ERR(block)) {
		/* [한국어] 디버그 링버퍼에 남긴다. DBF_EVENT_DEVID 는 장치 식별자("0.0.1234")를
		 * 앞에 붙여 주는 판이라, 장치별 영역이 아닌 공용 영역에 남길 때 쓴다. */
		DBF_EVENT_DEVID(DBF_WARNING, cdev, "%s", "could not allocate "
				"dasd block structure");
		/* [한국어] 장치에 매달아 둔 사적 영역을 떼어 낸다. **먼저 떼고 나서 풀어야**
		 * 그 사이에 다른 코드가 죽은 포인터를 보지 않는다. */
		device->private = NULL;
		/* [한국어] 사적 영역을 반납한다. */
		kfree(private);
		/* [한국어] 오류 포인터에서 오류 값을 꺼내 올려 보낸다. */
		return PTR_ERR(block);
	}
	/* [한국어] 장치에 블록 장치를 매단다. */
	device->block = block;
	/* [한국어] 블록 장치에서 장치로 되돌아오는 고리를 잇는다. 둘은 서로를 가리키는
	 * 짝이며, 이후 코드가 block->base 로 장치에 닿는다. */
	block->base = device;

	/* Read Device Characteristics */
	/* [한국어] **RDC(Read Device Characteristics)로 장치 특성을 읽는다.** 옆의 상류
	 * 주석이 그것을 밝힌다. dasd.c:3962 의 이 함수가 안에서 CCW 하나짜리 채널
	 * 프로그램을 만들어 내보내고 **완료를 기다린다** — 즉 이 줄에서 잠든다.
	 * 둘째 인자 DASD_FBA_MAGIC 이 그 요청의 magic 이 되어, 완료 인터럽트가
	 * 이 디시플린 것인지 확인하는 데 쓰인다.
	 * 목적지는 사적 영역 안의 특성 구조체이고 길이는 **32** 다. 그 값이
	 * struct dasd_fba_characteristics 의 크기와 정확히 같다. */
	rc = dasd_generic_read_dev_chars(device, DASD_FBA_MAGIC,
					 &private->rdc_data, 32);
	/* [한국어] 읽기가 실패했으면 이 장치를 FBA 로 다룰 수 없다. 애초에 FBA 장치가
	 * 아니었거나, 경로가 죽었거나, 시간이 만료된 경우다. */
	if (rc) {
		/* [한국어] 실패 사유를 디버그 링버퍼에 남긴다. */
		DBF_EVENT_DEVID(DBF_WARNING, cdev, "Read device "
				"characteristics returned error %d", rc);
		/* [한국어] 장치에 매달아 둔 블록 장치를 떼어 낸다. */
		device->block = NULL;
		/* [한국어] 블록 장치를 반납한다. dasd.c:176 의 이 함수는 kfree 한 줄이다. */
		dasd_free_block(block);
		/* [한국어] 사적 영역도 떼어 낸다. */
		device->private = NULL;
		/* [한국어] 사적 영역을 반납한다. **되돌리기를 goto 로 모으지 않고 각 자리에서
		 * 되풀이하는** 방식이라, 위 블록 할당 실패 경로와 코드가 겹친다. */
		kfree(private);
		/* [한국어] 읽기 실패 사유를 그대로 올려 보낸다. */
		return rc;
	}

	/* [한국어] 요청 하나가 살아 있을 수 있는 시간의 기본값을 심는다.
	 * dasd_int.h:717 의 DASD_EXPIRES 는 300(초)이며, build_cp 가 이 값을
	 * 지프로 바꿔 cqr->expires 에 넣는다. */
	device->default_expires = DASD_EXPIRES;
	/* [한국어] 재시도 횟수의 기본값을 심는다. 이 파일 앞머리의 FBA_DEFAULT_RETRIES
	 * 는 32 다. ECKD 도 같은 값을 쓴다. */
	device->default_retries = FBA_DEFAULT_RETRIES;
	/* [한국어] **경로 8개를 모두 사용 가능으로 표시한다.** LPM_ANYPATH 는 여덟 비트가
	 * 모두 선 마스크다(정의는 arch/s390 소관이라 이 트리에서 확인 못 함).
	 * ECKD 는 여기서 경로 그룹을 설정하고 경로마다 검증을 거치지만,
	 * **FBA 는 검증 없이 전부 쓸 수 있다고 선언한다.** */
	dasd_path_set_opm(device, LPM_ANYPATH);

	/* [한국어] z/VM 이 이 볼륨을 읽기 전용으로 붙였는지 묻는다. dasd.c:3339 의
	 * 이 함수는 z/VM 위에서만 뜻이 있고(그 밖에서는 언제나 0), 진단 명령으로
	 * 가상 장치 정보를 읽어 플래그 한 비트를 본다. */
	readonly = dasd_device_is_ro(device);
	/* [한국어] 읽기 전용이면 표시를 남긴다. */
	if (readonly)
		/* [한국어] 장치 플래그에 비트를 세운다. dasd_int.h:2165 의 이 비트는 **장치 자체가**
		 * 읽기 전용이라는 뜻이며, 사용자가 sysfs 로 설정하는 readonly 속성과는 다르다.
		 * 그래서 옆의 상류 주석이 그 둘을 구분하라고 적어 두었다. */
		set_bit(DASD_FLAG_DEVICE_RO, &device->flags);

	/* FBA supports discard, set the according feature bit */
	/* [한국어] **discard 지원을 켠다.** 옆의 상류 주석이 그 뜻을 밝힌다.
	 * dasd_devmap.c:2509 의 이 함수가 장치 매핑의 기능 비트를 켜고
	 * device->features 에도 옮긴다. 이것과 아래 vtable 의 has_discard 가
	 * **둘 다 있어야** 블록 계층이 discard 요청을 보내 준다 — 전자는 기능
	 * 비트, 후자는 큐 한계 설정 여부다. */
	dasd_set_feature(cdev, DASD_FEATURE_DISCARD, 1);

	/* [한국어] 사람이 읽을 온라인 메시지를 커널 로그에 남긴다. 사용자가 장치를 올린
	 * 직후 dmesg 에서 보는 줄이며, 인자는 차례로 장치 형식·모델, 제어 장치
	 * 형식·모델, 용량(MB), 블록 크기, 그리고 읽기 전용 꼬리말이다.
	 * 장치 형식 번호에는 위 dasd_fba_ids 표에 적힌 0x9336 이나 0x3370 이 찍히고,
	 * 제어 장치 형식 번호에는 0x6310 이나 0x3880 이 찍힌다.
	 * **용량을 MB 로 환산하는 산식** 이 눈여겨볼 만하다 — 블록 크기를 9비트
	 * 오른쪽으로 밀어 '블록 하나가 몇 개의 512바이트 섹터인가' 를 얻고,
	 * 총 블록 수에 곱해 전체 512바이트 섹터 수를 만든 뒤, 다시 11비트
	 * 오른쪽으로 밀어 MB 로 바꾼다. 512 × 2048 = 1048576 이기 때문이며,
	 * 두 값이 모두 2의 거듭제곱이라 나눗셈 없이 시프트만으로 끝난다.
	 * 마지막 인자는 읽기 전용이면 꼬리말을, 아니면 빈 문자열을 붙인다. */
	dev_info(&device->cdev->dev,
		 "New FBA DASD %04X/%02X (CU %04X/%02X) with %d MB "
		 "and %d B/blk%s\n",
		 cdev->id.dev_type,
		 cdev->id.dev_model,
		 cdev->id.cu_type,
		 cdev->id.cu_model,
		 ((private->rdc_data.blk_bdsa *
		   (private->rdc_data.blk_size >> 9)) >> 11),
		 private->rdc_data.blk_size,
		 readonly ? ", read-only device" : "");
	return 0;
}

/* [한국어]
 * dasd_fba_do_analysis - 볼륨을 분석해 블록 크기와 용량을 정한다
 *
 * @block: 분석할 블록 장치. base 를 통해 장치와 private 에 닿는다.
 * @return: 0 이면 볼륨을 쓸 수 있다는 뜻이고, dasd_check_blocksize() 가
 *          돌려준 -EMEDIUMTYPE 이면 다룰 수 없는 매체라는 뜻이다.
 *          후자면 dasd.c:314~316 이 장치를 DASD_STATE_UNFMT 로 두고
 *          사용자 공간에 uevent 를 올린다.
 *
 * **왜 필요한가**: 디시플린 vtable 의 do_analysis 칸에 꽂히는 함수이며,
 * 장치 상태를 basic 에서 ready 로 올릴 때 불린다. 블록 장치의 용량과
 * 블록 크기가 여기서 정해져야 gendisk 에 용량을 심고 파티션을 훑을 수 있다.
 *
 * **이 함수가 이 파일에서 가장 상징적인 함수** 다. 본문이 열 줄이고 CCW 를
 * 하나도 만들지 않는다. 이미 온라인 단계에서 읽어 둔 장치 특성만 보면
 * FBA 볼륨에 대해 알아야 할 것이 전부 나오기 때문이다. ECKD 는 같은 자리에서
 * 트랙 0 과 1 의 카운트 필드를 읽는 채널 프로그램을 띄우고, 그 완료를 기다리려
 * -EAGAIN 을 돌려주며 여러 번 불린다.
 *
 * 동작 과정:
 *   (1) 장치가 말한 블록 크기가 512/1024/2048/4096 중 하나인지 검사한다.
 *   (2) 총 블록 수를 블록 장치의 용량으로 삼는다.
 *   (3) 블록 크기를 블록 장치의 블록 크기로 삼는다.
 *   (4) 512 에서 시작해 블록 크기에 이를 때까지 두 배씩 올리며 몇 번
 *       올렸는지를 센다. 그 횟수가 s2b_shift 이며, 이후 블록 번호와
 *       512바이트 섹터 번호를 오갈 때 나눗셈 대신 쓰는 시프트 폭이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 상태 기계가 부른다. 잠들 수 있지만
 * 실제로 잠드는 일은 하지 않는다.
 *
 * caller: dasd.c:310 이 discipline->do_analysis 로 부른다. 그 위는
 * dasd.c:297 의 dasd_state_basic_to_ready() 다.
 * callee: dasd_int.h 의 dasd_check_blocksize().
 *
 * 에러 경로: 블록 크기가 유효하지 않으면 디버그 로그에 남기고 그 오류를
 * 그대로 돌려준다. 그러면 장치는 '포맷되지 않음' 상태로 남고, 사용자는
 * 포맷 도구로 손을 볼 수 있다.
 *
 * 호출 체인:
 *   dasd_state_basic_to_ready() → discipline->do_analysis == [이 함수]
 *     → dasd_check_blocksize()
 */
static int dasd_fba_do_analysis(struct dasd_block *block)
{
	/* [한국어] 장치 특성을 꺼내려고 사적 상태를 잡는다. block->base 로 블록 장치에서
	 * 장치로 거슬러 올라간다. */
	struct dasd_fba_private *private = block->base->private;
	/* [한국어] sb 는 아래 루프에서 512부터 두 배씩 올릴 값, rc 는 블록 크기 검사 결과다. */
	int sb, rc;

	/* [한국어] **장치가 말한 블록 크기가 512/1024/2048/4096 중 하나인지 검사한다.**
	 * dasd_int.h:2530 의 이 함수는 범위 검사와 2의 거듭제곱 검사를 합쳐
	 * 그 넷만 통과시키고, 어긋나면 -EMEDIUMTYPE 을 돌려준다. */
	rc = dasd_check_blocksize(private->rdc_data.blk_size);
	/* [한국어] 유효하지 않으면 이 볼륨을 쓸 수 없다. */
	if (rc) {
		/* [한국어] 어떤 값이 왔는지 디버그 링버퍼에 남긴다. DBF_DEV_EVENT 는 장치별
		 * 디버그 영역에 쓰는 판이다. */
		DBF_DEV_EVENT(DBF_WARNING, block->base, "unknown blocksize %d",
			    private->rdc_data.blk_size);
		/* [한국어] -EMEDIUMTYPE 을 그대로 올린다. 호출자 dasd.c:311~317 이 그것을 보고
		 * 장치를 DASD_STATE_UNFMT('포맷되지 않음')로 두고 사용자 공간에 uevent 를
		 * 올린다. 그러면 관리자가 포맷 도구로 손볼 수 있다. */
		return rc;
	}
	/* [한국어] **볼륨의 총 블록 수를 블록 장치의 용량으로 삼는다.** 이 값이 나중에
	 * dasd.c:346 에서 s2b_shift 만큼 밀려 set_capacity() 로 들어간다. */
	block->blocks = private->rdc_data.blk_bdsa;
	/* [한국어] 블록 크기를 블록 장치의 논리 블록 크기로 삼는다. dasd.c:323 이 이 값을
	 * 큐 한계의 logical_block_size 로 쓴다. */
	block->bp_block = private->rdc_data.blk_size;
	/* [한국어] 시프트 폭을 0 에서 시작한다. 옆의 상류 주석이 '512 를 블록으로 만드는
	 * 시프트 비트 수' 라고 밝힌다. */
	block->s2b_shift = 0;	/* bits to shift 512 to get a block */
	/* [한국어] 512 에서 시작해 블록 크기에 이를 때까지 두 배씩 올린다.
	 * 블록 크기가 512 면 루프가 한 번도 돌지 않아 시프트 폭은 0 이고,
	 * 4096 이면 세 번 돌아 3 이 된다. */
	for (sb = 512; sb < private->rdc_data.blk_size; sb = sb << 1)
		/* [한국어] 한 번 돌 때마다 시프트 폭을 하나 올린다. 결국 log2(블록 크기 / 512)다.
		 * **로그 함수 대신 루프를 쓰는 이유** 는 이 계산이 장치를 올릴 때 한 번만
		 * 일어나 성능이 문제되지 않고, 값의 범위가 0~3 으로 아주 작기 때문이다. */
		block->s2b_shift++;
	return 0;
}

/* [한국어]
 * dasd_fba_fill_geometry - 사용자 공간에 알릴 디스크 지오메트리를 지어낸다
 *
 * @block: 대상 블록 장치. 용량(blocks)과 시프트 폭(s2b_shift)을 읽는다.
 * @geo: 채울 struct hd_geometry. 사용자가 HDIO_GETGEO ioctl 로 요청한 버퍼다.
 * @return: 0 이면 채웠다는 뜻이고, -EINVAL 이면 블록 크기가 유효하지 않아
 *          채울 수 없다는 뜻이다.
 *
 * **왜 필요한가**: 옛 파티션 도구들이 디스크를 실린더/헤드/섹터로 이해하던
 * 시절의 인터페이스를 유지하기 위해서다. FBA 볼륨에는 실린더도 헤드도 없고
 * 번호가 매겨진 블록만 있으므로, 이 함수가 하는 일은 **있지도 않은 기하 구조를
 * 그럴듯한 숫자로 만들어 내는 것** 이다.
 *
 * 동작 과정:
 *   (1) 블록 크기가 유효한지 먼저 확인한다. 볼륨 분석 전이거나 분석이
 *       실패한 장치를 걸러 내기 위해서다.
 *   (2) 실린더 수는 볼륨 전체를 512바이트 섹터로 환산해 1024 로 나눈 값이다.
 *   (3) 헤드 수는 언제나 16 이다.
 *   (4) 섹터 수는 128 을 시프트 폭만큼 오른쪽으로 민 값이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ioctl 경로에서만 불린다.
 *
 * caller: dasd.c:3291 이 discipline->fill_geometry 로 부른다. 그 위는
 * dasd.c 의 HDIO_GETGEO 처리다.
 * callee: dasd_int.h 의 dasd_check_blocksize().
 *
 * 에러 경로: 블록 크기가 유효하지 않으면 -EINVAL. geo 는 손대지 않는다.
 *
 * [상류 코드 관찰] 세 값을 곱하면 실린더 × 헤드 × 섹터 = 총 블록 수 × 2 가
 * 되어, 볼륨의 실제 512바이트 섹터 수(blocks 를 s2b_shift 만큼 왼쪽으로 민 값)와
 * **블록 크기가 1024바이트일 때만** 일치한다. 지오메트리는 요즘 블록 장치에서
 * 실제 배치가 아니라 관례상의 값이라 이 어긋남이 겉으로 드러나지는 않는다.
 * 또 dasd_diag.c 의 같은 이름 함수가 본문까지 한 글자도 다르지 않다.
 * 원본(1f0e418bb6) 201~210줄과 dasd_diag.c 에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   사용자 공간의 HDIO_GETGEO → dasd.c 의 지오메트리 처리
 *     → discipline->fill_geometry == [이 함수] → dasd_check_blocksize()
 */
static int dasd_fba_fill_geometry(struct dasd_block *block,
				  struct hd_geometry *geo)
{
	/* [한국어] 블록 크기가 유효한지 먼저 확인한다. 볼륨 분석 전이거나 분석이
	 * 실패한 장치에서는 bp_block 이 0 일 수 있어, 그대로 계산하면 뜻 없는
	 * 값이 나온다. */
	if (dasd_check_blocksize(block->bp_block) != 0)
		return -EINVAL;
	/* [한국어] 실린더 수 — 볼륨 전체를 512바이트 섹터로 환산해 1024 로 나눈 값이다.
	 * 왼쪽 시프트로 섹터 수를 만들고 오른쪽 10비트 시프트로 나눈다. */
	geo->cylinders = (block->blocks << block->s2b_shift) >> 10;
	/* [한국어] 헤드 수는 언제나 16 이다. 실제 하드웨어와 무관한 고정값이다. */
	geo->heads = 16;
	/* [한국어] 섹터 수 — 128 을 시프트 폭만큼 오른쪽으로 민 값이다.
	 * 블록 크기 512 면 128, 4096 이면 16 이 된다. */
	geo->sectors = 128 >> block->s2b_shift;
	return 0;
}

/* [한국어]
 * dasd_fba_erp_action - 실패한 요청을 어떤 함수로 복구할지 고른다
 *
 * @cqr: 복구가 필요해진 요청. 상태가 DASD_CQR_NEED_ERP 인 것만 여기 온다.
 * @return: 실제로 복구를 수행할 함수의 포인터. **언제나 dasd_erp.c 의
 *          dasd_default_erp_action() 이다.** 호출자는 받은 즉시 그 함수를
 *          cqr 을 인자로 불러 실행한다.
 *
 * **왜 필요한가**: DASD 코어는 '복구 함수를 고르는 일' 과 '복구를 수행하는 일'
 * 을 나눠 두었다. 고르는 일이 디시플린 몫이라 vtable 에 erp_action 칸이 있고,
 * FBA 는 그 칸을 이 함수로 채운다. ECKD 는 같은 자리에서 센스 데이터를 보고
 * dasd_3990_erp.c 의 상태 기계로 넘길지 기본 복구로 갈지 가르지만,
 * **FBA 는 고르지 않는다 — 언제나 같은 답을 준다.**
 *
 * 그 답인 dasd_default_erp_action() 은 이름 그대로 재시도 횟수가 남아 있으면
 * 요청을 DASD_CQR_FILLED 로 되돌려 다시 큐에 넣을 뿐이고, 센스 데이터를
 * 해석하지 않는다. FBA 볼륨은 대개 z/VM 이 뒷받침하는 가상 디스크라, 물리
 * 디스크의 결함 유형을 세세히 가려 복구할 필요가 적기 때문으로 보인다.
 *
 * 동작 과정: 한 단계뿐이다 — 함수 포인터 하나를 돌려준다.
 *
 * 실행 컨텍스트: softirq(tasklet) 또는 프로세스 컨텍스트. dasd.c:2772 는
 * block tasklet 안이고, dasd.c:2247 은 동기 요청을 기다리는 프로세스
 * 컨텍스트다. 어느 쪽이든 잠들 수 없다.
 *
 * caller: dasd.c:2247 의 동기 요청 복구 판정과 dasd.c:2772 의 블록 큐 처리.
 * 둘 다 discipline->erp_action 을 거친다.
 * callee: 없다. 함수를 부르지 않고 주소만 돌려준다.
 *
 * 에러 경로: 없다. NULL 을 돌려주는 갈래가 없으므로 호출자가 NULL 검사 없이
 * 곧바로 부르는 것이 안전하다.
 *
 * [상류 코드 관찰] 인자 cqr 을 한 번도 쓰지 않는다. 시그니처는 vtable 이
 * 정한 형태라 받기는 하지만, 반환값이 인자와 무관한 상수다.
 * 원본(1f0e418bb6) 212~216줄에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   dasd.c 의 블록 tasklet → discipline->erp_action == [이 함수]
 *     → (호출자가) dasd_default_erp_action()
 */
static dasd_erp_fn_t
dasd_fba_erp_action(struct dasd_ccw_req * cqr)
{
	/* [한국어] **cqr 을 보지 않고 언제나 같은 함수를 돌려준다.** dasd_erp.c 의
	 * 이 함수는 재시도 횟수가 남아 있으면 요청을 다시 준비 상태로 돌릴 뿐,
	 * 센스 데이터를 해석하지 않는다. ECKD 는 같은 자리에서 3990 제어 장치의
	 * 센스 해석 상태 기계로 넘길지 말지를 가린다. */
	return dasd_default_erp_action;
}

/* [한국어]
 * dasd_fba_erp_postaction - 복구가 끝난 뒤 뒷정리할 함수를 고른다
 *
 * @cqr: 복구가 끝난 요청. cqr->function 에 방금 어떤 복구를 썼는지가 남아 있다.
 * @return: 뒷정리 함수의 포인터, 또는 NULL. 정상 경로에서는 dasd_erp.c 의
 *          dasd_default_erp_postaction() 이다.
 *
 * **왜 필요한가**: 복구는 원래 요청을 복구용 cqr 로 감싸 다시 내보내는 방식이라,
 * 복구가 끝나면 그 감싼 것을 풀고 결과를 원래 요청에 옮겨야 한다. 그 뒷정리
 * 함수를 고르는 것이 이 함수의 일이며, 위 erp_action 의 짝이다.
 *
 * 동작 과정:
 *   (1) 이 요청에 실제로 쓰인 복구 함수가 기본 복구였는지 본다.
 *   (2) 맞으면 그 짝인 기본 뒷정리 함수를 돌려준다.
 *   (3) 아니면 디버그 로그에 '알 수 없는 ERP 동작' 을 남기고 NULL 을 돌려준다.
 *
 * **(3)에 닿을 수 있는가**: 위 erp_action 이 언제나 기본 복구만 돌려주므로,
 * 정상 경로에서 cqr->function 이 다른 값일 이유가 없다. 방어적 검사다.
 *
 * 실행 컨텍스트: softirq(tasklet) 또는 프로세스 컨텍스트. 잠들 수 없다.
 *
 * caller: dasd.c:2679 의 __dasd_process_erp() 가 discipline->erp_postaction
 * 으로 부른다.
 * callee: DBF_DEV_EVENT 매크로를 거쳐 s390 디버그 기능의 기록 함수.
 *
 * 에러 경로: 짝이 맞지 않으면 NULL 을 돌려준다. **호출자 dasd.c:2679~2680 은
 * 그 NULL 을 검사하지 않고 곧바로 부른다.** 즉 이 갈래에 실제로 닿으면
 * 널 포인터 역참조가 된다 — 그만큼 '일어날 수 없는 일' 로 다루어진 코드다.
 *
 * 호출 체인:
 *   dasd.c 의 __dasd_process_erp() → discipline->erp_postaction == [이 함수]
 *     → (호출자가) dasd_default_erp_postaction()
 */
static dasd_erp_fn_t
dasd_fba_erp_postaction(struct dasd_ccw_req * cqr)
{
	/* [한국어] 방금 이 요청에 어떤 복구 함수가 쓰였는지 확인한다. cqr->function 은
	 * 복구 함수가 스스로 남겨 두는 표시다. */
	if (cqr->function == dasd_default_erp_action)
		/* [한국어] 기본 복구였으면 그 짝인 기본 뒷정리 함수를 돌려준다. **정상 경로는
		 * 언제나 여기로 온다** — 위 erp_action 이 기본 복구만 돌려주기 때문이다. */
		return dasd_default_erp_postaction;

	/* [한국어] 짝이 맞지 않으면 디버그 링버퍼에 남긴다. 어떤 함수였는지 주소로 찍으며,
	 * %p 라 값이 가려져 나온다.
	 * 복구 함수의 주소. */
	DBF_DEV_EVENT(DBF_WARNING, cqr->startdev, "unknown ERP action %p",
		    cqr->function);
	/* [한국어] 뒷정리 함수를 고르지 못했음을 NULL 로 알린다. **호출자 dasd.c:2679~2680
	 * 은 이 NULL 을 검사하지 않고 곧바로 부르므로**, 실제로 여기에 닿으면
	 * 널 포인터 역참조가 된다. 그만큼 '일어날 수 없는 일' 로 다루어진 갈래다. */
	return NULL;
}

/* [한국어]
 * dasd_fba_check_for_device_change - 인터럽트가 '장치 상태가 바뀌었다' 는 신호인지 본다
 *
 * @device: 인터럽트가 도착한 장치.
 * @cqr: 이 인터럽트와 짝이 되는 요청. 원인 불명 인터럽트면 NULL 이 온다.
 *       **이 함수는 이 인자를 쓰지 않는다.**
 * @irb: 채널 서브시스템이 넘겨준 인터럽트 응답 블록. 상태 바이트가 들어 있다.
 * @return: 없다.
 *
 * **왜 필요한가**: 디스크가 준비되거나, 매체가 바뀌거나, 경로가 되살아나면
 * 제어 장치가 요청과 무관한 인터럽트를 올려 보낸다. 그것을 알아보고 큐를
 * 다시 굴려 주지 않으면, 멈춰 있던 요청들이 영영 깨어나지 않는다.
 * 디시플린 vtable 의 check_for_device_change 칸이 그 판정을 맡는다.
 *
 * 동작 과정:
 *   (1) 세 상태 비트를 한 마스크로 묶는다 — 주의(Attention), 장치 종료
 *       (Device End), 유닛 예외(Unit Exception)다.
 *   (2) 인터럽트의 장치 상태 바이트에 **그 셋이 모두** 서 있는지 본다.
 *       하나라도 빠지면 아무 일도 하지 않는다.
 *   (3) 모두 서 있으면 공통 처리 함수를 불러 정지 비트를 풀고 두 tasklet 과
 *       blk-mq 큐를 다시 굴린다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.** dasd.c:1584 의 dasd_int_handler()
 * 안에서 채널 장치 잠금을 쥔 채 불린다. 잠들 수 없고 오래 걸려서도 안 된다.
 *
 * caller: dasd.c:1668(요청과 짝이 있는 인터럽트)과 dasd.c:1813(원인 불명
 * 인터럽트). 후자는 cqr 자리에 NULL 을 넘긴다.
 * callee: dasd.c:1523 의 dasd_generic_handle_state_change().
 *
 * 에러 경로: 없다. 조건이 맞지 않으면 조용히 돌아간다.
 *
 * ECKD 와 견주면: dasd_eckd.c 의 같은 자리 함수는 센스 데이터까지 읽어
 * 경로 검증을 다시 걸거나 별칭 구성을 다시 훑는 등 훨씬 많은 일을 한다.
 * FBA 는 비트 셋만 본다.
 *
 * [상류 코드 관찰] 함수 본문의 닫는 중괄호 뒤에 **불필요한 세미콜론** 이 하나
 * 붙어 있다. 문법상 빈 선언이라 무해하지만 이 파일의 다른 함수에는 없는
 * 표기다. 같은 표기가 dasd_fba_handle_terminated_request() 에도 있다.
 * 원본(1f0e418bb6) 239줄에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   채널 서브시스템 인터럽트 → dasd.c 의 dasd_int_handler()
 *     → discipline->check_for_device_change == [이 함수]
 *     → dasd_generic_handle_state_change()
 */
static void dasd_fba_check_for_device_change(struct dasd_device *device,
					     struct dasd_ccw_req *cqr,
					     struct irb *irb)
{
	/* [한국어] 상태 비트들을 담을 임시 변수. char 로 잡은 것은 장치 상태가 한 바이트이기
	 * 때문이다. */
	char mask;

	/* first of all check for state change pending interrupt */
	/* [한국어] **세 상태 비트를 한 마스크로 묶는다.** 주의(Attention), 장치 종료
	 * (Device End), 유닛 예외(Unit Exception)다. 옆의 상류 주석이 이것이
	 * '상태 변화 대기' 인터럽트를 알아보는 조건이라고 밝힌다.
	 * 세 상수의 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
	mask = DEV_STAT_ATTENTION | DEV_STAT_DEV_END | DEV_STAT_UNIT_EXCEP;
	/* [한국어] **셋이 모두 서 있을 때만** 참이다. AND 결과가 마스크와 같은지 보는
	 * 관용구이며, 하나라도 빠지면 다른 종류의 인터럽트이므로 손대지 않는다. */
	if ((irb->scsw.cmd.dstat & mask) == mask)
		/* [한국어] 정지 비트를 풀고 두 tasklet 과 blk-mq 큐를 다시 굴린다.
		 * dasd.c:1523 의 이 함수는 EER 에 상태 조회를 걸고, DASD_STOPPED_PENDING
		 * 비트를 지우고, 멈춰 있던 요청들을 깨운다.
		 * **인자 cqr 은 이 함수 전체에서 쓰이지 않는다** — vtable 이 정한 시그니처라
		 * 받기만 한다. */
		dasd_generic_handle_state_change(device);
};


/* [한국어]
 * ccw_write_no_data - **데이터를 싣지 않은 쓰기 CCW** 를 만든다. discard 의 핵심
 *
 * @ccw: 채울 CCW 자리. dasd_smalloc_request() 가 0 으로 지워 둔 칸이다.
 * @return: 없다.
 *
 * **왜 필요한가**: 위 상류 주석과 dasd_fba_build_cp_discard() 의 주석이 밝히듯,
 * z/VM 이 뒷받침하는 FBA 장치에서는 **전송 길이가 0 인 WRITE 명령** 이
 * 그 블록들에 해당하는 하이퍼바이저 페이지를 0 으로 채우는 특별한 동작을
 * 일으킨다. 즉 데이터를 한 바이트도 보내지 않고 블록을 비울 수 있다.
 * 그것이 이 드라이버가 discard 를 지원할 수 있는 이유이며, 이 함수가 바로
 * 그 '데이터 없는 WRITE' CCW 를 만드는 자리다.
 *
 * 동작 과정:
 *   (1) 명령 코드를 FBA 의 WRITE(0x41)로 놓는다.
 *   (2) SLI(Suppress Length Indication) 플래그를 얹는다. 전송 길이 0 과
 *       제어 장치가 기대하는 길이가 어긋나도 오류로 보지 말라는 뜻이다.
 *       **이 플래그가 없으면 길이 0 자체가 오류가 된다.**
 *   (3) 전송 길이를 0 으로 놓는다. 데이터 주소(cda)는 건드리지 않아
 *       0 인 채로 남는다 — 길이가 0 이므로 채널이 읽지 않는다.
 *
 * 플래그를 대입이 아니라 OR 로 얹는 이유는, 호출자가 이 함수를 부르기 직전에
 * 앞 CCW 에 체이닝 플래그를 세우는 순서와 무관하게 안전하도록 하기 위해서다.
 * 실제로는 CCW 배열이 0 으로 초기화되어 있어 결과가 같다.
 *
 * 실행 컨텍스트: I/O 제출 경로, 잠들 수 없다.
 *
 * caller: 이 파일의 dasd_fba_build_cp_discard() 한 곳뿐이다.
 * callee: 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   do_dasd_request() → dasd_fba_build_cp() → dasd_fba_build_cp_discard()
 *     → [이 함수]
 */
/*
 * Builds a CCW with no data payload
 */
static void ccw_write_no_data(struct ccw1 *ccw)
{
	/* [한국어] 명령 코드를 FBA 의 WRITE(0x41)로 놓는다. discard 인데도 쓰기 명령인 것은,
	 * z/VM 이 '데이터 없는 쓰기' 를 페이지 비우기로 해석하기 때문이다. */
	ccw->cmd_code = DASD_FBA_CCW_WRITE;
	/* [한국어] **SLI(Suppress Length Indication) 플래그를 얹는다.** 전송 길이가 0 이라
	 * 제어 장치가 기대하는 길이와 어긋나는데, 이 플래그가 없으면 그 어긋남
	 * 자체가 오류로 보고된다. 대입이 아니라 OR 인 것은 호출자가 이미 얹어 둔
	 * 플래그를 지우지 않기 위해서다(실제로는 배열이 0 으로 초기화돼 있어 결과가 같다).
	 * 플래그의 정확한 의미는 arch/s390 소관이라 이 트리에서 확인 못 함. */
	ccw->flags |= CCW_FLAG_SLI;
	/* [한국어] **전송 길이를 0 으로 놓는다 — 이것이 이 CCW 의 전부다.**
	 * 데이터 주소(cda)는 건드리지 않아 0 인 채로 남지만, 길이가 0 이라
	 * 채널이 읽지 않는다. */
	ccw->count = 0;
}

/* [한국어]
 * ccw_write_zero - 0 으로 채운 페이지를 실어 보내는 쓰기 CCW 를 만든다
 *
 * @ccw: 채울 CCW 자리.
 * @count: 실제로 써 넣을 바이트 수. 호출자가 '블록 수 × 블록 크기' 를 넘긴다.
 * @return: 없다.
 *
 * **왜 필요한가**: 위 ccw_write_no_data() 의 '데이터 없는 WRITE' 는 하이퍼바이저
 * 페이지 단위로만 통한다. 요청의 앞뒤가 페이지 경계에 맞지 않으면 그 조각은
 * **진짜로 0 을 써서** 지워야 한다. 그 조각을 담당하는 것이 이 함수다.
 * 상류 주석이 밝히듯 일부 z/VM 판본의 문제 때문에 4KB 정렬을 지켜야 해서
 * 생긴 우회로다.
 *
 * 동작 과정:
 *   (1) 명령 코드를 FBA 의 WRITE(0x41)로 놓는다.
 *   (2) SLI 플래그를 얹는다.
 *   (3) 전송 길이를 호출자가 준 바이트 수로 놓는다.
 *   (4) 데이터 주소로 **모듈 전역의 0 페이지** 를 가리킨다.
 *
 * **여기서 눈여겨볼 점**: 전송 길이가 한 페이지(4096)를 넘을 수 있다.
 * 호출자는 '정렬되지 않은 조각의 블록 수 × 블록 크기' 를 넘기는데, 블록
 * 크기가 4096 이면 조각 하나만으로도 그 값이 나온다. 페이지 하나를 넘는 길이가
 * 어떻게 처리되는지는 채널 서브시스템 소관이라 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: I/O 제출 경로, 잠들 수 없다.
 *
 * caller: 이 파일의 dasd_fba_build_cp_discard() 에서 두 번(앞 조각과 뒤 조각).
 * callee: arch/s390 의 주소 변환 하나. 이 트리에 없다.
 *
 * 에러 경로: 없다. 0 페이지는 모듈이 올라올 때 잡아 두므로 여기서 실패할
 * 여지가 없다.
 *
 * 호출 체인:
 *   do_dasd_request() → dasd_fba_build_cp() → dasd_fba_build_cp_discard()
 *     → [이 함수]
 */
/*
 * Builds a CCW that writes only zeroes.
 */
static void ccw_write_zero(struct ccw1 *ccw, int count)
{
	/* [한국어] 명령 코드를 FBA 의 WRITE(0x41)로 놓는다. */
	ccw->cmd_code = DASD_FBA_CCW_WRITE;
	/* [한국어] SLI 플래그를 얹는다. 여기서는 길이가 0 이 아니지만, 위 함수와 형식을
	 * 맞추고 제어 장치가 기대하는 길이와의 어긋남을 허용하기 위해 함께 세운다. */
	ccw->flags |= CCW_FLAG_SLI;
	/* [한국어] 호출자가 준 바이트 수를 전송 길이로 놓는다. '블록 수 × 블록 크기' 이므로
	 * 한 페이지(4096)를 넘을 수 있다. */
	ccw->count = count;
	/* [한국어] **데이터 원본으로 모듈 전역의 0 페이지를 가리킨다.** 이 페이지는
	 * dasd_fba_init() 이 잡아 두고 한 번도 쓰지 않으므로 언제나 0 으로 차 있다.
	 * 읽히기만 하므로 여러 장치가 동시에 가리켜도 경쟁이 없다 — 그래서 잠금이 없다. */
	ccw->cda = virt_to_dma32(dasd_fba_zero_page);
}

/* [한국어]
 * count_ccws - discard 요청에 CCW 가 몇 개 필요한지 미리 센다
 *
 * @first_rec: 요청의 첫 블록 번호.
 * @last_rec: 요청의 마지막 블록 번호(포함).
 * @blocks_per_page: 한 페이지에 들어가는 블록 수. PAGE_SIZE 를 블록 크기로
 *                   나눈 값이며 dasd_int.h:424 의 BLOCKS_PER_PAGE 가 계산한다.
 * @return: 필요한 '구간' 의 개수. 1, 2, 또는 3 이다. 호출자는 이 값에서
 *          CCW 개수와 자료 버퍼 크기를 뽑아 cqr 을 알맞은 크기로 잡는다.
 *
 * **왜 필요한가**: cqr 은 CCW 배열과 자료 버퍼를 한 덩어리로 잡아야 하는데,
 * 그 크기를 **채우기 전에** 알아야 한다. 그런데 discard 채널 프로그램의 모양은
 * 요청이 페이지 경계에 어떻게 걸치는지에 따라 갈린다. 그래서 실제로 채우는
 * 함수와 **똑같은 세 갈래 판정을 한 번 더 돌려** 개수만 세는 것이다.
 *
 * 세 갈래는 이렇다.
 *   (1) 앞 조각: 시작 블록이 페이지 경계에 맞지 않으면, 다음 페이지 경계
 *       직전까지가 한 구간이다.
 *   (2) 본체: 남은 구간이 한 페이지 이상이면, 페이지 단위로 딱 떨어지는
 *       부분이 한 구간이다. **이 구간만이 '데이터 없는 WRITE' 로 처리된다.**
 *   (3) 뒤 조각: 아직 남은 블록이 있거나 앞의 둘이 하나도 잡히지 않았으면
 *       나머지가 한 구간이다.
 *
 * 동작 과정은 위 셋을 차례로 판정하며 cur_pos 로 진행 위치를 옮기고
 * count 를 올리는 것뿐이다. 값을 어디에도 저장하지 않는 **모의 실행** 이다.
 *
 * 실행 컨텍스트: I/O 제출 경로, 잠들 수 없다.
 *
 * caller: 이 파일의 dasd_fba_build_cp_discard() 한 곳뿐이다.
 * callee: 없다. 정수 산술뿐이다.
 *
 * 에러 경로: 없다. 어떤 입력에도 1 이상을 돌려준다 — 앞의 둘이 모두
 * 잡히지 않으면 (3)의 `cur_pos == 0` 조건이 걸리기 때문이다.
 *
 * **유지보수상의 함정**: 이 함수의 세 조건식은 dasd_fba_build_cp_discard()
 * 안의 세 조건식과 **글자 그대로 같아야 한다.** 한쪽만 고치면 세어 둔 개수와
 * 실제로 채우는 개수가 어긋나 CCW 배열을 넘겨 쓰게 된다.
 *
 * 호출 체인:
 *   do_dasd_request() → dasd_fba_build_cp() → dasd_fba_build_cp_discard()
 *     → [이 함수]
 */
/*
 * Helper function to count the amount of necessary CCWs within a given range
 * with 4k alignment and command chaining in mind.
 */
static int count_ccws(sector_t first_rec, sector_t last_rec,
		      unsigned int blocks_per_page)
{
	/* [한국어] 앞 조각과 본체가 각각 어디서 끝나는지 담을 자리. 아래 실제 채우기 함수와
	 * 같은 이름을 써 두 코드를 나란히 놓고 대조할 수 있게 했다. */
	sector_t wz_stop = 0, d_stop = 0;
	/* [한국어] 익스텐트 안에서 지금까지 처리한 블록 수. '진행 위치' 다. */
	int cur_pos = 0;
	/* [한국어] 지금까지 센 구간 수. 이것이 반환값이 된다. */
	int count = 0;

	/* [한국어] **첫째 갈래 — 시작이 페이지 경계에 맞지 않는가.** 맞지 않으면 그 앞머리
	 * 조각은 '데이터 없는 WRITE' 로 지울 수 없어 따로 다뤄야 한다. */
	if (first_rec % blocks_per_page != 0) {
		/* [한국어] 다음 페이지 경계 **직전** 블록의 번호를 구한다.
		 * 시작 번호에 페이지당 블록 수를 더한 뒤, 시작 번호가 페이지 안에서 밀린
		 * 만큼을 빼고, 다시 1 을 빼면 경계 직전 블록이 된다. */
		wz_stop = first_rec + blocks_per_page -
			(first_rec % blocks_per_page) - 1;
		/* [한국어] 요청이 그 경계보다 먼저 끝날 수도 있다. */
		if (wz_stop > last_rec)
			/* [한국어] 그러면 요청의 마지막 블록까지가 이 조각이다. */
			wz_stop = last_rec;
		/* [한국어] 이 조각의 블록 수만큼 진행 위치를 옮긴다. 포함 구간이라 +1 이 붙는다. */
		cur_pos = wz_stop - first_rec + 1;
		/* [한국어] 구간 하나를 세었다. */
		count++;
	}

	/* [한국어] **둘째 갈래 — 남은 구간이 한 페이지 이상인가.** 그래야 페이지 단위로
	 * '데이터 없는 WRITE' 를 걸 수 있다. */
	if (last_rec - (first_rec + cur_pos) + 1 >= blocks_per_page) {
		/* [한국어] 남은 구간의 끝이 페이지 경계에 딱 맞는지 본다. 마지막 블록에서
		 * 페이지 하나를 물러난 자리를 기준으로 나머지를 본다. */
		if ((last_rec - blocks_per_page + 1) % blocks_per_page != 0)
			/* [한국어] 맞지 않으면 마지막 페이지 경계까지만 본체로 삼는다.
			 * 페이지당 블록 수로 나눈 나머지만큼 뒤로 물린다. */
			d_stop = last_rec - ((last_rec - blocks_per_page + 1) %
					     blocks_per_page);
		else
			/* [한국어] 딱 맞으면 요청의 마지막 블록까지가 본체다. */
			d_stop = last_rec;

		/* [한국어] 본체의 블록 수만큼 진행 위치를 옮긴다. */
		cur_pos += d_stop - (first_rec + cur_pos) + 1;
		/* [한국어] 구간 하나를 더 세었다. */
		count++;
	}

	/* [한국어] **셋째 갈래 — 아직 남은 블록이 있거나, 앞의 둘이 하나도 잡히지 않았는가.**
	 * 앞 조건(cur_pos == 0)이 있어야 요청 전체가 한 페이지에 못 미치는 작은
	 * 경우에도 구간 하나를 보장할 수 있다. 그래서 이 함수는 언제나 1 이상을 돌려준다. */
	if (cur_pos == 0 || first_rec + cur_pos - 1 < last_rec)
		/* [한국어] 마지막 구간을 세었다. */
		count++;

	/* [한국어] 구간 수를 돌려준다. 1, 2, 또는 3 이다. */
	return count;
}

/* [한국어]
 * dasd_fba_build_cp_discard - discard/write-zeroes 요청을 채널 프로그램으로 옮긴다
 *
 * @memdev: cqr 을 담을 메모리를 대 줄 장치. 여기서는 언제나 기본 장치다.
 * @block: 대상 블록 장치. 블록 크기와 시프트 폭을 여기서 읽는다.
 * @req: 블록 계층이 준 요청. REQ_OP_DISCARD 이거나 REQ_OP_WRITE_ZEROES 다.
 * @return: 채워진 struct dasd_ccw_req 포인터. 메모리가 모자라면
 *          ERR_PTR(-ENOMEM) 이며, 호출자 dasd.c:3069~3072 가 그것을
 *          BLK_STS_RESOURCE 로 옮겨 blk-mq 에 재시도를 맡긴다.
 *
 * **왜 필요한가**: FBA 볼륨은 z/VM 이 페이지 단위로 뒷받침하므로, 데이터를
 * 싣지 않은 WRITE 명령 하나로 페이지를 통째로 0 으로 되돌릴 수 있다.
 * 그 특성을 쓰면 discard 를 사실상 공짜로 구현할 수 있다. 이 함수가 그
 * 변환을 맡으며, 위 상류 주석이 그 원리와 **4KB 정렬 우회** 의 배경을 밝힌다.
 *
 * 동작 과정:
 *   (1) 요청의 섹터 범위를 블록 번호 범위로 옮기고 블록 수를 센다.
 *   (2) 페이지당 블록 수를 구하고, count_ccws() 로 구간이 몇 개 생길지 센다.
 *   (3) 구간 하나마다 Locate Record CCW 와 데이터 CCW 가 하나씩 필요하므로
 *       CCW 개수는 1 + 2 × 구간 수 이고, 자료 버퍼는 Define Extent 자료
 *       하나에 구간마다 Locate Record 자료 하나를 더한 크기다.
 *       **자료 버퍼 크기에 struct ccw1 크기가 구간마다 한 번씩 더 들어간다**
 *       (아래 관찰 참고).
 *   (4) 정적 풀에서 cqr 을 떼어 온다.
 *   (5) 첫 CCW 를 Define Extent 로 채워 요청 전체를 구간으로 연다.
 *   (6) 앞 조각이 있으면 Locate Record + '0 페이지 쓰기' 두 CCW 를 잇는다.
 *   (7) 본체가 있으면 Locate Record + '데이터 없는 쓰기' 두 CCW 를 잇는다.
 *       **여기가 진짜 discard 다.**
 *   (8) 뒤 조각이 남았으면 다시 Locate Record + '0 페이지 쓰기' 를 잇는다.
 *   (9) failfast 여부를 옮기고, cqr 의 나머지 칸(장치, 만료, 재시도, 생성
 *       시각, 상태)을 채워 DASD_CQR_FILLED 로 표시한다.
 *
 * CCW 를 잇는 방식이 독특하다 — 다음 CCW 를 채우기 **직전에**
 * `ccw[-1].flags |= CCW_FLAG_CC` 로 방금 채운 CCW 에 체이닝 플래그를 얹는다.
 * 그래서 사슬의 마지막 CCW 에는 자연스럽게 플래그가 남지 않고, 거기서
 * 채널이 멈춘다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. do_dasd_request() 가 dq->lock 을 스핀락으로
 * 쥐고 있으므로 **잠들 수 없다.** 메모리는 미리 잡아 둔 정적 풀에서 온다.
 *
 * caller: 이 파일의 dasd_fba_build_cp() 한 곳뿐이다.
 * callee: dasd_int.h:424 의 BLOCKS_PER_PAGE, 이 파일의 count_ccws(),
 * dasd.c:1136 의 dasd_smalloc_request(), 이 파일의 define_extent(),
 * locate_record(), ccw_write_zero(), ccw_write_no_data(),
 * include/linux/blk-mq.h 의 blk_rq_pos()/blk_rq_sectors()/blk_mq_rq_to_pdu(),
 * include/linux/blkdev.h:704 의 blk_noretry_request().
 *
 * 에러 경로: cqr 할당 실패 하나뿐이다. 그 값을 그대로 돌려주며, 그 전까지
 * 잡아 둔 자원이 없으므로 되돌릴 것도 없다.
 *
 * [상류 코드 관찰] 자료 버퍼 크기 계산에 struct ccw1 의 크기가 구간마다
 * 한 번씩 더해진다. CCW 배열은 cplength 로 따로 잡히므로 이 몫은 자료
 * 버퍼 안에서 쓰이지 않는 여윳값이다. 넉넉히 잡는 쪽이라 안전하다.
 * 원본(1f0e418bb6) 343~344줄에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   do_dasd_request() → discipline->build_cp == dasd_fba_build_cp()
 *     → [이 함수] → count_ccws() / dasd_smalloc_request() / define_extent()
 *     → locate_record() → ccw_write_zero() / ccw_write_no_data()
 */
/*
 * This function builds a CCW request for block layer discard requests.
 * Each page in the z/VM hypervisor that represents certain records of an FBA
 * device will be padded with zeros. This is a special behaviour of the WRITE
 * command which is triggered when no data payload is added to the CCW.
 *
 * Note: Due to issues in some z/VM versions, we can't fully utilise this
 * special behaviour. We have to keep a 4k (or 8 block) alignment in mind to
 * work around those issues and write actual zeroes to the unaligned parts in
 * the request. This workaround might be removed in the future.
 */
static struct dasd_ccw_req *dasd_fba_build_cp_discard(
						struct dasd_device *memdev,
						struct dasd_block *block,
						struct request *req)
{
	/* [한국어] Locate Record 자료들을 쌓아 갈 커서. cqr->data 안의 Define Extent 자료
	 * 뒤를 가리킨다. */
	struct LO_fba_data *LO_data;
	/* [한국어] 만들어 돌려줄 CCW 요청. */
	struct dasd_ccw_req *cqr;
	/* [한국어] CCW 배열을 채워 갈 커서. */
	struct ccw1 *ccw;

	/* [한국어] 앞 조각과 본체가 끝나는 블록 번호. **0 으로 초기화하는 것이 중요하다** —
	 * 아래 셋째 갈래가 `d_stop != 0` 과 `wz_stop != 0` 으로 앞의 두 갈래가
	 * 실행됐는지를 판정하기 때문이다. */
	sector_t wz_stop = 0, d_stop = 0;
	/* [한국어] 요청의 첫 블록과 마지막 블록 번호. */
	sector_t first_rec, last_rec;

	/* [한국어] 블록 크기를 꺼내 둔다. */
	unsigned int blksize = block->bp_block;
	/* [한국어] 페이지 하나에 들어가는 블록 수. */
	unsigned int blocks_per_page;
	/* [한국어] '진짜 0 을 쓸' 조각의 블록 수. */
	int wz_count = 0;
	/* [한국어] '데이터 없는 WRITE' 로 지울 본체의 블록 수. */
	int d_count = 0;
	/* [한국어] 익스텐트 안의 진행 위치. 옆의 상류 주석이 그 뜻을 밝힌다. */
	int cur_pos = 0; /* Current position within the extent */
	/* [한국어] 요청 전체의 블록 수. */
	int count = 0;
	/* [한국어] 만들 CCW 개수. */
	int cplength;
	/* [한국어] 자료 버퍼 크기. */
	int datasize;
	/* [한국어] 구간 수. count_ccws() 가 세어 줄 값이다. */
	int nr_ccws;

	/* [한국어] 요청의 시작 섹터를 블록 번호로 옮긴다. */
	first_rec = blk_rq_pos(req) >> block->s2b_shift;
	/* [한국어] 요청의 마지막 블록 번호를 구한다.
	 * 시작 섹터에 섹터 수를 더하고 1 을 뺀 뒤 블록 번호로 옮긴다. */
	last_rec =
		(blk_rq_pos(req) + blk_rq_sectors(req) - 1) >> block->s2b_shift;
	/* [한국어] 요청 전체의 블록 수. 포함 구간이라 +1 이다. */
	count = last_rec - first_rec + 1;

	/* [한국어] dasd_int.h:424 의 매크로가 PAGE_SIZE 를 블록 크기로 나눈다.
	 * 블록 크기 512 면 8, 4096 이면 1 이다. **z/VM 이 FBA 볼륨을 페이지 단위로
	 * 뒷받침하기 때문에** 이 값이 discard 의 알갱이가 된다. */
	blocks_per_page = BLOCKS_PER_PAGE(blksize);
	/* [한국어] 구간이 몇 개 생길지 미리 센다. 아래 세 갈래와 **똑같은 판정** 을
	 * 한 번 더 돌리는 모의 실행이다. */
	nr_ccws = count_ccws(first_rec, last_rec, blocks_per_page);

	/* define extent + nr_ccws * locate record + nr_ccws * single CCW */
	/* [한국어] CCW 개수 — Define Extent 하나에, 구간마다 Locate Record 하나와
	 * 데이터 CCW 하나씩이다. 옆의 상류 주석이 그 셈을 밝힌다. */
	cplength = 1 + 2 * nr_ccws;
	/* [한국어] 자료 버퍼 크기 — Define Extent 자료 하나에 구간마다 Locate Record 자료
	 * 하나씩이다.
	 * [상류 코드 관찰] 구간마다 struct ccw1 의 크기가 한 번 더 더해진다.
	 * CCW 배열은 위 cplength 로 따로 잡히므로 이 몫은 자료 버퍼 안에서 쓰이지
	 * 않는 여윳값이다. 넉넉히 잡는 쪽이라 안전하다.
	 * 원본(1f0e418bb6) 343~344줄에서 확인했으며 코드는 고치지 않았다. */
	datasize = sizeof(struct DE_fba_data) +
		nr_ccws * (sizeof(struct LO_fba_data) + sizeof(struct ccw1));

	/* [한국어] 정적 메모리 풀에서 cqr 과 CCW 배열, 자료 버퍼를 한 덩어리로 떼어 온다.
	 * 첫 인자가 cqr->magic 이 되어 인터럽트의 소유자 확인에 쓰인다.
	 * cqr 구조체 자체는 요청 뒤에 딸린 드라이버 전용 자리를 재사용한다. */
	cqr = dasd_smalloc_request(DASD_FBA_MAGIC, cplength, datasize, memdev,
				   blk_mq_rq_to_pdu(req));
	/* [한국어] 풀이 말랐으면 실패한다. */
	if (IS_ERR(cqr))
		/* [한국어] 오류 포인터를 그대로 올린다. 호출자가 BLK_STS_RESOURCE 로 옮겨
		 * blk-mq 에 재시도를 맡긴다. */
		return cqr;

	/* [한국어] CCW 배열의 첫 칸으로 커서를 놓는다. */
	ccw = cqr->cpaddr;

	/* [한국어] 첫 CCW 를 Define Extent 로 채워 요청 전체를 한 구간으로 연다.
	 * 방향은 리터럴 WRITE 다 — discard 는 데이터를 나르지 않지만 하드웨어에는
	 * 쓰기로 보이기 때문이다. **여기서는 시작 위치에 블록 번호를 넘긴다**
	 * (일반 경로는 섹터 번호를 넘긴다). */
	define_extent(ccw++, cqr->data, WRITE, blksize, first_rec, count);
	/* [한국어] Locate Record 자료 구역의 시작을 잡는다. Define Extent 자료 16바이트
	 * 바로 뒤다. **IDAL 구역이 없는 것이 일반 경로와 다른 점** 이다 — discard 는
	 * 데이터 버퍼를 가리키지 않으므로 IDAL 이 필요 없다. */
	LO_data = cqr->data + sizeof(struct DE_fba_data);

	/* First part is not aligned. Calculate range to write zeroes. */
	/* [한국어] **첫째 갈래 — 앞 조각.** count_ccws() 의 같은 조건과 글자 그대로 같아야 한다. */
	if (first_rec % blocks_per_page != 0) {
		/* [한국어] 다음 페이지 경계 직전 블록의 번호를 구한다.
		 * count_ccws() 와 같은 산식이다. */
		wz_stop = first_rec + blocks_per_page -
			(first_rec % blocks_per_page) - 1;
		/* [한국어] 요청이 그 경계보다 먼저 끝나는지 본다. */
		if (wz_stop > last_rec)
			/* [한국어] 그러면 요청의 마지막 블록까지가 이 조각이다. */
			wz_stop = last_rec;
		/* [한국어] 이 조각의 블록 수. */
		wz_count = wz_stop - first_rec + 1;

		/* [한국어] 방금 채운 Define Extent CCW 를 명령 체이닝으로 잇는다.
		 * 커서가 이미 다음 칸을 가리키므로 `ccw[-1]` 로 한 칸 뒤를 짚는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] Locate Record 를 채운다. 시작은 익스텐트의 처음(cur_pos 는 아직 0),
		 * 개수는 이 조각의 블록 수다. */
		locate_record(ccw++, LO_data++, WRITE, cur_pos, wz_count);

		/* [한국어] 방금 채운 Locate Record 를 명령 체이닝으로 잇는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] **진짜 0 을 쓰는 CCW** 를 만든다. 전송 길이는 블록 수 × 블록 크기다.
		 * 이 조각은 페이지 경계에 맞지 않아 '데이터 없는 WRITE' 로는 지울 수 없다. */
		ccw_write_zero(ccw++, wz_count * blksize);

		/* [한국어] 진행 위치를 이 조각의 끝으로 옮긴다. */
		cur_pos = wz_count;
	}

	/* We can do proper discard when we've got at least blocks_per_page blocks. */
	/* [한국어] **둘째 갈래 — 본체.** 남은 구간이 한 페이지 이상이어야 페이지 단위
	 * discard 를 걸 수 있다. 위 상류 주석이 그 뜻을 밝힌다. */
	if (last_rec - (first_rec + cur_pos) + 1 >= blocks_per_page) {
		/* is last record at page boundary? */
		/* [한국어] 남은 구간의 끝이 페이지 경계에 딱 맞는지 본다. 옆의 상류 주석이
		 * 그것을 묻는다고 적었다. */
		if ((last_rec - blocks_per_page + 1) % blocks_per_page != 0)
			/* [한국어] 맞지 않으면 마지막 페이지 경계까지만 본체로 삼는다.
			 * 페이지당 블록 수로 나눈 나머지만큼 뒤로 물린다. */
			d_stop = last_rec - ((last_rec - blocks_per_page + 1) %
					     blocks_per_page);
		else
			/* [한국어] 딱 맞으면 요청의 마지막 블록까지가 본체다. */
			d_stop = last_rec;

		/* [한국어] 본체의 블록 수. 진행 위치에서 본체 끝까지의 포함 구간이다. */
		d_count = d_stop - (first_rec + cur_pos) + 1;

		/* [한국어] 앞 CCW 를 명령 체이닝으로 잇는다. 첫째 갈래가 실행됐으면 '0 쓰기' CCW 이고,
		 * 아니면 Define Extent 다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] Locate Record 를 채운다. 시작은 진행 위치, 개수는 본체의 블록 수다. */
		locate_record(ccw++, LO_data++, WRITE, cur_pos, d_count);

		/* [한국어] 방금 채운 Locate Record 를 명령 체이닝으로 잇는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] **여기가 진짜 discard 다.** 데이터를 하나도 싣지 않은 WRITE 명령을
		 * 만들어, z/VM 이 해당 페이지들을 0 으로 되돌리게 한다. 전송할 데이터가
		 * 없으므로 CPU 도 채널도 바이트를 옮기지 않는다. */
		ccw_write_no_data(ccw++);

		/* [한국어] 진행 위치를 본체의 끝으로 옮긴다. */
		cur_pos += d_count;
	}

	/* We might still have some bits left which need to be zeroed. */
	/* [한국어] **셋째 갈래 — 뒤 조각.** 아직 남은 블록이 있거나(진행 위치가 마지막
	 * 블록에 못 미침), 앞의 두 갈래가 하나도 실행되지 않았으면(진행 위치가 0)
	 * 나머지를 0 으로 채워야 한다. 위 상류 주석이 그 뜻을 밝힌다. */
	if (cur_pos == 0 || first_rec + cur_pos - 1 < last_rec) {
		/* [한국어] 본체가 실행됐는지를 **d_stop 이 0 이 아닌가** 로 판정한다. 위에서
		 * 0 으로 초기화해 둔 덕분에 성립하는 판정이다. */
		if (d_stop != 0)
			/* [한국어] 본체 끝에서 요청 끝까지가 남은 조각이다. */
			wz_count = last_rec - d_stop;
		/* [한국어] 본체가 없었다면 앞 조각이 실행됐는지를 wz_stop 으로 판정한다. */
		else if (wz_stop != 0)
			/* [한국어] 앞 조각 끝에서 요청 끝까지가 남은 조각이다. */
			wz_count = last_rec - wz_stop;
		else
			/* [한국어] 둘 다 없었다면 요청 전체가 이 조각이다 — 한 페이지에 못 미치면서
			 * 시작도 경계에 맞는 작은 요청이 여기 온다. */
			wz_count = count;

		/* [한국어] 앞 CCW 를 명령 체이닝으로 잇는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] Locate Record 를 채운다. 시작은 진행 위치, 개수는 남은 블록 수다. */
		locate_record(ccw++, LO_data++, WRITE, cur_pos, wz_count);

		/* [한국어] 방금 채운 Locate Record 를 명령 체이닝으로 잇는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] 마지막 조각을 0 으로 채운다. **이 CCW 에는 체이닝 플래그를 얹지
		 * 않으므로 여기서 사슬이 끝난다.** */
		ccw_write_zero(ccw++, wz_count * blksize);
	}

	/* [한국어] 요청 자체가 재시도 금지 표시를 달고 있는지 본다.
	 * 또는 장치에 failfast 기능이 켜져 있는지 본다. */
	if (blk_noretry_request(req) ||
	    block->base->features & DASD_FEATURE_FAILFAST)
		/* [한국어] 둘 중 하나면 cqr 에 failfast 플래그를 세운다. */
		set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);

	/* [한국어] 이 요청을 실제로 내보낼 장치. */
	cqr->startdev = memdev;
	/* [한국어] 이 요청의 메모리를 대 준 장치. free_cp 가 반납할 때 쓴다. */
	cqr->memdev = memdev;
	/* [한국어] 이 요청이 속한 블록 장치. */
	cqr->block = block;
	/* [한국어] 만료 시간을 초에서 지프로 옮긴다. 기본 300초이며 옆의 상류 주석이
	 * 5분이라 적었다. */
	cqr->expires = memdev->default_expires * HZ;	/* default 5 minutes */
	/* [한국어] 재시도 횟수. FBA 의 기본값은 32 다. */
	cqr->retries = memdev->default_retries;
	/* [한국어] 요청을 만든 시각을 TOD 시계로 찍어 둔다. */
	cqr->buildclk = get_tod_clock();
	/* [한국어] **요청을 '준비 완료' 로 표시한다.** 이 상태가 되어야 tasklet 이
	 * 집어 채널로 내보낸다. */
	cqr->status = DASD_CQR_FILLED;

	/* [한국어] 완성된 요청을 돌려준다. */
	return cqr;
}

/* [한국어]
 * dasd_fba_build_cp_regular - 일반 읽기·쓰기 요청을 채널 프로그램으로 옮긴다
 *
 * @memdev: cqr 을 담을 메모리를 대 줄 장치.
 * @block: 대상 블록 장치. 블록 크기와 시프트 폭을 여기서 읽는다.
 * @req: 블록 계층이 준 요청. 읽기이거나 쓰기다.
 * @return: 채워진 struct dasd_ccw_req 포인터. 요청이 블록 크기의 배수가
 *          아니거나 블록 수 검산이 어긋나면 ERR_PTR(-EINVAL) 이고,
 *          메모리가 모자라면 ERR_PTR(-ENOMEM) 이다. 호출자
 *          dasd.c:3069~3078 이 앞의 것은 BLK_STS_INVAL 로, 뒤의 것은
 *          BLK_STS_RESOURCE 로 옮긴다.
 *
 * **이 파일의 심장이자 가장 긴 함수** 다. 블록 계층의 struct request 를
 * 채널이 실행할 CCW 사슬로 옮기는 실제 변환이 여기서 일어난다.
 *
 * **만들어지는 채널 프로그램의 모양은 두 가지** 이며, 장치가 데이터
 * 체이닝을 지원하는지(private->rdc_data.mode.bits.data_chain)로 갈린다.
 *
 *   지원하는 장치:
 *     [DE] -CC-> [LO(전체 블록 수)] -CC-> [데이터] -DC-> [데이터] -DC-> ...
 *     Locate Record 가 하나뿐이고, 두 번째 데이터 CCW 부터는 데이터
 *     체이닝으로 이어 붙는다. CCW 개수는 2 + 블록 수다.
 *
 *   지원하지 않는 장치("stupid devices"):
 *     [DE] -CC-> [LO(1)] -CC-> [데이터] -CC-> [LO(1)] -CC-> [데이터] -CC-> ...
 *     블록마다 Locate Record 를 앞세운다. CCW 개수는 2 + 2 × 블록 수 - 1 이고,
 *     자료 버퍼도 Locate Record 자료만큼 더 든다.
 *
 * 동작 과정:
 *   (1) 요청 방향으로 명령 코드를 정한다(READ 0x42 / WRITE 0x41).
 *   (2) 요청의 섹터 범위를 블록 번호 범위로 옮긴다.
 *   (3) 요청의 세그먼트를 훑으며 블록 수를 세고, 동시에 IDAL 이 필요한
 *       세그먼트의 블록 수도 따로 센다. 세그먼트 길이가 블록 크기의 배수가
 *       아니면 곧바로 -EINVAL 이다.
 *   (4) 센 블록 수가 (2)의 범위와 맞는지 검산한다.
 *   (5) CCW 개수와 자료 버퍼 크기를 정한다. 데이터 체이닝을 못 하는 장치면
 *       둘 다 늘린다.
 *   (6) 정적 풀에서 cqr 을 떼어 온다.
 *   (7) 첫 CCW 를 Define Extent 로 채운다.
 *   (8) 자료 버퍼 안의 배치를 정한다 — Define Extent 자료 뒤에 IDAL 낱말
 *       배열, 그 뒤에 Locate Record 자료들이 온다.
 *   (9) 데이터 체이닝이 되면 여기서 Locate Record 하나를 만들어 둔다.
 *   (10) 세그먼트를 다시 훑으며 블록마다 데이터 CCW 를 하나씩 만든다.
 *        버퍼가 채널이 직접 가리킬 수 없는 자리면 IDAL 로 우회한다.
 *   (11) failfast 여부를 옮기고 cqr 의 나머지 칸을 채워 DASD_CQR_FILLED 로
 *        표시한다.
 *
 * **세그먼트를 두 번 훑는 이유**: 첫 번째는 cqr 을 얼마나 크게 잡을지 알기
 * 위한 계산이고, 두 번째가 실제로 채우는 일이다. 그 사이에 할당이 끼어
 * 있어 한 번으로 합칠 수 없다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. dq->lock 을 쥔 채라 **잠들 수 없다.**
 *
 * caller: 이 파일의 dasd_fba_build_cp() 한 곳뿐이다.
 * callee: include/linux/blk-mq.h 의 rq_data_dir()/blk_rq_pos()/
 * blk_rq_sectors()/rq_for_each_segment()/blk_mq_rq_to_pdu(),
 * dasd.c:1136 의 dasd_smalloc_request(), 이 파일의 define_extent() 와
 * locate_record(), 그리고 arch/s390 의 IDAL 처리와 주소 변환.
 *
 * 에러 경로: 세 갈래다. 세그먼트가 블록 크기의 배수가 아니면 -EINVAL,
 * 블록 수 검산이 어긋나면 -EINVAL, cqr 할당이 실패하면 그 오류를 그대로
 * 돌려준다. 앞의 둘은 아직 아무것도 잡지 않은 상태라 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   do_dasd_request() → discipline->build_cp == dasd_fba_build_cp()
 *     → [이 함수] → dasd_smalloc_request() → define_extent() → locate_record()
 */
static struct dasd_ccw_req *dasd_fba_build_cp_regular(
						struct dasd_device *memdev,
						struct dasd_block *block,
						struct request *req)
{
	/* [한국어] 장치의 사적 상태. 여기서 필요한 것은 데이터 체이닝 비트 하나뿐이며,
	 * 그 한 비트가 아래에서 채널 프로그램의 모양을 두 갈래로 가른다. */
	struct dasd_fba_private *private = block->base->private;
	/* [한국어] IDAL(간접 데이터 주소 목록) 낱말들을 쌓아 갈 커서. 64비트 채널 주소를
	 * 담는 타입이며, cqr->data 버퍼 안의 한 구역을 가리킨다. */
	dma64_t *idaws;
	/* [한국어] Locate Record 자료들을 쌓아 갈 커서. 역시 cqr->data 안을 가리킨다. */
	struct LO_fba_data *LO_data;
	/* [한국어] 만들어 돌려줄 CCW 요청. */
	struct dasd_ccw_req *cqr;
	/* [한국어] CCW 배열을 채워 갈 커서. cqr->cpaddr 에서 시작한다. */
	struct ccw1 *ccw;
	/* [한국어] 블록 계층 요청의 세그먼트를 훑는 반복자. */
	struct req_iterator iter;
	/* [한국어] 훑는 동안 세그먼트 하나를 담을 자리. */
	struct bio_vec bv;
	/* [한국어] 세그먼트의 커널 가상 주소. 고정 버퍼 모드에서는 바운스 버퍼 주소로 바뀐다. */
	char *dst;
	/* [한국어] count 는 요청의 총 블록 수, cidaw 는 그중 IDAL 이 필요한 블록 수,
	 * cplength 는 만들 CCW 개수, datasize 는 자료 버퍼 크기다. */
	int count, cidaw, cplength, datasize;
	/* [한국어] recid 는 지금 만들고 있는 블록의 번호, first_rec 과 last_rec 은 요청의
	 * 첫 블록과 마지막 블록 번호다. */
	sector_t recid, first_rec, last_rec;
	/* [한국어] 블록 크기와 세그먼트 안의 진행 위치. */
	unsigned int blksize, off;
	/* [한국어] 이 요청에 쓸 CCW 명령 코드. 읽기면 0x42, 쓰기면 0x41 이다. */
	unsigned char cmd;

	/* [한국어] 요청 방향을 본다. include/linux/blk-mq.h:246 의 rq_data_dir() 은
	 * op_is_write() 로 판정해 WRITE 아니면 READ 를 돌려준다. */
	if (rq_data_dir(req) == READ) {
		/* [한국어] 읽기면 FBA 의 READ 명령 코드. */
		cmd = DASD_FBA_CCW_READ;
	/* [한국어] 쓰기인지 다시 확인한다. */
	} else if (rq_data_dir(req) == WRITE) {
		/* [한국어] 쓰기면 FBA 의 WRITE 명령 코드. */
		cmd = DASD_FBA_CCW_WRITE;
	} else
		/* [한국어] **닿을 수 없는 갈래** — rq_data_dir() 이 셋째 값을 돌려주지 않기 때문이다.
		 * 그럼에도 두는 것은 cmd 가 초기화되지 않은 채로 쓰이는 일이 없게 하려는
		 * 방어이고, 컴파일러의 '초기화되지 않았을 수 있음' 경고도 막아 준다. */
		return ERR_PTR(-EINVAL);
	/* [한국어] 블록 크기를 꺼내 둔다. 아래에서 여러 번 쓰인다. */
	blksize = block->bp_block;
	/* Calculate record id of first and last block. */
	/* [한국어] 요청의 시작 섹터(512바이트 단위)를 블록 번호로 옮긴다. 나눗셈 대신
	 * 시프트를 쓸 수 있는 것은 블록 크기가 2의 거듭제곱이기 때문이다. */
	first_rec = blk_rq_pos(req) >> block->s2b_shift;
	/* [한국어] 요청의 마지막 블록 번호를 구한다.
	 * 시작 섹터에 섹터 수를 더하고 1 을 뺀 것이 마지막 섹터이고, 그것을
	 * 블록 번호로 옮긴다. **포함 구간** 이라 -1 이 필요하다. */
	last_rec =
		(blk_rq_pos(req) + blk_rq_sectors(req) - 1) >> block->s2b_shift;
	/* Check struct bio and count the number of blocks for the request. */
	/* [한국어] 총 블록 수를 셀 준비. */
	count = 0;
	/* [한국어] IDAL 이 필요한 블록 수를 셀 준비. */
	cidaw = 0;
	/* [한국어] **첫 번째 순회 — 세기만 한다.** cqr 을 얼마나 크게 잡을지 알아야
	 * 할당을 할 수 있고, 할당이 끝나야 채울 수 있어 순회가 두 번 필요하다. */
	rq_for_each_segment(bv, req, iter) {
		/* [한국어] 세그먼트 길이가 블록 크기의 배수인지 본다. blksize 가 2의 거듭제곱이라
		 * `& (blksize - 1)` 로 나머지를 얻을 수 있다. */
		if (bv.bv_len & (blksize - 1))
			/* Fba can only do full blocks. */
			/* [한국어] 옆의 상류 주석대로 FBA 는 블록 단위로만 전송할 수 있다. 어긋나면
			 * 요청을 만들 수 없으므로 -EINVAL 이다. 호출자 dasd.c:3073~3074 가 이 값을
			 * BLK_STS_INVAL 로 옮겨 재시도 없이 실패시킨다. */
			return ERR_PTR(-EINVAL);
		/* [한국어] 이 세그먼트가 몇 블록인지 더한다. 바이트 수를 (s2b_shift + 9)만큼
		 * 오른쪽으로 미는 것은 **바이트 → 512바이트 섹터(9비트) → 블록(s2b_shift)**
		 * 두 단계를 한 번에 하는 것이다. 결과적으로 bv_len / blksize 와 같다. */
		count += bv.bv_len >> (block->s2b_shift + 9);
		/* [한국어] 이 세그먼트의 버퍼를 CCW 가 직접 가리킬 수 있는지 묻는다. 못 하면
		 * IDAL 로 우회해야 한다. 판정 기준(4GB 경계를 넘는지 등)은 arch/s390 소관이라
		 * 이 트리에서 확인 못 함. */
		if (idal_is_needed (page_address(bv.bv_page), bv.bv_len))
			/* [한국어] IDAL 이 필요하면 이 세그먼트의 블록 수만큼 IDAL 낱말이 더 든다.
			 * 낱말 하나가 블록 하나를 감당한다고 보고 세는 셈이다. */
			cidaw += bv.bv_len / blksize;
	}
	/* Paranoia. */
	/* [한국어] 옆의 상류 주석이 'Paranoia' 라고 적었듯 검산이다. 세그먼트를 훑어 센
	 * 블록 수가 섹터 범위에서 계산한 블록 수와 같아야 한다. 어긋나면 요청이
	 * 망가진 것이므로 만들지 않는다. */
	if (count != last_rec - first_rec + 1)
		/* [한국어] 검산 실패. -EINVAL 로 재시도 없이 실패시킨다. */
		return ERR_PTR(-EINVAL);
	/* 1x define extent + 1x locate record + number of blocks */
	/* [한국어] CCW 개수의 기본값 — Define Extent 하나, Locate Record 하나, 그리고
	 * 블록마다 데이터 CCW 하나다. */
	cplength = 2 + count;
	/* 1x define extent + 1x locate record */
	/* [한국어] 자료 버퍼 크기의 기본값 — Define Extent 자료 16바이트, Locate Record
	 * 자료 8바이트, 그리고 IDAL 낱말들이다.
	 * IDAL 낱말 하나의 크기를 unsigned long 으로 잡는다. s390 이 64비트라
	 * 위 dma64_t 커서가 나아가는 폭과 맞아떨어진다. */
	datasize = sizeof(struct DE_fba_data) + sizeof(struct LO_fba_data) +
		cidaw * sizeof(unsigned long);
	/*
	 * Find out number of additional locate record ccws if the device
	 * can't do data chaining.
	 */
	/* [한국어] **여기서 채널 프로그램의 모양이 갈린다.** 데이터 체이닝을 못 하는
	 * 장치면 블록마다 Locate Record 를 앞세워야 한다. 위 상류 주석이 그 뜻을 밝힌다. */
	if (private->rdc_data.mode.bits.data_chain == 0) {
		/* [한국어] 이미 하나를 세어 두었으므로 블록 수에서 1 을 뺀 만큼 CCW 를 더한다. */
		cplength += count - 1;
		/* [한국어] 자료 버퍼도 같은 개수만큼 Locate Record 자료가 더 든다. */
		datasize += (count - 1)*sizeof(struct LO_fba_data);
	}
	/* Allocate the ccw request. */
	/* [한국어] 장치의 정적 메모리 풀에서 cqr 과 CCW 배열, 자료 버퍼를 **한 덩어리로**
	 * 떼어 온다. 첫 인자 DASD_FBA_MAGIC 이 cqr->magic 에 들어가 나중에
	 * 인터럽트가 이 디시플린 것인지 확인하는 데 쓰인다.
	 * dasd.c:1136 의 이 함수는 mem_lock 을 인터럽트를 막는 판으로 잡으므로
	 * 어느 문맥에서 불려도 안전하다.
	 * 다섯째 인자로 요청 뒤에 붙어 있는 드라이버 전용 영역을 넘긴다.
	 * include/linux/blk-mq.h:1015 의 blk_mq_rq_to_pdu() 는 `rq + 1` 을 돌려주며,
	 * blk-mq 태그셋이 그 자리에 cqr 하나가 들어갈 만큼을 미리 잡아 두었다.
	 * 그래서 cqr 구조체 자체는 풀에서 떼어 오지 않고 **요청에 딸린 자리를
	 * 재사용** 한다 — 풀에서는 CCW 배열과 자료 버퍼만 떼어 온다. */
	cqr = dasd_smalloc_request(DASD_FBA_MAGIC, cplength, datasize, memdev,
				   blk_mq_rq_to_pdu(req));
	/* [한국어] 풀이 말랐으면 실패한다. */
	if (IS_ERR(cqr))
		/* [한국어] 오류 포인터를 그대로 올린다. 호출자가 -ENOMEM 을 BLK_STS_RESOURCE 로
		 * 옮겨 blk-mq 에 재시도를 맡긴다. 여기까지 잡아 둔 자원이 없어 되돌릴 것도 없다. */
		return cqr;
	/* [한국어] CCW 배열의 첫 칸으로 커서를 놓는다. */
	ccw = cqr->cpaddr;
	/* First ccw is define extent. */
	/* [한국어] 첫 CCW 를 Define Extent 로 채운다. `ccw++` 로 **채운 뒤 커서를 한 칸
	 * 앞으로** 민다. 자료는 cqr->data 의 맨 앞 16바이트를 쓴다.
	 * [상류 코드 관찰] 마지막 두 인자에 **블록 번호가 아니라 512바이트 섹터
	 * 번호** 를 넘긴다. 같은 함수를 부르는 discard 경로는 블록 번호를 넘기므로
	 * 두 경로의 단위가 서로 다르다. 블록 크기가 512바이트여서 s2b_shift 가 0 이면
	 * 두 단위가 일치한다. 원본(1f0e418bb6) 487~488줄과 353줄에서 확인했으며
	 * 코드는 고치지 않았다. */
	define_extent(ccw++, cqr->data, rq_data_dir(req),
		      block->bp_block, blk_rq_pos(req), blk_rq_sectors(req));
	/* Build locate_record + read/write ccws. */
	/* [한국어] 자료 버퍼 안의 배치를 정한다. Define Extent 자료 16바이트 **뒤부터**
	 * IDAL 낱말들이 놓인다. */
	idaws = (dma64_t *)(cqr->data + sizeof(struct DE_fba_data));
	/* [한국어] IDAL 낱말 구역이 끝나는 자리부터 Locate Record 자료들이 놓인다.
	 * `idaws + cidaw` 는 포인터 산술이라 cidaw 개의 dma64_t 만큼 건너뛴다.
	 * 버퍼 배치는 결국 [DE 자료 16][IDAL 낱말 × cidaw][LO 자료 × n] 이다. */
	LO_data = (struct LO_fba_data *) (idaws + cidaw);
	/* Locate record for all blocks for smart devices. */
	/* [한국어] 데이터 체이닝을 지원하는 장치면 Locate Record 를 **요청 전체에 하나만**
	 * 둔다. 위 상류 주석이 그런 장치를 'smart devices' 라고 부른다. */
	if (private->rdc_data.mode.bits.data_chain != 0) {
		/* [한국어] **방금 채운 Define Extent CCW 에 명령 체이닝 플래그를 얹는다.**
		 * 커서는 이미 다음 칸을 가리키므로 `ccw[-1]` 로 한 칸 뒤를 짚는다.
		 * 이 패턴이 이 파일 전체에서 되풀이된다 — 다음 CCW 를 채우기 직전에
		 * 앞 CCW 를 잇는 방식이라, 사슬의 마지막에는 자연스럽게 플래그가 남지 않는다. */
		ccw[-1].flags |= CCW_FLAG_CC;
		/* [한국어] Locate Record 를 채운다. 시작 번호 0(익스텐트의 처음)과 요청 전체의
		 * 블록 수를 넣는다. 두 커서를 모두 한 칸씩 민다. */
		locate_record(ccw++, LO_data++, rq_data_dir(req), 0, count);
	}
	/* [한국어] 블록 번호 커서를 요청의 첫 블록에 놓는다. 아래에서 블록마다 하나씩 올린다. */
	recid = first_rec;
	/* [한국어] **두 번째 순회 — 실제로 채운다.** 위 첫 순회와 같은 순서로 돌아야
	 * 세어 둔 개수와 채우는 개수가 맞는다. */
	rq_for_each_segment(bv, req, iter) {
		/* [한국어] 세그먼트의 커널 가상 주소를 얻는다. 채널에 보여 줄 버퍼의 기본값이다. */
		dst = bvec_virt(&bv);
		/* [한국어] **고정 버퍼 모드일 때만** 바운스 버퍼를 쓴다. 이 전역은 커널 파라미터
		 * `dasd=fixedbuffers` 를 준 경우에만 채워지므로 기본 설정에서는 NULL 이다. */
		if (dasd_page_cache) {
			/* [한국어] DMA 가능한 페이지 하나를 슬랩 캐시에서 떼어 온다.
			 * GFP_DMA 로 채널이 닿을 수 있는 영역을 요구하고, __GFP_NOWARN 으로
			 * 실패해도 경고를 찍지 않게 한다 — 실패해도 원래 버퍼를 그대로 쓰면 되므로
			 * 치명적이지 않기 때문이다. */
			char *copy = kmem_cache_alloc(dasd_page_cache,
						      GFP_DMA | __GFP_NOWARN);
			/* [한국어] 쓰기라면 채널이 읽어 갈 데이터를 바운스 버퍼에 미리 옮겨 두어야 한다. */
			if (copy && rq_data_dir(req) == WRITE)
				/* [한국어] 세그먼트의 페이지 안 오프셋을 그대로 유지한 채 복사한다. 바운스 버퍼가
				 * 페이지 하나이고 원래 세그먼트도 페이지 안의 한 구간이라, 오프셋을 맞춰
				 * 두면 아래 주소 계산이 단순해진다. */
				memcpy(copy + bv.bv_offset, dst, bv.bv_len);
			/* [한국어] 바운스 버퍼를 얻었는지 다시 확인한다. */
			if (copy)
				/* [한국어] 얻었으면 이후로는 채널에 이 버퍼를 보여 준다. 못 얻었으면 dst 가
				 * 원래 버퍼인 채로 남아 그대로 쓰인다. */
				dst = copy + bv.bv_offset;
		}
		/* [한국어] 세그먼트를 블록 단위로 쪼갠다. 블록 하나마다 데이터 CCW 하나를 만든다. */
		for (off = 0; off < bv.bv_len; off += blksize) {
			/* Locate record for stupid devices. */
			/* [한국어] 데이터 체이닝을 못 하는 장치면 블록마다 Locate Record 를 앞세운다.
			 * 위 상류 주석이 그런 장치를 'stupid devices' 라고 부른다. */
			if (private->rdc_data.mode.bits.data_chain == 0) {
				/* [한국어] 앞 CCW 를 명령 체이닝으로 잇는다. 첫 블록이면 앞 CCW 가 Define Extent 다. */
				ccw[-1].flags |= CCW_FLAG_CC;
				/* [한국어] Locate Record 를 채운다. **여기서는 `ccw++` 를 쓰지 않는다** — 아래에서
				 * 플래그를 한 번 더 손봐야 하기 때문이다.
				 * 시작 번호는 익스텐트의 처음을 0 으로 세는 상대 번호이고, 개수는 언제나 1 이다.
				 * 이번 요청의 방향. */
				locate_record(ccw, LO_data++,
					      rq_data_dir(req),
					      recid - first_rec, 1);
				/* [한국어] **locate_record() 가 플래그를 0 으로 놓았으므로 다시 세운다.**
				 * 이 Locate Record 뒤에 곧바로 데이터 CCW 가 이어져야 하므로 명령 체이닝이
				 * 필요하다. `|=` 가 아니라 `=` 인 것은 방금 0 이 된 것을 알기 때문이다. */
				ccw->flags = CCW_FLAG_CC;
				/* [한국어] 이제 커서를 다음 칸(데이터 CCW 자리)으로 민다. */
				ccw++;
			} else {
				/* [한국어] 데이터 체이닝을 지원하는 장치의 경우. 첫 블록인지 아닌지로 잇는 방식이 갈린다. */
				if (recid > first_rec)
					/* [한국어] **두 번째 블록부터는 데이터 체이닝** 으로 잇는다. 앞 데이터 CCW 의
					 * 명령을 계속하되 버퍼만 바꾸라는 뜻이라, 제어 장치가 명령을 새로 해석하지
					 * 않아 그만큼 빠르다. */
					ccw[-1].flags |= CCW_FLAG_DC;
				else
					/* [한국어] 첫 블록이면 앞 CCW 가 Locate Record 이므로 **명령 체이닝** 으로 잇는다.
					 * 데이터 체이닝은 같은 명령을 이어 갈 때만 쓸 수 있어 여기서는 쓸 수 없다. */
					ccw[-1].flags |= CCW_FLAG_CC;
			}
			/* [한국어] 데이터 CCW 의 명령 코드를 넣는다. 위에서 정한 읽기/쓰기 값이다. */
			ccw->cmd_code = cmd;
			/* [한국어] 전송 길이는 블록 하나의 크기다. CCW 하나가 블록 하나를 나른다. */
			ccw->count = block->bp_block;
			/* [한국어] 이 버퍼를 CCW 가 직접 가리킬 수 있는지 묻는다. 첫 순회에서 세그먼트
			 * 단위로 물었던 것을 여기서는 블록 단위로 다시 묻는다. */
			if (idal_is_needed(dst, blksize)) {
				/* [한국어] 못 가리키면 IDAL 낱말 구역의 현재 위치를 CCW 에 넣는다. 채널은 이
				 * 주소에서 주소 목록을 읽어 실제 버퍼를 찾아간다. */
				ccw->cda = virt_to_dma32(idaws);
				/* [한국어] IDA 플래그를 세워 '이 cda 는 데이터가 아니라 주소 목록' 임을 알린다.
				 * `|=` 가 아니라 `=` 인 것은 이 CCW 가 아직 0 으로 초기화된 상태이기 때문이다. */
				ccw->flags = CCW_FLAG_IDA;
				/* [한국어] 실제 버퍼 주소를 IDAL 낱말로 풀어 쌓고, 커서를 그만큼 앞으로 민 값을
				 * 돌려받는다. 한 블록이 여러 낱말로 풀릴 수 있어 반환값을 다시 받는 형태다.
				 * 구현은 arch/s390 소관이라 이 트리에서 확인 못 함. */
				idaws = idal_create_words(idaws, dst, blksize);
			} else {
				/* [한국어] 직접 가리킬 수 있으면 버퍼의 채널 주소를 그대로 넣는다. */
				ccw->cda = virt_to_dma32(dst);
				/* [한국어] 플래그를 0 으로 놓는다. 체이닝 플래그는 **다음 반복이** `ccw[-1]` 로
				 * 얹어 줄 것이므로 여기서 세우지 않는다. */
				ccw->flags = 0;
			}
			/* [한국어] 다음 CCW 자리로 커서를 민다. */
			ccw++;
			/* [한국어] 버퍼 커서를 블록 하나만큼 앞으로 민다. */
			dst += blksize;
			/* [한국어] 블록 번호를 하나 올린다. 다음 반복에서 Locate Record 의 상대 번호를
			 * 계산하는 데 쓰인다. */
			recid++;
		}
	}
	/* [한국어] 요청 자체가 재시도 금지 표시를 달고 있는지 본다.
	 * include/linux/blkdev.h:704 의 이 매크로는 REQ_FAILFAST 세 비트 중
	 * 하나라도 서 있으면 참이다.
	 * 또는 장치에 failfast 기능이 켜져 있는지 본다. sysfs 의 failfast 속성으로
	 * 설정하며 dasd_devmap.c:990 이 그 값을 옮긴다. */
	if (blk_noretry_request(req) ||
	    block->base->features & DASD_FEATURE_FAILFAST)
		/* [한국어] 둘 중 하나면 cqr 에 failfast 플래그를 세운다. 그러면 오류가 났을 때
		 * 재시도하지 않고 곧바로 실패로 확정된다 — 다중 경로 소프트웨어가 위에
		 * 있을 때 유용한 동작이다. */
		set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);
	/* [한국어] 이 요청을 실제로 내보낼 장치. FBA 에는 별칭이 없으므로 언제나 기본 장치다. */
	cqr->startdev = memdev;
	/* [한국어] 이 요청의 메모리를 대 준 장치. free_cp 가 반납할 때 이 값을 쓴다. */
	cqr->memdev = memdev;
	/* [한국어] 이 요청이 속한 블록 장치. 완료 처리가 이것으로 큐를 되찾는다. */
	cqr->block = block;
	/* [한국어] 만료 시간을 초에서 지프로 옮겨 넣는다. 기본값은 dasd_int.h:717 의
	 * DASD_EXPIRES(300초)이며 옆의 상류 주석이 5분이라 적었다. 이 시간을 넘으면
	 * dasd.c 의 타이머가 요청을 끊는다. */
	cqr->expires = memdev->default_expires * HZ;	/* default 5 minutes */
	/* [한국어] 재시도 횟수. FBA 의 기본값은 이 파일 앞머리의 FBA_DEFAULT_RETRIES(32)다. */
	cqr->retries = memdev->default_retries;
	/* [한국어] 요청을 만든 시각을 TOD 시계로 찍어 둔다. 통계와 디버그에서 요청이 큐에
	 * 머문 시간을 재는 기준이 된다. */
	cqr->buildclk = get_tod_clock();
	/* [한국어] **요청을 '준비 완료' 로 표시한다.** 이 상태가 되어야 dasd.c 의
	 * tasklet 이 이 요청을 집어 채널로 내보낸다. 이 줄 앞까지는 아직 미완성이므로
	 * 순서가 중요하다. */
	cqr->status = DASD_CQR_FILLED;
	/* [한국어] 완성된 요청을 돌려준다. */
	return cqr;
}

/* [한국어]
 * dasd_fba_build_cp - I/O 제출 경로의 진입점. 요청 종류로 두 갈래를 가른다
 *
 * @memdev: cqr 을 담을 메모리를 대 줄 장치. 그대로 아래로 넘긴다.
 * @block: 대상 블록 장치. 그대로 아래로 넘긴다.
 * @req: 블록 계층이 준 요청. 이 함수가 보는 것은 req_op(req) 하나뿐이다.
 * @return: 아래 두 함수 중 하나가 돌려준 값을 그대로 넘긴다.
 *          성공이면 채워진 cqr, 실패면 ERR_PTR 로 감싼 음수 errno 다.
 *
 * **왜 필요한가**: 디시플린 vtable 의 build_cp 칸은 하나뿐인데 FBA 가 만들어야
 * 할 채널 프로그램은 두 종류다. 데이터를 실어 나르는 보통의 읽기·쓰기와,
 * 데이터를 싣지 않고 블록을 비우는 discard 다. 둘의 CCW 배치가 아예 달라
 * 함수를 나누었고, 이 한 줄짜리 갈림길이 vtable 쪽 얼굴 노릇을 한다.
 *
 * 동작 과정: req_op(req) 가 REQ_OP_DISCARD 이거나 REQ_OP_WRITE_ZEROES 이면
 * discard 판으로, 그 밖이면 일반 판으로 보낸다. 두 종류를 한데 묶은 것은
 * FBA 에서 둘의 처리가 완전히 같기 때문이다 — 어차피 지워진 블록은 0 을
 * 읽어 주므로 '버려라' 와 '0 으로 채워라' 가 같은 일이 된다.
 *
 * 실행 컨텍스트: blk-mq 제출 경로. do_dasd_request() 가 dq->lock 을 쥔 채
 * 부르므로 잠들 수 없다. 여러 하드웨어 큐에서 동시에 들어올 수 있으나
 * 다루는 자료가 전부 요청별이라 재진입에 안전하다.
 *
 * caller: dasd.c:3067 이 discipline->build_cp 로 부른다.
 * callee: 이 파일의 dasd_fba_build_cp_discard() 와 dasd_fba_build_cp_regular(),
 * 그리고 include/linux/blk-mq.h:229 의 req_op().
 *
 * 에러 경로: 이 함수 자체에는 없다. 아래에서 나온 ERR_PTR 를 그대로 올린다.
 *
 * 호출 체인:
 *   blk-mq → do_dasd_request() → discipline->build_cp == [이 함수]
 *     → dasd_fba_build_cp_discard() 또는 dasd_fba_build_cp_regular()
 */
static struct dasd_ccw_req *dasd_fba_build_cp(struct dasd_device *memdev,
					      struct dasd_block *block,
					      struct request *req)
{
	/* [한국어] 요청 종류를 본다. include/linux/blk-mq.h:229 의 req_op() 가 cmd_flags 에서
	 * 연산 코드를 꺼낸다. **버리기와 0 채우기를 한데 묶는 이유** 는 FBA 에서
	 * 둘의 처리가 완전히 같기 때문이다 — 버려진 블록은 다시 읽으면 0 을 주므로
	 * '버려라' 와 '0 으로 채워라' 가 같은 일이 된다. */
	if (req_op(req) == REQ_OP_DISCARD || req_op(req) == REQ_OP_WRITE_ZEROES)
		/* [한국어] 데이터를 나르지 않는 요청이므로 discard 전용 경로로 보낸다. */
		return dasd_fba_build_cp_discard(memdev, block, req);
	else
		/* [한국어] 그 밖(읽기·쓰기)은 일반 경로로 보낸다. */
		return dasd_fba_build_cp_regular(memdev, block, req);
}

/* [한국어]
 * dasd_fba_free_cp - 끝난 요청을 정리하고 cqr 을 정적 풀에 돌려준다
 *
 * @cqr: 완료(또는 실패)한 요청.
 * @req: 그 요청의 바탕이 된 블록 계층 요청. 바운스 버퍼를 되돌릴 때 쓴다.
 * @return: cqr 의 최종 상태가 DASD_CQR_DONE 이면 1, 아니면 0. 호출자
 *          dasd.c:2694~2716 이 이 값을 보고 blk-mq 에 알릴 오류를 정한다 —
 *          1 이면 오류 없음, 0 이면 cqr->intrc 를 살펴 BLK_STS_TIMEOUT 이나
 *          BLK_STS_IOERR 등으로 옮긴다. 음수를 돌려주는 갈래는 없다.
 *
 * **왜 필요한가**: build_cp 의 짝이다. build_cp 가 정적 풀에서 떼어 온 cqr 을
 * 여기서 돌려주지 않으면 풀이 말라 다음 I/O 를 만들 수 없다. 그리고
 * '고정 버퍼 모드' 에서 쓴 바운스 버퍼가 있다면, 읽기 결과를 원래 버퍼로
 * 옮겨 담고 그 바운스 버퍼도 반납해야 한다.
 *
 * **두 부분으로 나뉜 함수** 다. 앞부분은 고정 버퍼 모드에서만 도는 되돌리기
 * 작업이고, 뒷부분(out 라벨 이후)은 언제나 도는 cqr 반납이다.
 *
 * 고정 버퍼 모드란: 커널 파라미터로 `dasd=fixedbuffers` 를 주면
 * dasd_devmap.c:256 이 페이지 크기의 DMA 슬랩 캐시를 하나 만든다. 그러면
 * build_cp 가 블록 계층 버퍼 대신 그 캐시에서 떼어 온 페이지를 채널에
 * 보여 주고, 완료 후 이 함수가 결과를 원래 버퍼로 옮긴다. 캐시가 만들어지지
 * 않았으면(기본값) 그 전역이 NULL 이라 이 함수는 곧바로 out 으로 뛴다.
 *
 * 동작 과정(고정 버퍼 모드일 때):
 *   (1) Define Extent CCW 를 건너뛴다.
 *   (2) 데이터 체이닝이 되는 장치면 Locate Record CCW 도 하나 건너뛴다.
 *   (3) 세그먼트를 훑으며 블록마다 CCW 를 하나씩 따라간다. 데이터 체이닝을
 *       못 하는 장치면 블록마다 Locate Record 를 하나씩 더 건너뛴다.
 *   (4) CCW 가 가리키는 실제 버퍼 주소를 꺼낸다. IDAL 을 쓴 CCW 면 한 겹
 *       더 들어가야 한다.
 *   (5) 그 주소가 원래 버퍼와 다르면 바운스 버퍼를 쓴 것이므로, 읽기였다면
 *       내용을 원래 버퍼로 옮기고 페이지를 슬랩 캐시에 돌려준다.
 *
 * 실행 컨텍스트: softirq(tasklet) 또는 프로세스 컨텍스트. dasd.c:2694 는
 * 블록 tasklet 안이고 dasd.c:3878 은 요청 재큐잉 경로다. 잠들 수 없다.
 *
 * caller: dasd.c:2694 의 __dasd_cleanup_cqr() 과 dasd.c:3878 의
 * dasd_generic_requeue_all_requests(). 둘 다 discipline->free_cp 를 거친다.
 * callee: include/linux/blk-mq.h 의 rq_for_each_segment()/rq_data_dir(),
 * memcpy(), kmem_cache_free(), dasd.c:1221 의 dasd_sfree_request(),
 * 그리고 arch/s390 의 주소 변환.
 *
 * 에러 경로: 없다. 어떤 상태의 cqr 이 와도 반납은 반드시 이루어진다.
 *
 * **주의**: (1)~(5)의 CCW 건너뛰기 셈은 dasd_fba_build_cp_regular() 가 만든
 * 배치를 전제로 한다. discard 경로의 배치는 다르지만, 기본 설정에서는
 * 고정 버퍼 캐시가 없어 그 코드에 닿지 않는다.
 *
 * 호출 체인:
 *   dasd.c 의 블록 tasklet → __dasd_cleanup_cqr()
 *     → discipline->free_cp == [이 함수] → dasd_sfree_request()
 */
static int
dasd_fba_free_cp(struct dasd_ccw_req *cqr, struct request *req)
{
	/* [한국어] 장치의 사적 상태를 꺼낸다. cqr->block->base 로 두 단계를 거슬러 올라가는
	 * 이유는, cqr 은 별칭 장치로 나갈 수 있어도 사적 상태는 언제나 기본 장치에
	 * 붙어 있기 때문이다(FBA 에는 별칭이 없지만 코드 관례를 따른다).
	 * 여기서 필요한 것은 데이터 체이닝 비트 하나뿐이다. */
	struct dasd_fba_private *private = cqr->block->base->private;
	/* [한국어] CCW 사슬을 훑을 커서. */
	struct ccw1 *ccw;
	/* [한국어] 블록 계층 요청의 세그먼트를 훑는 반복자. include/linux/blk-mq.h:1092 의
	 * 매크로가 이것을 쓴다. */
	struct req_iterator iter;
	/* [한국어] 훑는 동안 세그먼트 하나를 담을 자리. */
	struct bio_vec bv;
	/* [한국어] dst 는 블록 계층이 준 원래 버퍼 주소, cda 는 CCW 가 실제로 가리킨 주소다.
	 * 둘이 다르면 바운스 버퍼를 쓴 것이다. */
	char *dst, *cda;
	/* [한국어] 블록 크기와 세그먼트 안의 진행 위치. */
	unsigned int blksize, off;
	/* [한국어] 돌려줄 값 — 요청이 정상 완료였는지 여부다. */
	int status;

	/* [한국어] **고정 버퍼 모드가 아니면 여기서 곧바로 정리로 뛴다.**
	 * 이 전역은 커널 파라미터 `dasd=fixedbuffers` 를 준 경우에만
	 * dasd_devmap.c:256 이 만들어 채우므로, 기본 설정에서는 NULL 이다.
	 * 즉 아래의 되돌리기 코드는 평소에는 한 줄도 실행되지 않는다. */
	if (!dasd_page_cache)
		goto out;
	/* [한국어] 블록 크기를 꺼내 둔다. 세그먼트를 블록 단위로 쪼개 훑는 데 쓴다. */
	blksize = cqr->block->bp_block;
	/* [한국어] CCW 사슬의 첫 칸으로 커서를 놓는다. */
	ccw = cqr->cpaddr;
	/* Skip over define extent & locate record. */
	/* [한국어] 첫 CCW 는 Define Extent 이므로 무조건 건너뛴다. */
	ccw++;
	/* [한국어] 데이터 체이닝을 지원하는 장치였다면 build_cp 가 Locate Record 를 딱 하나
	 * 만들어 두 번째 칸에 놓았다. */
	if (private->rdc_data.mode.bits.data_chain != 0)
		/* [한국어] 그 Locate Record 도 건너뛴다. 이제 커서는 첫 데이터 CCW 를 가리킨다. */
		ccw++;
	/* [한국어] 요청의 세그먼트를 build_cp 와 **같은 순서로** 다시 훑는다. 같은 순서라야
	 * CCW 커서와 세그먼트가 짝이 맞는다. */
	rq_for_each_segment(bv, req, iter) {
		/* [한국어] 세그먼트의 커널 가상 주소를 얻는다. build_cp 가 이 주소를 CCW 에 넣었거나,
		 * 바운스 버퍼를 썼다면 다른 주소를 넣었을 것이다. */
		dst = bvec_virt(&bv);
		/* [한국어] 세그먼트를 블록 단위로 쪼갠다. 블록 하나에 데이터 CCW 하나가 대응한다. */
		for (off = 0; off < bv.bv_len; off += blksize) {
			/* Skip locate record. */
			/* [한국어] 데이터 체이닝을 못 하는 장치였다면 build_cp 가 블록마다 Locate Record 를
			 * 앞세웠다. */
			if (private->rdc_data.mode.bits.data_chain == 0)
				/* [한국어] 그 Locate Record 를 건너뛴다. */
				ccw++;
			/* [한국어] dst 가 아직 살아 있을 때만 처리한다. 아래에서 한 번 처리한 뒤 NULL 로
			 * 비우므로, **세그먼트마다 첫 블록에서 한 번만** 이 안으로 들어온다.
			 * 바운스 버퍼가 세그먼트 단위(페이지 하나)로 잡히기 때문이다. */
			if (dst) {
				/* [한국어] 이 CCW 가 IDAL 을 통해 데이터를 가리키는지 본다. */
				if (ccw->flags & CCW_FLAG_IDA)
					/* [한국어] IDAL 이면 한 겹 더 들어간다 — CCW 의 주소는 64비트 주소 목록을 가리키고,
					 * 그 목록의 첫 칸이 실제 데이터 주소다. 안쪽부터 읽으면
					 * 'CCW 의 32비트 채널 주소를 가상 주소로 → 그 자리를 dma64_t 로 읽음 →
					 * 그 64비트 값을 다시 가상 주소로' 다. */
					cda = dma64_to_virt(*((dma64_t *)dma32_to_virt(ccw->cda)));
				else
					/* [한국어] IDAL 이 아니면 CCW 의 주소가 곧 데이터 주소다. 채널 주소를 커널 가상
					 * 주소로 되돌리기만 하면 된다. */
					cda = dma32_to_virt(ccw->cda);
				/* [한국어] CCW 가 가리킨 주소가 원래 버퍼와 다르면 바운스 버퍼를 쓴 것이다.
				 * 같으면 바운스가 없었으므로 할 일이 없다(고정 버퍼 캐시에서 페이지를
				 * 얻지 못한 경우가 그렇다). */
				if (dst != cda) {
					/* [한국어] 읽기였다면 채널이 바운스 버퍼에 담아 온 데이터를 원래 버퍼로 옮겨야 한다.
					 * 쓰기였다면 반대 방향의 복사를 build_cp 가 이미 했으므로 여기서는 할 일이 없다. */
					if (rq_data_dir(req) == READ)
						/* [한국어] 세그먼트 길이만큼 통째로 옮긴다. **블록 단위가 아니라 세그먼트 단위** 이며,
						 * 그래서 위에서 세그먼트마다 한 번만 이 안으로 들어오게 해 둔 것이다. */
						memcpy(dst, cda, bv.bv_len);
					/* [한국어] 바운스 페이지를 슬랩 캐시에 돌려준다.
					 * 캐시에 돌려줄 때는 **페이지의 시작 주소** 여야 한다. cda 는 세그먼트의
					 * 오프셋만큼 안쪽을 가리킬 수 있으므로 PAGE_MASK 로 하위 비트를 잘라 낸다.
					 * build_cp 가 `copy + bv.bv_offset` 을 CCW 에 넣었던 것의 되돌리기다. */
					kmem_cache_free(dasd_page_cache,
					    (void *)((addr_t)cda & PAGE_MASK));
				}
				/* [한국어] 이 세그먼트는 처리했다고 표시한다. 남은 블록들의 CCW 는 건너뛰기만 한다. */
				dst = NULL;
			}
			/* [한국어] 다음 데이터 CCW 로 커서를 옮긴다. */
			ccw++;
		}
	}
out:
	/* [한국어] **반환값을 정한다.** 최종 상태가 완료면 1, 그 밖(실패·취소 등)이면 0 이다.
	 * 호출자 dasd.c:2695~2716 은 0 일 때 cqr->intrc 를 살펴 blk-mq 에 올릴 오류를
	 * 고르고, 1 이면 정상 완료로 처리한다. 음수를 돌려주는 갈래가 없으므로
	 * 호출자의 `status < 0` 검사에는 걸리지 않는다. */
	status = cqr->status == DASD_CQR_DONE;
	/* [한국어] cqr 이 차지하던 덩어리를 장치의 정적 메모리 풀에 돌려준다.
	 * **이 줄 뒤로 cqr 을 건드리면 안 되므로** 반환값을 위에서 미리 꺼내 두었다.
	 * dasd.c:1221 의 이 함수는 mem_lock 을 인터럽트를 막는 판으로 잡고 덩어리를
	 * 합쳐 넣으며, 장치 참조 계수도 하나 내린다. */
	dasd_sfree_request(cqr, cqr->memdev);
	/* [한국어] 미리 꺼내 둔 상태 값을 돌려준다. */
	return status;
}

/* [한국어]
 * dasd_fba_handle_terminated_request - 중간에 끊긴 요청을 다시 시도할지 정한다
 *
 * @cqr: 상태가 DASD_CQR_TERMINATED 인 요청. 만료나 경로 소실로 채널에서
 *       강제로 끊긴 것이다.
 * @return: 없다. cqr->status 를 바꾸는 것으로 결과를 알린다.
 *
 * **왜 필요한가**: 요청이 정상적으로 실패한 것이 아니라 **드라이버가 끊은**
 * 경우는 따로 다뤄야 한다. 센스 데이터가 없을 수 있어 보통의 오류 복구를
 * 걸 수 없기 때문이다. 디시플린 vtable 의 handle_terminated_request 칸이
 * 그 판정을 맡는다.
 *
 * 동작 과정:
 *   (1) 남은 재시도 횟수가 음수면 더 시도할 여지가 없으므로 요청을
 *       DASD_CQR_FAILED 로 확정한다. 그러면 위층이 blk-mq 에 I/O 오류를 올린다.
 *   (2) 아직 남아 있으면 DASD_CQR_FILLED 로 되돌린다. 그 상태는 '아직
 *       내보내지 않은, 준비된 요청' 을 뜻하므로 다음 tasklet 순회에서
 *       처음부터 다시 내보내진다.
 *
 * **재시도 횟수를 여기서 줄이지 않는다** 는 점이 눈에 띈다. 감소는 요청을
 * 내보내는 쪽에서 이루어지고, 이 함수는 이미 줄어든 값을 읽기만 한다.
 *
 * 실행 컨텍스트: softirq(tasklet) 또는 프로세스 컨텍스트. 잠들 수 없다.
 *
 * caller: dasd.c:2243 의 __dasd_sleep_on_erp() 와 dasd.c:2766 의
 * __dasd_process_block_ccw_queue(). 둘 다
 * discipline->handle_terminated_request 를 거친다.
 * callee: 없다.
 *
 * 에러 경로: 없다. 두 갈래 모두 정상 흐름이다.
 *
 * [상류 코드 관찰] 위 dasd_fba_check_for_device_change() 와 마찬가지로
 * 함수 본문의 닫는 중괄호 뒤에 불필요한 세미콜론이 붙어 있다.
 * 원본(1f0e418bb6) 614줄에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   dasd.c 의 블록 tasklet → __dasd_process_block_ccw_queue()
 *     → discipline->handle_terminated_request == [이 함수]
 */
static void dasd_fba_handle_terminated_request(struct dasd_ccw_req *cqr)
{
	/* [한국어] 남은 재시도 횟수가 **음수** 인지 본다. 0 이 아니라 음수를 기준으로 삼는
	 * 이유는, 요청을 내보낼 때마다 하나씩 줄이므로 마지막 시도 뒤에 -1 이 되기
	 * 때문이다. 즉 0 은 '마지막 한 번이 아직 남았다' 가 아니라 '방금 마지막을
	 * 썼다' 에 해당하고, 그 뒤 한 번 더 줄어야 음수가 된다. */
	if (cqr->retries < 0)
		/* [한국어] 더 시도할 여지가 없으므로 요청을 최종 실패로 확정한다. 그러면 위층이
		 * blk-mq 에 I/O 오류를 올린다. */
		cqr->status = DASD_CQR_FAILED;
	else
		/* [한국어] 아직 남았으므로 '준비된 요청' 상태로 되돌린다. 다음 tasklet 순회에서
		 * 같은 CCW 사슬이 처음부터 다시 내보내진다. 채널 프로그램 자체는 손대지
		 * 않는데, build_cp 가 만든 CCW 가 재사용 가능하기 때문이다. */
		cqr->status = DASD_CQR_FILLED;
};

/* [한국어]
 * dasd_fba_fill_info - 사용자 공간이 물어보는 장치 정보를 채운다
 *
 * @device: 대상 장치. private 를 통해 장치 특성에 닿는다.
 * @info: 채울 사용자 공간 구조체. 호출자가 커널 쪽 임시 버퍼로 마련해 둔다.
 * @return: 언제나 0. 실패하는 갈래가 없다.
 *
 * **왜 필요한가**: 사용자 공간 도구(dasdfmt, fdasd 등)가 BIODASDINFO 계열
 * ioctl 로 '이 볼륨이 어떤 형식인지' 를 물어 온다. 그 답을 채우는 것이
 * 디시플린 vtable 의 fill_info 칸이며, FBA 판이 이 함수다.
 *
 * 동작 과정:
 *   (1) 레이블이 있는 블록 번호를 1 로 알린다. FBA 볼륨은 블록 0 이 아니라
 *       **블록 1 에 볼륨 레이블** 이 놓인다.
 *   (2) FBA 배치임을 알린다.
 *   (3) 볼륨 형식을 LDL(Linux Disk Layout)로 알린다. FBA 에는 OS/390 호환
 *       배치(CDL)라는 개념이 없어 언제나 이 값이다.
 *   (4) 장치 특성의 크기와 내용을 통째로 복사해 넘긴다. **읽는 코드가 없던
 *       필드들도 여기서 사용자에게 전달된다.**
 *   (5) 구성 데이터 크기를 0 으로 알린다. FBA 에는 ECKD 의 Read Configuration
 *       Data 에 해당하는 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ioctl 경로에서만 불린다. 잠들 수 있다.
 *
 * caller: dasd_ioctl.c:513 이 discipline->fill_info 로 부른다.
 * callee: memcpy().
 *
 * 에러 경로: 없다.
 *
 * 이 정보가 실제로 쓰이는 곳: block/partitions/ibm.c 가 이 ioctl 결과를 보고
 * DASD 볼륨의 파티션을 해석한다. 그 파일이 info->format 이 DASD_FORMAT_LDL
 * 인지, FBA_layout 이 서 있는지, label_block 이 몇인지를 본다.
 *
 * 호출 체인:
 *   사용자 공간의 ioctl → dasd_ioctl.c 의 정보 조회
 *     → discipline->fill_info == [이 함수]
 */
static int
dasd_fba_fill_info(struct dasd_device * device,
		   struct dasd_information2_t * info)
{
	/* [한국어] 장치 특성을 꺼내려고 사적 상태를 잡는다. */
	struct dasd_fba_private *private = device->private;

	/* [한국어] **볼륨 레이블이 놓인 블록 번호를 1 로 알린다.** FBA 볼륨은 블록 0 이
	 * 아니라 블록 1 에 레이블이 있다. block/partitions/ibm.c 가 이 값을 보고
	 * 어느 블록을 읽어 볼지 정한다. */
	info->label_block = 1;
	/* [한국어] FBA 배치임을 알린다. 파티션 해석기가 CKD 배치와 다른 규칙을 쓰게 하는 표시다. */
	info->FBA_layout = 1;
	/* [한국어] 볼륨 형식을 LDL(Linux Disk Layout)로 알린다. **FBA 에는 CDL(OS/390 호환
	 * 배치)이라는 개념이 없어 언제나 이 값이다.** ECKD 는 같은 자리에서
	 * uses_cdl 여부를 보고 두 값 중 하나를 고른다(dasd_eckd.c:5011).
	 * DASD_FORMAT_LDL 의 정의는 uapi 헤더 소관이라 이 트리에서 확인 못 함. */
	info->format = DASD_FORMAT_LDL;
	/* [한국어] 넘겨줄 장치 특성의 바이트 수. struct dasd_fba_characteristics 의 크기이므로 32 다. */
	info->characteristics_size = sizeof(private->rdc_data);
	/* [한국어] 장치 특성을 통째로 복사한다. **이름으로 읽는 코드가 없던 필드들도
	 * 여기서 사용자 공간에 전달된다** — 커널이 쓰지 않을 뿐 정보 자체는 버리지 않는다.
	 * 복사 길이. 위 characteristics_size 와 같은 값을 다시 계산한다. */
	memcpy(info->characteristics, &private->rdc_data,
	       sizeof(private->rdc_data));
	/* [한국어] 구성 데이터는 없다고 알린다. ECKD 는 Read Configuration Data 로 받은
	 * 256바이트를 여기 실어 보내지만, **FBA 에는 그런 명령이 없다.** */
	info->confdata_size = 0;
	return 0;
}

/* [한국어]
 * dasd_fba_dump_sense_dbf - 센스 데이터를 s390 디버그 기능 링버퍼에 짧게 남긴다
 *
 * @device: 오류가 난 장치. 장치별 디버그 영역을 갖고 있다.
 * @irb: 인터럽트 응답 블록. 상태 바이트와 센스 데이터가 들어 있다.
 * @reason: 어느 자리에서 불렀는지 나타내는 짧은 문자열. 호출자가 넘기며
 *          "int"(인터럽트), "uc"(원인 불명 인터럽트), "log"(오류 기록) 셋이 온다.
 * @return: 없다.
 *
 * **왜 필요한가**: 아래 dasd_fba_dump_sense() 는 페이지를 잡아 사람이 읽을
 * 글을 만들어 커널 로그에 찍는 무거운 함수라, 오류가 날 때마다 부를 수 없다.
 * 그래서 **가볍고 언제나 도는 기록** 을 따로 둔 것이 이 함수다. 고정 형식의
 * 한 줄을 s390 디버그 기능의 링버퍼에 넣을 뿐이라 메모리 할당이 없다.
 *
 * 동작 과정:
 *   (1) irb 에서 센스 데이터의 주소를 꺼낸다.
 *   (2) 있으면 상태 세 값(부속 상태 코드, 채널 상태, 장치 상태)과 센스
 *       데이터 32바이트를 8바이트 낱말 넷으로 한 줄에 찍는다. 전송 모드
 *       인터럽트인지 아닌지도 "t"/"c" 한 글자로 남긴다.
 *   (3) 없으면 그 사실만 남긴다.
 *
 * 기록 등급이 DBF_EMERG 다. 디버그 기능의 등급 문턱을 넘겨 **반드시 남도록**
 * 가장 높은 등급을 쓴다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트를 포함한다.** dasd.c:1664 와 1812 는
 * dasd_int_handler() 안이고, dasd_erp.c 의 dasd_log_sense_dbf() 는 tasklet
 * 쪽이다. 잠들 수 없고, 그래서 메모리를 잡지 않는 설계다.
 *
 * caller: dasd.c:1664, dasd.c:1812, 그리고 dasd_erp.c 의 dasd_log_sense_dbf().
 * 모두 discipline->dump_sense_dbf 를 거친다.
 * callee: dasd.c:3986 의 dasd_get_sense(), DBF_DEV_EVENT 매크로를 거쳐
 * s390 디버그 기능의 기록 함수, 그리고 irb 접근자들(arch/s390, 이 트리에 없음).
 *
 * 에러 경로: 센스 데이터가 없으면 그 사실을 남기고 돌아간다. 그것이
 * 유일한 갈래다.
 *
 * 호출 체인:
 *   dasd.c 의 dasd_int_handler() → discipline->dump_sense_dbf == [이 함수]
 *     → dasd_get_sense() → DBF_DEV_EVENT
 */
static void
dasd_fba_dump_sense_dbf(struct dasd_device *device, struct irb *irb,
			char *reason)
{
	/* [한국어] 센스 데이터를 8바이트 낱말 넷으로 보기 위한 포인터. 실제 센스 데이터는
	 * 바이트 배열이지만, 한 줄에 짧게 찍으려고 낱말 단위로 읽는다. */
	u64 *sense;

	/* [한국어] irb 안 어디에 센스 데이터가 있는지는 인터럽트의 종류(전송 모드인지 아닌지)에
	 * 따라 다르다. 그 판별을 dasd.c:3986 의 dasd_get_sense() 가 대신해 주고,
	 * 없으면 NULL 을 돌려준다. */
	sense = (u64 *) dasd_get_sense(irb);
	/* [한국어] 센스 데이터가 있을 때만 상세 형식으로 찍는다. */
	if (sense) {
		/* [한국어] 등급 DBF_EMERG 로 한 줄을 남긴다. 디버그 기능은 등급이 문턱보다 낮으면
		 * 버리므로, 오류 기록이 반드시 남도록 가장 높은 등급을 쓴다.
		 * device->debug_area 는 장치마다 따로 있는 링버퍼다.
		 * 형식 문자열의 앞부분 — 호출 자리를 나타내는 문자열, 인터럽트 종류 한 글자,
		 * 그리고 상태 세 값을 2자리 16진수로 붙여 찍는다. */
		DBF_DEV_EVENT(DBF_EMERG, device,
			      "%s: %s %02x%02x%02x %016llx %016llx %016llx "
			      /* [한국어] 형식 문자열의 뒷부분과 첫 인자. reason 은 호출자가 준 "int"/"uc"/"log" 중 하나로,
			       * 같은 링버퍼에 섞여 남는 기록들을 구분하는 꼬리표다.
			       * 센스 데이터의 나머지 두 낱말. 넷을 합쳐 32바이트다 — irb 의 센스 영역이
			       * 그만큼이며, 그보다 짧은 센스가 왔더라도 뒤쪽은 이전 값이 남아 있을 수 있다.
			       * 장치 상태(device status)와 센스 데이터의 앞 두 낱말. 장치 상태는
			       * 제어 장치가 보고한 문제(유닛 검사, 장치 종료 등)를 나타낸다.
			       * 상태 코드 세 값 중 앞의 둘 — 부속 상태 코드(condition code)와
			       * 채널 상태(channel status)다. 채널 상태는 채널 쪽에서 생긴 문제
			       * (프로그램 검사, 보호 검사 등)를 나타낸다.
			       * 전송 모드(zHPF) 인터럽트면 "t", 보통의 CCW 인터럽트면 "c" 를 찍는다.
			       * **FBA 는 전송 모드를 쓰지 않으므로 실제로는 언제나 "c" 여야 한다.**
			       * 그럼에도 이 판정을 두는 것은 ECKD 의 같은 함수와 형식을 맞추기 위해서다.
			       * scsw_is_tm() 의 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
			      "%016llx", reason,
			      scsw_is_tm(&irb->scsw) ? "t" : "c",
			      scsw_cc(&irb->scsw), scsw_cstat(&irb->scsw),
			      scsw_dstat(&irb->scsw), sense[0], sense[1],
			      sense[2], sense[3]);
	} else {
		/* [한국어] 센스 데이터가 없는 경우. 상태 값도 찍지 않고 그 사실만 남긴다.
		 * 고정 문자열. 끝의 개행은 DBF_DEV_EVENT 매크로가 형식 문자열에 개행을
		 * 한 번 더 붙이므로 결과적으로 두 번 들어간다. */
		DBF_DEV_EVENT(DBF_EMERG, device, "%s",
			      "SORRY - NO VALID SENSE AVAILABLE\n");
	}
}


/* [한국어]
 * dasd_fba_dump_sense - 실패한 요청의 센스 데이터와 채널 프로그램을 사람이 읽게 찍는다
 *
 * @device: 오류가 난 장치. 커널 로그에 장치 이름을 붙이는 데 쓴다.
 * @req: 실패한 요청. cpaddr 로 채널 프로그램의 시작을 찾는다.
 * @irb: 인터럽트 응답 블록. 상태 바이트, 센스 데이터, 실패한 CCW 의 주소가
 *       들어 있다.
 * @return: 없다.
 *
 * **왜 필요한가**: 위 dasd_fba_dump_sense_dbf() 가 남기는 한 줄은 링버퍼를
 * 따로 꺼내 봐야 읽을 수 있다. 그와 별개로, 관리자가 dmesg 만 보고도 무슨
 * 일이 있었는지 알 수 있게 **읽을 수 있는 보고서** 를 찍는 것이 이 함수다.
 * 비용이 크므로 언제나 도는 것이 아니라, 장치에 ERPLOG 기능이 켜져 있거나
 * (dasd.c:1666) 요청이 최종 실패했을 때(dasd_erp.c 의 dasd_log_sense)만 돈다.
 *
 * 찍는 내용은 네 덩어리다.
 *   (1) I/O 상태 보고 — 요청 주소, 채널 상태, 장치 상태, 실패한 CCW 의 주소.
 *   (2) 센스 데이터 32바이트를 8바이트씩 네 줄로.
 *   (3) 채널 프로그램의 **앞쪽** CCW 들과 각 CCW 가 가리키는 데이터 앞부분.
 *   (4) **실패한 CCW 주변** 과 **사슬의 끝** CCW 들. 앞에서 이미 찍은 자리를
 *       건너뛰었으면 "......" 로 생략을 표시한다.
 *
 * 동작 과정에서 눈여겨볼 점 셋:
 *   - 페이지 하나를 GFP_ATOMIC 으로 잡아 그 위에 글을 쌓고, 덩어리마다
 *     dev_err() 로 한 번에 내보낸 뒤 len 을 0 으로 되돌려 페이지를 재사용한다.
 *   - 사슬의 끝은 '체이닝 플래그가 없는 첫 CCW' 로 찾는다. 본문이 빈
 *     for 루프가 그 일을 한다.
 *   - CCW 가 가리키는 데이터는 최대 32바이트까지, 4바이트씩 끊어 찍는다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트를 포함한다.** dasd.c:1667 은
 * dasd_int_handler() 안이다. 그래서 페이지를 GFP_ATOMIC 으로 잡고, 실패하면
 * 포기한다. 잠들 수 없다.
 *
 * caller: dasd.c:1667 과 dasd_erp.c 의 dasd_log_sense(). 둘 다
 * discipline->dump_sense 를 거친다.
 * callee: get_zeroed_page(), sprintf(), dev_err(), free_page(),
 * 그리고 arch/s390 의 주소 변환. 후자는 이 트리에 없다.
 *
 * 에러 경로: 페이지를 잡지 못하면 디버그 로그에 그 사실만 남기고 곧바로
 * 돌아간다. 그 밖에는 실패할 여지가 없다.
 *
 * **보안상의 주의**: 형식 지정자 %px 를 쓴다. 커널 주소를 가리지 않고 그대로
 * 찍는다는 뜻이며, 이 정보가 없으면 CCW 사슬을 따라갈 수 없어 진단이
 * 불가능하기 때문이다. 그래서 이 출력은 dmesg 를 읽을 권한이 있는 사람에게만
 * 보인다는 전제 위에 서 있다.
 *
 * [상류 코드 관찰] 상류 주석은 '앞쪽 CCW 를 최대 8개 찍는다' 고 적었지만,
 * 끝을 `act + 8` 로 잡고 반복 조건을 `act <= end` 로 두어 실제로는 최대
 * 9개를 찍는다. 원본(1f0e418bb6) 693줄, 696줄, 698줄에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 호출 체인:
 *   dasd.c 의 dasd_int_handler() → discipline->dump_sense == [이 함수]
 *     → get_zeroed_page() → sprintf() → dev_err() → free_page()
 */
static void
dasd_fba_dump_sense(struct dasd_device *device, struct dasd_ccw_req * req,
		    struct irb *irb)
{
	/* [한국어] 채널 프로그램을 훑을 세 포인터 — act 는 지금 찍고 있는 CCW,
	 * end 는 이번 덩어리의 마지막, last 는 사슬 전체의 마지막이다. */
	struct ccw1 *act, *end, *last;
	/* [한국어] 페이지에 쌓인 글의 길이(len), 센스 줄 번호(sl), 줄 안의 바이트 번호(sct),
	 * 그리고 CCW 데이터 덤프의 바이트 위치(count)다. */
	int len, sl, sct, count;
	/* [한국어] dev_err() 에 넘길 장치 포인터. 커널 로그에 "dasd-fba 0.0.xxxx:" 같은
	 * 접두사를 붙여 준다. */
	struct device *dev;
	/* [한국어] 글을 쌓을 임시 버퍼. 페이지 하나를 통째로 쓴다. */
	char *page;

	/* [한국어] ccw_device 안의 struct device 를 꺼낸다. 이 함수 전체에서 dev_err() 의
	 * 첫 인자로 쓰인다. */
	dev = &device->cdev->dev;

	/* [한국어] **GFP_ATOMIC** 으로 페이지를 잡는다. 이 함수가 dasd.c:1667 을 통해
	 * 인터럽트 컨텍스트에서 불릴 수 있어 잠들 수 없기 때문이다. 그래서
	 * 할당이 실패할 수 있고, 아래에서 그 경우를 다룬다. */
	page = (char *) get_zeroed_page(GFP_ATOMIC);
	/* [한국어] 페이지를 못 잡았으면 글을 쌓을 자리가 없다. */
	if (page == NULL) {
		/* [한국어] 상세 보고는 포기하고 디버그 링버퍼에 그 사실만 남긴다. 여기서는
		 * dev_err() 를 쓰지 않는데, 페이지 없이도 찍을 수는 있지만 이미 메모리가
		 * 말라 있는 상황이라 로그 부담을 더하지 않으려는 선택으로 보인다. */
		DBF_DEV_EVENT(DBF_WARNING, device, "%s",
			      "No memory to dump sense data");
		return;
	}
	/* [한국어] 첫 덩어리를 시작한다. **len 을 += 가 아니라 = 로 놓아** 페이지의 처음부터
	 * 다시 쓴다. sprintf 는 쓴 글자 수를 돌려주므로 그것이 곧 다음에 이어 쓸 위치다. */
	len = sprintf(page, "I/O status report:\n");
	/* [한국어] 실패한 요청의 주소와 채널 상태(CS), 장치 상태(DS)를 찍는다.
	 * **%px 는 커널 주소를 가리지 않고 그대로 찍는 형식** 이다. 아래에서 CCW
	 * 주소들과 대조해야 하므로 가린 값(%p)으로는 쓸모가 없다.
	 * 인자 셋. irb->scsw.cmd 는 인터럽트 상태 낱말의 CCW 모드 판이며,
	 * 그 안의 cstat/dstat 가 위 CS/DS 로 찍힌다. */
	len += sprintf(page + len, "in req: %px CS: 0x%02X DS: 0x%02X\n",
		       req, irb->scsw.cmd.cstat, irb->scsw.cmd.dstat);
	/* [한국어] **실패한 CCW 의 주소** 를 찍는다. 아래 셋째 덩어리가 이 주소 둘레를
	 * 다시 훑을 때 기준이 되는 값이다.
	 * cpa(Channel Program Address)는 32비트 채널 주소라 그대로는 포인터가 아니다.
	 * 32비트 값으로 꺼낸 뒤 u64 를 거쳐 포인터로 캐스팅한다. 이 변환들의 정의는
	 * arch/s390 소관이라 이 트리에서 확인 못 함. */
	len += sprintf(page + len, "Failing CCW: %px\n",
		       (void *)(u64)dma32_to_u32(irb->scsw.cmd.cpa));
	/* [한국어] 센스 데이터가 실려 왔는지(concurrent sense)를 나타내는 비트를 본다.
	 * esw0 는 확장 상태 낱말의 형식 0 판이고, erw 는 그 안의 확장 보고 낱말,
	 * cons 가 '센스 있음' 비트다. 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
	if (irb->esw.esw0.erw.cons) {
		/* [한국어] 센스 데이터 32바이트를 8바이트씩 네 줄로 나눠 찍는다. */
		for (sl = 0; sl < 4; sl++) {
			/* [한국어] 줄머리에 이 줄이 담는 바이트 범위를 적는다.
			 * 줄 번호에서 시작 바이트와 끝 바이트를 계산한다 — 0-7, 8-15, 16-23, 24-31 이다. */
			len += sprintf(page + len, "Sense(hex) %2d-%2d:",
				       (8 * sl), ((8 * sl) + 7));

			/* [한국어] 한 줄에 여덟 바이트를 찍는다. */
			for (sct = 0; sct < 8; sct++) {
				/* [한국어] 바이트 하나를 2자리 16진수로 이어 붙인다.
				 * ecw(Extended Control Word) 배열이 센스 데이터가 실려 오는 자리다.
				 * 줄 번호 × 8 + 줄 안 위치로 인덱스를 만든다. */
				len += sprintf(page + len, " %02x",
					       irb->ecw[8 * sl + sct]);
			}
			/* [한국어] 한 줄을 마치고 개행을 붙인다. */
			len += sprintf(page + len, "\n");
		}
	} else {
		/* [한국어] 센스 데이터가 없으면 그 사실만 한 줄 적는다. 위 dasd_fba_dump_sense_dbf()
		 * 가 쓰는 문구와 같다. */
		len += sprintf(page + len, "SORRY - NO VALID SENSE AVAILABLE\n");
	}
	/* [한국어] 첫 덩어리를 커널 로그에 오류 등급으로 내보낸다. 형식 문자열을 "%s" 로
	 * 두고 내용을 인자로 넘기는 것은, 페이지 안에 %기호가 들어 있어도 형식
	 * 지정자로 해석되지 않게 하려는 안전 조치다. */
	dev_err(dev, "%s", page);

	/* dump the Channel Program */
	/* print first CCWs (maximum 8) */
	/* [한국어] 채널 프로그램의 첫 CCW 로 커서를 놓는다. cqr->cpaddr 이 build_cp 가 만든
	 * CCW 배열의 시작이다. */
	act = req->cpaddr;
	/* [한국어] **사슬의 끝을 찾는다.** 본문이 비어 있는 for 루프이며, 체이닝 플래그
	 * (CC 또는 DC)가 서 있는 동안 last 를 앞으로 민다. 플래그가 없는 CCW 가
	 * 사슬의 마지막이므로, 루프가 멈춘 자리의 last 가 곧 마지막 CCW 를 가리킨다.
	 * build_cp 가 마지막 CCW 에는 체이닝 플래그를 얹지 않는다는 규약에 기댄 코드다. */
	for (last = act; last->flags & (CCW_FLAG_CC | CCW_FLAG_DC); last++);
	/* [한국어] 첫 덩어리에서 찍을 마지막 CCW 를 정한다. 시작에서 여덟 칸 뒤와 사슬 끝
	 * 중 앞의 것이다.
	 * [상류 코드 관찰] 위 상류 주석은 '최대 8개' 라고 적었지만, 끝을 act + 8 로
	 * 잡고 아래 반복 조건을 `act <= end` 로 두었으므로 실제로는 **최대 9개** 를
	 * 찍는다. 원본(1f0e418bb6) 693, 696, 698줄에서 확인했으며 코드는 고치지 않았다. */
	end = min(act + 8, last);
	/* [한국어] 둘째 덩어리를 시작한다. len 을 = 로 놓아 페이지를 처음부터 재사용한다 —
	 * 앞 덩어리는 이미 로그에 나갔으므로 덮어써도 된다. */
	len = sprintf(page, "Related CP in req: %px\n", req);
	/* [한국어] 첫 CCW 부터 end 까지 훑는다. end 를 **포함** 한다. */
	while (act <= end) {
		/* [한국어] CCW 하나의 주소와 그 8바이트를 32비트 낱말 둘로 날것 그대로 찍는다.
		 * struct ccw1 을 int 배열로 겹쳐 읽는다. 필드 이름으로 찍지 않고 낱말로
		 * 찍는 이유는, 명령 코드·플래그·길이·주소가 어떻게 배치돼 있는지를 그대로
		 * 보여 주어 채널 서브시스템 쪽 문서와 대조할 수 있게 하려는 것이다. */
		len += sprintf(page + len, "CCW %px: %08X %08X DAT:",
			       act, ((int *) act)[0], ((int *) act)[1]);
		/* [한국어] 이 CCW 가 가리키는 데이터의 앞부분을 최대 32바이트까지 찍는다.
		 * 전송 길이가 그보다 짧으면 그만큼만 찍는다. */
		for (count = 0; count < 32 && count < act->count;
		     /* [한국어] 4바이트씩 나아간다. */
		     count += sizeof(int))
			/* [한국어] 데이터 낱말 하나를 8자리 16진수로 이어 붙인다.
			 * CCW 의 데이터 주소를 채널 주소에서 커널 가상 주소로 되돌려 int 배열로 읽는다.
			 * **이 CCW 가 IDAL 을 쓰는 경우는 걸러 내지 않는다** — 그때 이 주소는 데이터가
			 * 아니라 주소 목록을 가리키므로, 찍히는 값이 데이터가 아닌 주소들이다.
			 * 진단용 덤프라 그대로 두었다. */
			len += sprintf(page + len, " %08X",
				       ((int *)dma32_to_virt(act->cda))
				       /* [한국어] 바이트 위치를 4로 나눠 낱말 인덱스로 바꾼다. 시프트를 쓰는 것은
				        * count 가 언제나 4의 배수이기 때문이다. */
				       [(count>>2)]);
		/* [한국어] CCW 한 줄을 마친다. */
		len += sprintf(page + len, "\n");
		/* [한국어] 다음 CCW 로 커서를 옮긴다. */
		act++;
	}
	/* [한국어] 둘째 덩어리를 내보낸다. */
	dev_err(dev, "%s", page);

	/* print failing CCW area */
	/* [한국어] 셋째 덩어리를 시작한다. **여기서는 = 대신 0 을 직접 넣고** 아래에서 += 로
	 * 쌓는다. 앞의 두 덩어리와 달리 첫 sprintf 가 조건 안에 있어, 조건이 거짓이면
	 * 아무것도 쓰지 않은 채로 다음 단계로 가야 하기 때문이다. */
	len = 0;
	/* [한국어] 지금 커서가 실패한 CCW 의 두 칸 앞보다도 더 앞에 있는지 본다. 그렇다면
	 * 그 사이를 건너뛰어야 한다. */
	if (act < ((struct ccw1 *)dma32_to_virt(irb->scsw.cmd.cpa)) - 2) {
		/* [한국어] 커서를 실패한 CCW 의 두 칸 앞으로 옮긴다. 실패 지점의 앞뒤 맥락을
		 * 보여 주려는 것이다. */
		act = ((struct ccw1 *)dma32_to_virt(irb->scsw.cmd.cpa)) - 2;
		/* [한국어] 건너뛴 구간이 있음을 점 여섯 개로 표시한다. */
		len += sprintf(page + len, "......\n");
	}
	/* [한국어] 이번 덩어리의 끝을 정한다 — 실패한 CCW 의 두 칸 뒤와 사슬 끝 중 앞의 것이다. */
	end = min((struct ccw1 *)dma32_to_virt(irb->scsw.cmd.cpa) + 2, last);
	/* [한국어] 실패 지점 둘레를 훑는다. 아래 본문은 둘째 덩어리와 글자 그대로 같다. */
	while (act <= end) {
		/* [한국어] CCW 의 주소와 8바이트를 찍는다.
		 * 위와 같이 int 배열로 겹쳐 읽는다. */
		len += sprintf(page + len, "CCW %px: %08X %08X DAT:",
			       act, ((int *) act)[0], ((int *) act)[1]);
		/* [한국어] 이 CCW 가 가리키는 데이터를 최대 32바이트까지. */
		for (count = 0; count < 32 && count < act->count;
		     /* [한국어] 4바이트씩 나아간다. */
		     count += sizeof(int))
			/* [한국어] 데이터 낱말 하나.
			 * 채널 주소를 커널 가상 주소로 되돌린다. */
			len += sprintf(page + len, " %08X",
				       ((int *)dma32_to_virt(act->cda))
				       /* [한국어] 바이트 위치를 낱말 인덱스로. */
				       [(count>>2)]);
		/* [한국어] 한 줄을 마친다. */
		len += sprintf(page + len, "\n");
		/* [한국어] 다음 CCW 로. */
		act++;
	}

	/* print last CCWs */
	/* [한국어] 마지막 덩어리 — 사슬의 끝 세 CCW 를 보여 준다. 커서가 아직 그보다 앞에
	 * 있으면 사이를 건너뛴다. */
	if (act <  last - 2) {
		/* [한국어] 커서를 사슬 끝의 두 칸 앞으로 옮긴다. */
		act = last - 2;
		/* [한국어] 건너뛴 구간 표시. **len 을 0 으로 되돌리지 않고 이어 쓴다** — 셋째 덩어리와
		 * 넷째 덩어리가 한 번의 dev_err() 로 함께 나가기 때문이다. */
		len += sprintf(page + len, "......\n");
	}
	/* [한국어] 사슬의 마지막 CCW 까지 훑는다. last 를 포함한다. */
	while (act <= last) {
		/* [한국어] CCW 의 주소와 8바이트를 찍는다.
		 * int 배열로 겹쳐 읽는다. */
		len += sprintf(page + len, "CCW %px: %08X %08X DAT:",
			       act, ((int *) act)[0], ((int *) act)[1]);
		/* [한국어] 데이터를 최대 32바이트까지. */
		for (count = 0; count < 32 && count < act->count;
		     /* [한국어] 4바이트씩. */
		     count += sizeof(int))
			/* [한국어] 데이터 낱말 하나.
			 * 채널 주소를 커널 가상 주소로. */
			len += sprintf(page + len, " %08X",
				       ((int *)dma32_to_virt(act->cda))
				       /* [한국어] 낱말 인덱스. */
				       [(count>>2)]);
		/* [한국어] 한 줄을 마친다. */
		len += sprintf(page + len, "\n");
		/* [한국어] 다음 CCW 로. */
		act++;
	}
	/* [한국어] 셋째와 넷째 덩어리에서 쓴 글이 있을 때만 내보낸다. 실패한 CCW 가 이미
	 * 둘째 덩어리 안에 들어 있었다면 여기 쌓인 글이 없을 수 있다. */
	if (len > 0)
		/* [한국어] 마지막 덩어리를 내보낸다. */
		dev_err(dev, "%s", page);
	/* [한국어] 임시 페이지를 반납한다. 위에서 잡은 가상 주소를 unsigned long 으로
	 * 되돌려 넘긴다. 이 함수의 모든 정상 경로가 여기를 지난다. */
	free_page((unsigned long) page);
}

/* [한국어]
 * dasd_fba_max_sectors - 요청 하나에 담을 수 있는 최대 섹터 수를 알린다
 *
 * @block: 대상 블록 장치. 시프트 폭을 여기서 읽는다.
 * @return: 512바이트 섹터 단위의 상한. DASD_FBA_MAX_BLOCKS(96)에 블록 하나가
 *          몇 섹터인지를 곱한 값이다.
 *
 * **왜 필요한가**: build_cp 가 블록 하나마다 CCW 를 하나씩 만들므로, 요청이
 * 커질수록 CCW 배열과 정적 메모리 풀의 소모가 커진다. 그 상한을 블록 계층에
 * 미리 알려 **애초에 그보다 큰 요청이 만들어지지 않게** 하는 것이 이 함수다.
 *
 * 동작 과정: 블록 수 상한을 시프트 폭만큼 왼쪽으로 밀어 섹터 수로 바꾼다.
 * 블록 크기가 512 면 96 섹터(48KB), 4096 이면 768 섹터(384KB)가 된다.
 *
 * 호출자가 이 값을 어떻게 쓰는지: dasd.c:321~322 가 큐 한계의
 * max_dev_sectors 와 max_hw_sectors 에 그대로 넣는다. 그 뒤로 blk-mq 는
 * 이보다 큰 요청을 병합하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 장치 상태를 basic 에서 ready 로 올릴 때
 * 딱 한 번 불린다.
 *
 * caller: dasd.c:321 이 discipline->max_sectors 로 부른다. 그 위는
 * dasd_state_basic_to_ready() 다.
 * callee: 없다.
 *
 * 에러 경로: 없다.
 *
 * **이 칸은 비워 둘 수 없다.** dasd.c:321 이 NULL 검사 없이 부르기 때문이다.
 * FBA, ECKD, DIAG 세 디시플린이 모두 이 칸을 채우는 이유다.
 *
 * 호출 체인:
 *   dasd_state_basic_to_ready() → discipline->max_sectors == [이 함수]
 */
static unsigned int dasd_fba_max_sectors(struct dasd_block *block)
{
	/* [한국어] 블록 수 상한 96 을 s2b_shift 만큼 왼쪽으로 밀어 512바이트 섹터 수로 바꾼다.
	 * 나눗셈·곱셈 대신 시프트를 쓸 수 있는 이유는 블록 크기가 언제나 2의 거듭제곱
	 * 이기 때문이다. 블록 크기 512 면 96(48KB), 4096 이면 768(384KB)이 된다. */
	return DASD_FBA_MAX_BLOCKS << block->s2b_shift;
}

/* [한국어]
 * dasd_fba_pe_handler - 경로 상태가 바뀌었을 때 경로를 다시 검증한다
 *
 * @device: 대상 장치.
 * @tbvpm: 검증해야 할 경로들의 비트마스크(to-be-verified path mask).
 *         8개 경로가 한 바이트에 담기며 경로 0 이 최상위 비트다.
 * @fcsecpm: 파이버 채널 암호화 관련 경로 마스크. **이 함수는 쓰지 않는다.**
 * @return: dasd_generic_verify_path() 의 반환값. 그 함수는 언제나 0 이다.
 *
 * **왜 필요한가**: 채널 경로가 살아나거나 죽으면 채널 서브시스템이 알려 오고,
 * 드라이버는 그 경로를 실제로 쓸 수 있는지 확인해 사용 가능 경로 마스크(opm)를
 * 갱신해야 한다. 그 확인 방법이 디시플린마다 다를 수 있어 vtable 에
 * pe_handler 칸이 있다.
 *
 * **FBA 는 확인을 하지 않는다.** 공통 함수에 그대로 넘기며, 그 함수는
 * CCW 를 하나도 내보내지 않고 넘겨받은 마스크를 그냥 사용 가능으로 표시한다.
 * ECKD 는 같은 자리에서 경로 그룹을 설정하고 구성 데이터를 다시 읽고
 * 별칭 구성을 갱신하는 훨씬 큰 일을 한다.
 *
 * 동작 과정: 한 단계뿐이다 — 인자 둘 중 앞의 것만 넘겨 공통 함수를 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(작업 큐). dasd.c:2045 는
 * dasd_device_tasklet 이 아니라 경로 검증 작업 안에서 불린다.
 *
 * caller: dasd.c:2045 가 discipline->pe_handler 로 부른다.
 * callee: dasd.c:3797 의 dasd_generic_verify_path().
 *
 * 에러 경로: 없다. 아래 함수가 언제나 0 을 돌려준다.
 *
 * [상류 코드 관찰] 세 번째 인자 fcsecpm 을 쓰지 않는다. 시그니처는 vtable 이
 * 정한 형태라 받기만 한다. dasd_diag.c 의 같은 자리 함수도 같은 모양이다.
 * 원본(1f0e418bb6) 756~760줄에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   경로 이벤트 → dasd.c 의 경로 검증 → discipline->pe_handler == [이 함수]
 *     → dasd_generic_verify_path()
 */
static int dasd_fba_pe_handler(struct dasd_device *device,
			       __u8 tbvpm, __u8 fcsecpm)
{
	/* [한국어] 세 번째 인자 fcsecpm 은 버리고 tbvpm 만 공통 함수에 넘긴다. 그 함수는
	 * CCW 를 하나도 내보내지 않고 넘겨받은 마스크를 사용 가능 경로(opm)에 얹기만
	 * 한다 — 즉 FBA 는 경로를 '검증' 하지 않고 '믿는다'. */
	return dasd_generic_verify_path(device, tbvpm);
}

/* [한국어] **이 파일의 결선판** — FBA 디시플린의 가상 함수표.
 * dasd_int.h:972 의 struct dasd_discipline 은 50개가 넘는 칸을 갖는데,
 * 여기서 채우는 것은 **20개** 뿐이다(ECKD 는 53개, DIAG 는 16개).
 * 비어 있는 칸 하나하나가 'FBA 에는 그 개념이 없다' 는 선언이다. 대표적으로
 * format_device 와 check_device_format(포맷 명령), ioctl(디시플린 전용 ioctl),
 * get_uid 와 kick_validate(PAV 별칭), is_ese 와 ext_pool 계열(씬 프로비저닝),
 * pprc 계열(원격 복제), hpf 계열(고성능 FICON), device_ping, hosts_print 가 없다.
 * 호출자들은 대개 부르기 전에 NULL 인지 확인한다 — 예를 들어 dasd.c:356 은
 * basic_to_ready 를, dasd_ioctl.c:198 은 format_device 를, dasd_devmap.c:2373 은
 * device_ping 을 검사한 뒤 부른다.
 * 파일 앞머리(dasd_fba.c 의 첫 전역 선언)에서 이 이름을 미리 선언해 둔 이유가
 * 여기서 드러난다 — dasd_fba_set_online() 이 이 표의 주소를 써야 하는데
 * 정의는 파일 맨 끝에 있기 때문이다.
 * [상류 코드 관찰] check_attention 칸을 비워 두었는데, dasd.c:1676 은 장치
 * 상태에 Attention 비트가 서 있으면 **NULL 검사 없이** 그 칸을 부른다.
 * 같은 자리를 ECKD 는 채우고 DIAG 는 비워 둔다(DIAG 는 dasd.c:1633 에서
 * 그 앞에 걸러진다). 원본(1f0e418bb6) 762~783줄과 dasd.c 에서 확인했으며
 * 코드는 고치지 않았다. */
static struct dasd_discipline dasd_fba_discipline = {
	/* [한국어] 이 디시플린을 담은 모듈. 장치가 이 디시플린을 쓰는 동안 모듈이 내려가지
	 * 않도록 dasd_generic_set_online() 이 이 포인터로 참조를 잡는다. */
	.owner = THIS_MODULE,
	/* [한국어] ASCII 이름. 커널 로그와 sysfs 출력에 그대로 쓰인다. 8바이트 배열에
	 * 4글자와 공백 하나가 들어가며 뒤는 0 으로 채워진다. */
	.name = "FBA ",
	/* [한국어] EBCDIC 이름 — **여기서는 아직 ASCII 다.** dasd_fba_init() 이 모듈 적재 때
	 * 이 4바이트를 그 자리에서 EBCDIC 으로 바꾼다. 바뀐 뒤의 값이
	 * DASD_FBA_MAGIC(0xC6C2C140)과 같아져, dasd.c:1687 의 인터럽트 소유자 확인이
	 * 통과한다. */
	.ebcname = "FBA ",
	/* [한국어] **이 디렉터리에서 이 칸을 참으로 두는 유일한 디시플린이다.**
	 * dasd.c:330 이 이 값을 보고 블록 큐에 discard 한계
	 * (discard_granularity, max_hw_discard_sectors, max_write_zeroes_sectors)를
	 * 설정한다. 즉 이 한 줄이 있어야 블록 계층이 REQ_OP_DISCARD 요청을 보내 준다. */
	.has_discard = true,
	/* [한국어] 장치 인식. dasd.c:3502 가 온라인 경로에서 부른다. */
	.check_device = dasd_fba_check_characteristics,
	/* [한국어] 볼륨 분석. dasd.c:310 이 basic → ready 전이에서 부른다.
	 * 이 칸은 NULL 이어도 되며(dasd.c:309 가 검사한다) DIAG 는 비워 둔다. */
	.do_analysis = dasd_fba_do_analysis,
	/* [한국어] 경로 이벤트 처리. dasd.c:2045 가 NULL 검사 없이 부르므로 반드시 채워야 한다. */
	.pe_handler = dasd_fba_pe_handler,
	/* [한국어] 요청 크기 상한. dasd.c:321 이 NULL 검사 없이 부르므로 반드시 채워야 한다. */
	.max_sectors = dasd_fba_max_sectors,
	/* [한국어] HDIO_GETGEO 용 지오메트리. dasd.c:3287 이 NULL 인지 검사한 뒤 부른다. */
	.fill_geometry = dasd_fba_fill_geometry,
	/* [한국어] **공통 구현을 그대로 꽂는다.** dasd.c:1328 의 dasd_start_IO() 는 cqr 을
	 * 채널 서브시스템에 넘기는 일반 함수이며, FBA 는 그 위에 더할 것이 없다.
	 * ECKD 도 같은 함수를 쓰고, DIAG 만 자기 것을 쓴다(DIAG 는 CCW 가 아니라
	 * z/VM 진단 명령으로 I/O 를 내기 때문이다). */
	.start_IO = dasd_start_IO,
	/* [한국어] 마찬가지로 공통 구현. dasd.c:1270 의 dasd_term_IO() 가 진행 중인 요청을
	 * 채널에서 끊는다. */
	.term_IO = dasd_term_IO,
	/* [한국어] 끊긴 요청의 재시도 여부 판정. */
	.handle_terminated_request = dasd_fba_handle_terminated_request,
	/* [한국어] 복구 함수 선택. 언제나 기본 재시도를 돌려준다. */
	.erp_action = dasd_fba_erp_action,
	/* [한국어] 복구 뒷정리 함수 선택. */
	.erp_postaction = dasd_fba_erp_postaction,
	/* [한국어] 인터럽트가 상태 변화 신호인지 판정. dasd.c:1807 이 이 칸이 NULL 이면
	 * 원인 불명 인터럽트 처리를 통째로 건너뛴다. */
	.check_for_device_change = dasd_fba_check_for_device_change,
	/* [한국어] **I/O 경로의 핵심.** dasd.c:3067 이 요청마다 부른다. */
	.build_cp = dasd_fba_build_cp,
	/* [한국어] 요청 정리. dasd.c:2694 가 완료마다 부른다. */
	.free_cp = dasd_fba_free_cp,
	/* [한국어] 센스 데이터를 사람이 읽게 찍기. */
	.dump_sense = dasd_fba_dump_sense,
	/* [한국어] 센스 데이터를 디버그 링버퍼에 찍기. dasd.c:1811 이 NULL 인지 검사한 뒤
	 * 부르는 자리도 있고, dasd.c:1664 처럼 검사 없이 부르는 자리도 있다. */
	.dump_sense_dbf = dasd_fba_dump_sense_dbf,
	/* [한국어] 사용자 공간에 알릴 장치 정보 채우기. dasd_ioctl.c:510 이 NULL 검사 후 부른다. */
	.fill_info = dasd_fba_fill_info,
/* [한국어] 표의 끝. 여기까지 20개 칸을 채웠고 나머지는 정적 초기화 규칙에 따라
 * 전부 0(즉 NULL 이거나 거짓)이다. */
};

/* [한국어]
 * dasd_fba_init - 모듈이 올라올 때 FBA 디시플린을 시스템에 등록한다
 *
 * @return: 0 이면 등록 성공이다. 0 페이지를 잡지 못하면 -ENOMEM 이고,
 *          드라이버 등록이 실패하면 그 오류다. 0 이 아니면 모듈이 올라오지 않는다.
 *
 * **왜 필요한가**: 이 파일은 커널 모듈 하나를 이루며, 그 진입점이다.
 * 세 가지를 해 둔다 — 디시플린 이름을 EBCDIC 으로 바꾸고, discard 경로가 쓸
 * 0 페이지를 잡고, ccw 드라이버를 등록한다.
 *
 * 동작 과정:
 *   (1) 디시플린의 ebcname 필드 4바이트를 그 자리에서 EBCDIC 으로 바꾼다.
 *       정적 초기화 때는 ASCII 문자열 "FBA " 로 들어 있고, 여기서 바뀐다.
 *       **왜 필요한가**: dasd.c:1687 이 인터럽트마다
 *       `strncmp(discipline->ebcname, (char *)&cqr->magic, 4)` 로 요청이
 *       이 디시플린 것인지 확인하는데, 그 magic 값 DASD_FBA_MAGIC 이
 *       0xC6C2C140 — 즉 'F','B','A',' ' 의 **EBCDIC 바이트 그대로** 이기 때문이다.
 *       변환하지 않으면 이 비교가 언제나 어긋나 모든 인터럽트가 버려진다.
 *   (2) 0 으로 채운 페이지를 DMA 가능한 메모리로 하나 잡는다.
 *       ccw_write_zero() 가 데이터 원본으로 쓰는 페이지다. 모듈 수명 동안
 *       **읽기 전용으로만 쓰이므로** 모든 FBA 장치가 함께 써도 안전하다.
 *   (3) ccw 드라이버를 등록한다. 그 순간부터 dasd_fba_ids 표에 맞는 장치가
 *       나타나면 dasd_generic_probe() 가 불린다.
 *   (4) 등록에 성공했으면 장치 탐색이 끝날 때까지 기다린다. 그래야 모듈이
 *       올라온 직후 곧바로 장치를 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 모듈 적재(insmod) 또는 부팅 시 초기화
 * 호출이며, GFP_KERNEL 로 메모리를 잡고 (4)에서 잠든다.
 *
 * caller: 커널의 모듈 초기화. 파일 맨 끝의 module_init() 이 등록한다.
 * callee: ASCEBC(arch/s390, 이 트리에 없음), get_zeroed_page(),
 * ccw_driver_register(), wait_for_device_probe().
 *
 * 에러 경로: 0 페이지를 잡지 못하면 -ENOMEM 을 돌려주고 끝낸다. 드라이버
 * 등록이 실패하면 그 오류를 돌려주는데, **이때 이미 잡아 둔 0 페이지를
 * 풀지 않는다** — 모듈 적재가 실패해 정리 함수도 불리지 않으므로 그 페이지가
 * 남는다. 실무상으로는 드라이버 등록이 실패하는 일이 드물다.
 *
 * 호출 체인:
 *   insmod / 커널 초기화 → [이 함수] → ccw_driver_register()
 *     → (장치가 있으면) dasd_generic_probe()
 */
static int __init
dasd_fba_init(void)
{
	/* [한국어] 드라이버 등록 결과를 담을 변수. */
	int ret;

	/* [한국어] 디시플린 이름 4바이트를 **그 자리에서** ASCII → EBCDIC 으로 바꾼다.
	 * 정적 초기화 때 "FBA " 라는 ASCII 로 들어 있던 것이 여기서 0xC6 0xC2 0xC1 0x40
	 * 이 되며, 그 값이 곧 dasd_int.h:271 의 DASD_FBA_MAGIC 이다. 이 변환이 없으면
	 * dasd.c:1687 의 `strncmp(discipline->ebcname, (char *)&cqr->magic, 4)` 가 언제나
	 * 어긋나 이 디시플린으로 만든 요청의 인터럽트가 전부 버려진다.
	 * 길이를 sizeof 가 아니라 4 로 못 박은 이유는 배열이 8바이트인데 이름이
	 * 4글자뿐이라, 뒤쪽 0 바이트까지 변환하면 안 되기 때문이다.
	 * ASCEBC 의 정의는 arch/s390 소관이라 이 트리에서 확인 못 함. */
	ASCEBC(dasd_fba_discipline.ebcname, 4);

	/* [한국어] discard 경로가 쓸 **0 으로 채운 페이지** 를 하나 잡는다.
	 * GFP_DMA 를 붙이는 이유는 채널이 이 페이지를 직접 읽어 가야 하기 때문이며,
	 * s390 에서 그 영역이 어디까지인지는 arch/s390 소관이라 이 트리에서 확인 못 함.
	 * 모듈 수명 동안 **한 번도 쓰이지 않고 읽히기만 하므로** 모든 FBA 장치가
	 * 동시에 이 페이지를 가리켜도 경쟁이 생기지 않는다 — 그래서 잠금이 없다. */
	dasd_fba_zero_page = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	/* [한국어] 페이지를 못 잡았으면 discard 를 만들 수 없으므로 모듈 적재 자체를 포기한다. */
	if (!dasd_fba_zero_page)
		return -ENOMEM;

	/* [한국어] ccw 버스에 이 드라이버를 등록한다. 이 순간부터 dasd_fba_ids 표에 맞는
	 * 장치가 나타나면 버스가 .probe(dasd_generic_probe)를 불러 준다. */
	ret = ccw_driver_register(&dasd_fba_driver);
	/* [한국어] 등록에 성공했을 때만 다음 줄로 간다. 실패했으면 그 오류를 그대로 돌려준다. */
	if (!ret)
		/* [한국어] 이미 버스에 붙어 있는 장치들의 탐색이 끝날 때까지 기다린다.
		 * 이것이 없으면 modprobe 가 돌아온 직후에도 장치가 아직 나타나지 않아,
		 * 곧바로 볼륨을 열려는 부팅 스크립트가 실패할 수 있다. */
		wait_for_device_probe();

	/* [한국어] 등록 결과를 그대로 돌려준다. 0 이 아니면 모듈이 올라오지 않는다. */
	return ret;
}

/* [한국어]
 * dasd_fba_cleanup - 모듈이 내려갈 때 등록을 풀고 0 페이지를 반납한다
 *
 * @return: 없다.
 *
 * **왜 필요한가**: 위 dasd_fba_init() 이 잡아 둔 두 자원을 되돌린다.
 * 순서가 중요하다 — 드라이버 등록을 먼저 풀어야 새 I/O 가 들어오지 않고,
 * 그다음에야 0 페이지를 반납해도 안전하다.
 *
 * 동작 과정:
 *   (1) ccw 드라이버 등록을 푼다. 그 안에서 이 드라이버가 맡고 있던 장치들이
 *       전부 오프라인이 되고 dasd_generic_remove() 가 불린다. 즉 이 한 줄이
 *       돌아올 때는 진행 중인 I/O 가 남아 있지 않다.
 *   (2) 0 페이지를 커널에 돌려준다. (1)이 끝난 뒤이므로 이 페이지를
 *       가리키는 CCW 가 남아 있지 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 모듈 제거(rmmod) 경로이며 (1)에서 잠들 수 있다.
 *
 * caller: 커널의 모듈 정리. 파일 맨 끝의 module_exit() 이 등록한다.
 * callee: ccw_driver_unregister(), free_page().
 *
 * 에러 경로: 없다. 두 호출 모두 실패하지 않는다.
 *
 * 호출 체인:
 *   rmmod → [이 함수] → ccw_driver_unregister() → dasd_generic_remove()
 */
static void __exit
dasd_fba_cleanup(void)
{
	/* [한국어] 드라이버 등록을 먼저 푼다. 그 안에서 이 드라이버가 맡던 장치들이 모두
	 * 오프라인이 되므로, 이 줄이 돌아올 때는 진행 중인 CCW 사슬이 남아 있지 않다.
	 * **순서가 중요하다** — 이것을 먼저 하지 않으면 아래에서 반납한 0 페이지를
	 * 가리키는 CCW 가 아직 실행 중일 수 있다. */
	ccw_driver_unregister(&dasd_fba_driver);
	/* [한국어] 0 페이지를 커널에 돌려준다. 위에서 전역에 담아 둔 가상 주소를
	 * unsigned long 으로 되돌려 넘긴다. */
	free_page((unsigned long)dasd_fba_zero_page);
}

/* [한국어] 모듈 진입점을 등록한다. 모듈로 빌드되면 insmod 때, 커널에 내장되면
 * 부팅 초기화 때 위 함수가 불린다. */
module_init(dasd_fba_init);
/* [한국어] 모듈 정리점을 등록한다. rmmod 때 위 함수가 불린다. 커널에 내장된
 * 경우에는 불리지 않는다. */
module_exit(dasd_fba_cleanup);
