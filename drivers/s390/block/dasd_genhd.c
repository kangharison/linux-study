// SPDX-License-Identifier: GPL-2.0
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *		    Horst Hummel <Horst.Hummel@de.ibm.com>
 *		    Carsten Otte <Cotte@de.ibm.com>
 *		    Martin Schwidefsky <schwidefsky@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2001
 *
 * gendisk related functions for the dasd driver.
 *
 */

/* [한국어] [한국어 설명] DASD 를 사용자 공간의 블록 장치로 세우는 gendisk·파티션 계층 (dasd_genhd.c)
 * 
 * === 파일의 역할 ===
 * struct dasd_block 하나가 /dev/dasda 같은 **눈에 보이는 디스크** 가 되는 자리다.
 * 233줄이지만 이 드라이버가 리눅스 블록 계층과 맞닿는 접점 전부가 여기 있다.
 * 하는 일은 다섯이다.
 * (1) 주번호 등록과 해제 — dasd_gendisk_init() 이 DASD 전용 주번호를 잡고,
 * dasd_gendisk_exit() 이 놓는다. 모듈이 실릴 때와 내려갈 때 한 번씩만 불린다.
 * (2) blk-mq 태그 집합과 gendisk 만들기 — dasd_gendisk_alloc() 이 요청 큐,
 * 하드웨어 큐 개수, 큐 깊이, 세그먼트 제약을 정하고 디스크를 등록한다.
 * (3) 장치 이름 짓기 — dasd_name_format() 이 장치 색인 번호를 dasda, dasdaa,
 * dasdaaa … 처럼 알파벳 자리 올림 표기로 바꾼다.
 * (4) 파티션 스캔과 제거 — dasd_scan_partitions() 이 디스크를 스스로 한 번 열어
 * 파티션 표를 읽게 하고, dasd_destroy_partitions() 이 그 반대를 한다.
 * (5) 그 뒷정리 — dasd_gd_free() 와 dasd_gendisk_free().
 * **이 파일에는 I/O 를 처리하는 코드가 한 줄도 없다.** 요청을 실제로 다루는
 * 콜백 표(dasd_mq_ops)와 블록 장치 연산표(dasd_device_operations)는 dasd.c 에
 * 있고, 여기서는 그 주소를 태그 집합과 gendisk 에 꽂아 주기만 한다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 장치 상태 기계의 **BASIC 과 READY 두 단계에 걸쳐** 불린다. dasd_int.h 가
 * 설명하는 다섯 단계(NEW → KNOWN → BASIC → READY → ONLINE) 가운데,
 * KNOWN → BASIC 으로 올라갈 때 디스크가 만들어지고 BASIC → READY 로 올라갈 때
 * 파티션이 인식된다. 내려올 때는 정확히 그 반대다.
 * 
 *   dasd.c 의 dasd_state_known_to_basic()   [프로세스 컨텍스트]
 *     → dasd_gendisk_alloc()(220줄)  ─ 태그 집합 + gendisk 생성, 디스크 등록
 *   dasd.c 의 dasd_state_basic_to_ready()
 *     → 볼륨 분석 → set_capacity() → dasd_scan_partitions()(349줄)
 *   dasd.c 의 dasd_state_ready_to_basic()
 *     → dasd_destroy_partitions()(388줄)
 *   dasd.c 의 dasd_state_basic_to_known()
 *     → dasd_gendisk_free()(264줄)
 * 
 * 디스크가 등록된 뒤의 I/O 흐름에서 이 파일이 정한 값들이 계속 살아 움직인다.
 * 
 *   사용자 공간 → 파일시스템 → blk-mq
 *     → dasd_mq_ops.queue_rq(= dasd.c 의 do_dasd_request)
 *       → 큐의 queuedata 에서 struct dasd_block 을 되찾는다(dasd.c:3021·3114)
 *         → discipline->build_cp() 가 cqr 을 만든다
 *           → 두 태스클릿을 거쳐 채널로 나간다
 * 
 * 그 queuedata 를 꽂아 주는 곳이 이 파일의 dasd_gendisk_alloc() 이다. 즉 blk-mq
 * 요청이 어느 DASD 볼륨의 것인지 알 수 있게 만드는 연결이 여기서 맺어진다.
 * 
 * 실행 컨텍스트는 전부 **프로세스 컨텍스트** 다. 이 파일의 함수들은 잠들 수 있는
 * 호출(뮤텍스, 디스크 등록, 파일 열기, 파티션 읽기)을 서슴없이 하며, 인터럽트나
 * 태스클릿 문맥에서는 절대 불리지 않는다. 이 점이 dasd_erp.c 와 정반대다.
 * 
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 dasd.c 의 장치 상태 기계와 모듈 초기화/종료가 유일한 호출자다.
 * dasd.c 는 이 파일의 여섯 함수를 dasd_int.h 의 선언(2800~2810줄)을 통해 부른다.
 * 같은 dasd.c 에서 두 개의 정적 표를 빌려 온다 — 요청 처리 콜백 dasd_mq_ops
 * (3206줄)와 블록 장치 연산표 dasd_device_operations(3299줄).
 * dasd_devmap.c 의 dasd_add_link_to_gendisk()(929줄)를 불러 gendisk 의
 * private_data 에 busid 대응표 항목을 심어 두면, 나중에 dasd_ioctl.c 와
 * dasd.c 가 gendisk 만 들고도 장치를 되찾을 수 있다.
 * dasd_eckd.c 는 PPRC(원격 복제) 쌍을 바꿀 때 그 연결만 따로 다시 심는다(6180줄).
 * 
 * 아래쪽으로는 리눅스 블록 계층에 기댄다. 이 트리에서 확인할 수 있는 것들은
 * include/linux/blk-mq.h 의 blk_mq_alloc_tag_set()·blk_mq_alloc_disk(),
 * include/linux/blkdev.h 의 device_add_disk()·del_gendisk()·put_disk()·
 * set_capacity()·set_disk_ro()·disk_devt()·bdev_file_open_by_dev()·file_bdev(),
 * 그리고 block/partitions/core.c 의 bdev_disk_changed()(1486줄)다.
 * register_blkdev()/unregister_blkdev() 와 fput() 은 이 스파스 체크아웃에
 * 헤더가 없어 선언을 이 트리에서 확인 못 함.
 * 
 * 데이터 흐름은 두 갈래다. **아래로** — block->tag_set 에 큐 깊이와 하드웨어 큐
 * 개수가 채워지고, gendisk 에 주/부번호와 이름과 연산표가 채워진 뒤 블록 계층에
 * 등록된다. **위로** — 등록된 디스크에서 파티션 표가 읽혀 /dev 에 파티션 노드가
 * 생기고, 그 사실이 block->bdev_file 이라는 한 개의 포인터로 이 드라이버에
 * 되돌아온다. 그 포인터의 존재 여부가 곧 '파티션 스캔이 살아 있는가' 이며,
 * 장치를 오프라인으로 내릴 수 있는지 판정하는 기준이 된다(dasd.c:3554).
 * 
 * === 주요 함수/구조체 요약 ===
 * dasd_gendisk_init()      DASD 주번호를 블록 계층에 등록한다. 모듈 적재 시 한 번.
 * dasd_gendisk_exit()      그 등록을 되돌린다. 모듈 해제 시 한 번.
 * dasd_name_format()       장치 색인을 dasda … dasdzzzz 로 바꾼다. 26진법인데
 *                          자리마다 1 을 빼는 변형이라 자리 수가 늘어도 이름이
 *                          겹치지 않는다.
 * dasd_gendisk_alloc()     태그 집합과 gendisk 를 만들어 디스크를 등록한다.
 *                          이 파일에서 가장 큰 함수이며 실패 경로가 셋이다.
 * dasd_gd_free()           디스크를 등록 해제하고 참조를 놓는 정적 도우미.
 *                          **앞쪽에 전방 선언이 있는 유일한 함수** 다.
 * dasd_gendisk_free()      위를 감싸 태그 집합까지 함께 놓는다.
 * dasd_scan_partitions()   디스크를 스스로 열어 파티션 표를 읽게 한다. 연 파일을
 *                          닫지 않고 block->bdev_file 에 남겨 두는 것이 요점이다.
 * dasd_destroy_partitions() 그 짝. 파티션을 무효화하고 열어 둔 파일을 닫는다.
 * 
 * === 이 파일을 읽을 때 알아 두면 좋은 어휘 ===
 * gendisk    리눅스가 디스크 하나를 나타내는 구조체. 주/부번호, 이름, 연산표,
 *            파티션 표를 담는다.
 * 태그 집합  blk-mq 가 요청 하나하나에 붙일 태그를 관리하는 자료구조.
 *            여기서 정한 큐 깊이와 하드웨어 큐 개수가 동시 처리 한도를 정한다.
 * 부번호     minor number. 이 드라이버는 장치 하나에 파티션 몫까지 묶어
 *            연속된 부번호 구간을 통째로 잡는다.
 * IDAW/TIDAW s390 채널이 쓰는 간접 데이터 주소 낱말. 세그먼트 하나가 그 하나로
 *            옮겨지도록 세그먼트 크기를 한 페이지로 제한하는 이유다.
 *            arch/s390 소관이라 이 트리에서 확인 못 함. */
/* [한국어] 인터럽트 처리와 하반부(태스클릿) 관련 선언.
 * [상류 코드 관찰] 이 파일에는 인터럽트 처리기도 태스클릿도 없다. gendisk 코드가
 * dasd.c 에서 갈라져 나오기 전의 잔재로 보인다. 이 스파스 체크아웃에는 그 헤더가
 * 없어 내용을 확인 못 함. 원본(1f0e418bb6) 14줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/interrupt.h>
/* [한국어] 블록 장치 주번호(major number) 상수들을 모아 둔 헤더. 아래에서 쓰는
 * DASD_MAJOR 가 이 계통에서 온다. 이 스파스 체크아웃에는 그 헤더가 없어
 * 값을 이 트리에서 확인 못 함 — 다만 dasd_gendisk_init() 위의 상류 주석이
 * 'static dasd major 94' 라고 적어 두어 94 임을 알 수 있다. */
#include <linux/major.h>
/* [한국어] 파일과 아이노드 계층. 아래 dasd_scan_partitions() 이 다루는 struct file 과
 * dasd_destroy_partitions() 이 부르는 fput() 이 이 계통에서 온다. 이 드라이버가
 * 파티션을 읽으려고 **자기 디스크를 스스로 파일로 여는** 방식을 쓰기 때문에
 * 필요하다. 이 스파스 체크아웃에는 그 헤더가 없어 선언을 확인 못 함. */
#include <linux/fs.h>
/* [한국어] 파티션 조작 ioctl(BLKPG 계열)이 주고받는 자료구조.
 * [상류 코드 관찰] 이 파일에는 ioctl 처리기가 없다 — DASD 의 ioctl 은 모두
 * dasd_ioctl.c 에 있다. 파티션 코드가 이 파일에 함께 있던 시절의 잔재로 보인다.
 * 이 스파스 체크아웃에는 그 헤더가 없어 내용을 확인 못 함.
 * 원본(1f0e418bb6) 17줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/blkpg.h>

/* [한국어] 사용자 공간과 데이터를 주고받는 복사 함수들.
 * [상류 코드 관찰] 이 파일에는 사용자 버퍼를 만지는 코드가 없다.
 * 원본(1f0e418bb6) 19줄에서 확인했으며 코드는 고치지 않았다. */
#include <linux/uaccess.h>

/* [한국어] DASD 서브시스템의 중앙 헤더. 이 파일이 쓰는 어휘가 여기서 온다 —
 * struct dasd_block(gdp·tag_set·bdev_file 을 가진 블록 장치 구조체),
 * struct dasd_device(devindex·features·flags·cdev 를 가진 장치 구조체),
 * 장치 개수 상한 DASD_PER_MAJOR, 읽기 전용 표지 DASD_FLAG_DEVICE_RO,
 * 로그 매크로 DBF_DEV_EVENT, 그리고 dasd.c 가 정의한 dasd_mq_ops 와
 * dasd_device_operations 의 extern 선언(2600·2602줄)이다.
 * 이 헤더는 include/linux/blk-mq.h 도 포함하므로, 아래에서 쓰는 blk-mq 함수들의
 * 선언도 이 한 줄을 통해 들어온다. */
#include "dasd_int.h"

/* [한국어] 새 DASD 마다 blk-mq 에 요청할 **큐 깊이의 기본값**. 하드웨어 큐 하나가
 * 동시에 품을 수 있는 요청 개수이며, 이 값이 곧 태그 개수다.
 * 설정자: 아래 module_param 을 통해 부팅 매개변수나 모듈 인자로 바꿀 수 있다.
 * 읽는 자: dasd_gendisk_alloc() 이 block->tag_set.queue_depth 에 복사한다.
 * 값 범위: 부호 없는 정수. 기본 32.
 * 동기화: 장치를 만들 때 한 번 읽을 뿐이며, 이미 만들어진 장치에는 영향이
 * 없으므로 잠금이 필요 없다. */
static unsigned int queue_depth = 32;
/* [한국어] 새 DASD 마다 만들 **하드웨어 큐 개수의 기본값**. blk-mq 는 CPU 를 하드웨어
 * 큐에 나눠 붙여 잠금 경합을 줄이는데, 그 큐 수를 정하는 값이다.
 * 설정자·읽는 자·동기화: 위 queue_depth 와 같다.
 * 값 범위: 부호 없는 정수. 기본 4.
 * DASD 는 채널 하나로 나가므로 하드웨어 큐가 실제 병렬 채널을 뜻하지는 않는다 —
 * 큐 사이의 잠금 경합을 나누는 목적이다. 실제 병렬성은 PAV 별칭 장치가 준다. */
static unsigned int nr_hw_queues = 4;
/* [한국어] dasd_gd_free() 의 전방 선언. **정의는 아래쪽(236줄)에 있는데 그보다 먼저
 * dasd_gendisk_alloc() 이 이름을 쓰기 때문에** 필요하다. 이름 짓기가 실패했을 때
 * 그 함수가 이것을 불러 디스크를 되돌린다. 정적 함수라 이 파일 밖에서는
 * 보이지 않는다. */
static void dasd_gd_free(struct gendisk *gdp);

/* [한국어] 위 queue_depth 를 모듈 매개변수로 노출한다. uint 는 값의 형이고,
 * 0444 는 sysfs 파일의 권한 — 소유자·그룹·다른 사용자 모두 읽기만 가능하다.
 * **쓰기 권한을 주지 않은 이유** 는 이미 만들어진 장치의 큐 깊이를 나중에 바꿀
 * 수 없기 때문이며, 값이 쓰이는 시점이 장치 생성 한 번뿐이라는 사실과 짝을 이룬다.
 * 모듈 적재 시 인자로, 또는 커널 명령줄로 정할 수 있다. */
module_param(queue_depth, uint, 0444);
/* [한국어] 그 매개변수의 설명 문자열. modinfo 로 볼 수 있다. */
MODULE_PARM_DESC(queue_depth, "Default queue depth for new DASD devices");

/* [한국어] 위 nr_hw_queues 도 같은 방식으로 노출한다. 권한도 같은 이유로 읽기 전용이다. */
module_param(nr_hw_queues, uint, 0444);
/* [한국어] 그 매개변수의 설명 문자열. */
MODULE_PARM_DESC(nr_hw_queues, "Default number of hardware queues for new DASD devices");

/* [한국어]
 * dasd_name_format - 장치 색인 번호를 dasda, dasdaa … 꼴의 디스크 이름으로 바꾼다
 * 
 * @prefix: 이름 앞에 붙일 문자열. 유일한 호출자가 언제나 "dasd" 를 넘긴다.
 * @index: 장치 색인 번호(base->devindex). 0 이 첫 장치다.
 * @buf: 결과를 쓸 버퍼. 호출자는 gdp->disk_name 을 넘긴다.
 * @buflen: 그 버퍼의 크기. 호출자는 sizeof 로 32(DISK_NAME_LEN,
 *       include/linux/blkdev.h:59·155)를 넘긴다.
 * @return: 0 이면 성공. 버퍼가 모자라면 -EINVAL.
 * 
 * **왜 그냥 dasd0, dasd1 로 하지 않았는가** 가 이 함수의 배경이다. DASD 는
 * SCSI 디스크(sda, sdb …)와 같은 관례를 따르며, 장치가 늘어도 이름 길이가
 * 천천히 늘도록 26진법 비슷한 표기를 쓴다. 위쪽 상류 주석의 표가 그 결과다 —
 * 한 자리로 26개, 두 자리로 676개가 더해져 702개, 세 자리까지 18278개,
 * 네 자리까지 475252개를 이름 붙일 수 있다.
 * 
 * **보통의 26진법이 아니다.** 자리를 하나 올릴 때마다 몫에서 1 을 뺀다
 * (`index = (index / unit) - 1`). 그 한 번의 뺄셈이 없으면 'a' 와 'aa' 가 같은
 * 번호를 두 번 쓰게 된다 — 26진법에서는 앞자리의 0 이 '없는 자리' 와 구별되지
 * 않기 때문이다. 1 을 빼면 '한 자리로 쓸 수 있는 26개' 를 이미 소진한 것으로
 * 치므로, 두 자리 구간이 26 부터 다시 시작해 겹치지 않는다.
 * 
 * 동작은 셋이다.
 * 1. 버퍼의 **맨 뒤에서부터** 글자를 채운다. 자리 수를 미리 알 수 없으니 낮은
 *    자리부터 만들어 앞으로 밀고 나가는 편이 간단하기 때문이다.
 * 2. 다 만들면 그 결과를 접두사 바로 뒤로 옮긴다.
 * 3. 마지막에 접두사를 버퍼 앞에 써 넣는다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 장치가 상태 기계에서 BASIC 으로 올라갈 때
 * 한 번만 불린다.
 * 
 * caller: 이 파일의 dasd_gendisk_alloc() 한 곳뿐(209줄).
 * callee: strlen(), memmove(), memcpy().
 * 
 * 에러 경로: 만들 글자가 버퍼에 남은 자리보다 많으면 -EINVAL 을 돌려주고
 * 버퍼는 미완성 상태로 남는다. 호출자가 그것을 보고 디스크 생성을 포기한다.
 * 다만 상류 주석의 표대로 네 자리로 47만 개가 넘는 이름을 만들 수 있고
 * 버퍼가 32바이트라, 실제로 이 경로에 들어가는 일은 없다.
 * 
 * 호출 체인:
 *   dasd_gendisk_alloc() → [이 함수] → memmove() → memcpy() */
/*
 * Set device name.
 *   dasda - dasdz : 26 devices
 *   dasdaa - dasdzz : 676 devices, added up = 702
 *   dasdaaa - dasdzzz : 17576 devices, added up = 18278
 *   dasdaaaa - dasdzzzz : 456976 devices, added up = 475252
 */
static int dasd_name_format(char *prefix, int index, char *buf, int buflen)
{
	/* [한국어] 한 자리에 쓸 수 있는 글자 수, 곧 알파벳 26. 상수를 직접 26 이라 쓰지 않고
	 * 'z' 와 'a' 의 차이로 적어, 문자 집합이 연속이라는 전제를 코드에 드러냈다. */
	const int base = 'z' - 'a' + 1;
	/* [한국어] 접두사가 끝나고 숫자 부분이 시작될 자리. **아직 buf 에 접두사를 쓰지
	 * 않았지만** 길이만 알면 되므로 미리 계산해 둔다. 아래 do 루프의 종료 검사가
	 * 이 경계를 넘지 않는지 보는 데 쓴다. */
	char *begin = buf + strlen(prefix);
	/* [한국어] 버퍼의 끝(마지막 바이트 다음). 글자를 뒤에서부터 채우므로 여기가 출발선이다. */
	char *end = buf + buflen;
	/* [한국어] 뒤에서 앞으로 움직이며 글자를 쓰는 커서. */
	char *p;
	/* [한국어] 한 자리의 진법. 아래에서 base 값을 그대로 받는다.
	 * [상류 코드 관찰] base 를 한 번 복사한 뒤 끝까지 바뀌지 않는다. 즉 base 를
	 * 바로 써도 결과가 같은 사실상 중복 변수다. 자리마다 진법이 달랐던 옛 판의
	 * 흔적으로 보인다. 원본(1f0e418bb6) 46·50·54·55줄에서 확인했으며 코드는
	 * 고치지 않았다. */
	int unit;

	/* [한국어] 커서를 버퍼의 마지막 바이트에 둔다. */
	p = end - 1;
	/* [한국어] 그 자리에 문자열 종결자를 쓴다. 아래에서 글자를 앞으로 채워 나가므로,
	 * 종결자가 언제나 결과의 끝에 놓인다. */
	*p = '\0';
	/* [한국어] 진법을 26 으로 잡는다. */
	unit = base;
	/* [한국어] **do-while 인 이유** 는 index 가 0 이어도 글자를 반드시 하나는 만들어야
	 * 하기 때문이다. while 로 썼다면 첫 장치의 이름이 'dasd' 로 끝나 버린다. */
	do {
		/* [한국어] 커서가 접두사 자리까지 밀려왔는지 본다. 더 쓰면 접두사를 덮어쓰게 된다. */
		if (p == begin)
			/* [한국어] 자리가 모자라면 실패로 돌아간다. 버퍼에 이미 쓴 글자는 그대로 남지만,
			 * 호출자가 이 반환값을 보고 그 버퍼를 버리므로 문제되지 않는다. */
			return -EINVAL;
		/* [한국어] 커서를 한 칸 앞으로 물린 뒤 그 자리에 이번 자리의 글자를 쓴다.
		 * `--p` 가 먼저 실행되므로 종결자를 덮어쓰지 않는다.
		 * `'a' + (index % unit)` 이 나머지를 알파벳 한 글자로 옮기는 계산이다. */
		*--p = 'a' + (index % unit);
		/* [한국어] 몫을 구하되 **1 을 뺀다.** 이 뺄셈이 이 표기법의 전부다 — 자리 수가 늘어날
		 * 때 앞자리 구간이 겹치지 않게 만든다. 결과가 음수(-1)가 되면 아래 조건이
		 * 거짓이 되어 반복이 끝난다. */
		index = (index / unit) - 1;
	/* [한국어] 몫이 0 이상이면 자리를 하나 더 만든다. -1 이면 여기서 끝난다. */
	} while (index >= 0);

	/* [한국어] 만들어 둔 글자들을 버퍼 뒤쪽에서 접두사 바로 뒤로 옮긴다. 옮길 길이
	 * `end - p` 에는 종결자 한 바이트가 포함된다.
	 * **memcpy 가 아니라 memmove 인 이유** 는 원본과 대상 구간이 겹칠 수 있기
	 * 때문이다. 색인이 커서 글자가 여러 자리가 되면 p 가 begin 가까이까지
	 * 내려온다. */
	memmove(begin, p, end - p);
	/* [한국어] 마지막으로 접두사를 버퍼 맨 앞에 써 넣는다. **순서가 뒤인 이유** 는
	 * 위 memmove 가 접두사 자리까지 손댈 일이 없도록, 옮기기를 먼저 끝내고
	 * 덮어쓰기 때문이다. 종결자는 위에서 함께 옮겨졌으므로 여기서는 접두사
	 * 길이만큼만 쓰면 된다. */
	memcpy(buf, prefix, strlen(prefix));

	/* [한국어] 이름이 완성되었음을 알린다. */
	return 0;
}

/* [한국어]
 * dasd_gendisk_alloc - blk-mq 태그 집합과 gendisk 를 만들어 디스크를 등록한다
 * 
 * @block: 디스크로 세울 블록 장치 구조체. 여기의 tag_set 과 gdp 두 칸이 이
 *       함수가 채우는 결과물이며, base 를 통해 소유 장치에 닿는다.
 * @return: 0 이면 성공. 장치 색인이 주번호 하나에 담을 수 있는 범위를 넘으면
 *       -EBUSY, 태그 집합·디스크 생성이 실패하면 그쪽이 준 음수 errno,
 *       이름 짓기가 실패하면 -EINVAL, 디스크 등록이 실패하면 그 값.
 * 
 * **struct dasd_block 이 /dev/dasda 가 되는 순간** 이 이 함수다. 이 파일에서
 * 가장 큰 함수이며, 하는 일이 크게 셋이다.
 * 
 * 1. **blk-mq 태그 집합 만들기.** 요청 처리 콜백 표(dasd.c 의 dasd_mq_ops),
 *    요청 하나에 딸릴 드라이버 전용 공간의 크기, 하드웨어 큐 개수, 큐 깊이를
 *    정한 뒤 블록 계층에 등록한다. 이 태그 집합이 이 볼륨의 동시 처리 한도를
 *    정한다.
 * 2. **gendisk 만들기와 채우기.** 태그 집합에서 요청 큐와 gendisk 를 한 번에
 *    받아 오고, 주/부번호·이름·연산표를 채운다. 세그먼트 제약을 함께 넘겨
 *    채널이 다룰 수 있는 형태로 bio 가 쪼개지게 한다.
 * 3. **등록.** 읽기 전용 여부를 반영하고, gendisk 에서 장치를 되찾을 연결을
 *    심고, 용량을 0 으로 둔 채 디스크를 등록한다.
 * 
 * **용량을 0 으로 두고 등록하는 이유** 는 이 시점에 볼륨의 크기를 아직 모르기
 * 때문이다. 크기는 다음 단계인 BASIC → READY 전이에서 볼륨 분석이 끝난 뒤
 * dasd.c:347 이 다시 채운다. 즉 이 함수가 만든 디스크는 등록 직후에는 '크기 0
 * 짜리 디스크' 로 잠깐 존재한다.
 * 
 * 세 개의 실패 경로가 서로 다른 정리를 한다는 점을 눈여겨봐야 한다 — 색인
 * 초과는 아무것도 만들지 않았으므로 그냥 돌아가고, 디스크 생성 실패는 태그
 * 집합만 되돌리고, 등록 실패는 디스크와 태그 집합을 함께 되돌린다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있는 호출(태그 집합 할당, 디스크
 * 등록)을 하므로 다른 문맥에서는 부를 수 없다. 장치를 온라인으로 올리는
 * 경로에서 장치마다 한 번씩만 불린다.
 * 
 * caller: dasd.c 의 dasd_state_known_to_basic()(220줄) 한 곳뿐.
 * callee: blk_mq_alloc_tag_set(), blk_mq_alloc_disk(), blk_mq_free_tag_set()
 * (include/linux/blk-mq.h), dasd_name_format()(이 파일 위), set_disk_ro(),
 * set_capacity(), device_add_disk()(include/linux/blkdev.h),
 * dasd_add_link_to_gendisk()(dasd_devmap.c:929), dasd_gd_free()/
 * dasd_gendisk_free()(이 파일 아래).
 * 
 * 에러 경로: 위에 적은 셋. 호출자는 반환값이 0 이 아니면 상태 전이를 중단하고
 * 장치를 KNOWN 단계에 남겨 둔다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_state_known_to_basic() → [이 함수]
 *     → blk_mq_alloc_tag_set() → blk_mq_alloc_disk()
 *       → dasd_name_format() → dasd_add_link_to_gendisk() → device_add_disk() */
/*
 * Allocate and register gendisk structure for device.
 */
int dasd_gendisk_alloc(struct dasd_block *block)
{
	/* [한국어] 이 디스크의 요청 큐에 걸 제약 모음. 아래 세 칸만 채우고 나머지는 0 으로
	 * 두어 블록 계층의 기본값을 쓴다.
	 * 설정자: 이 초기화식 한 번.
	 * 읽는 자: blk_mq_alloc_disk() 가 큐를 만들 때 복사해 간다. 그 뒤로는
	 * 블록 계층이 bio 를 쪼갤 때마다 참조한다.
	 * 값 범위: struct queue_limits(include/linux/blkdev.h).
	 * 동기화: 지역 변수라 잠금이 필요 없다. 큐로 복사된 뒤의 갱신은 블록 계층의
	 * queue_limits 갱신 절차를 따른다 — dasd.c:344 가 볼륨 분석 뒤에 그 절차로
	 * 블록 크기와 discard 한도를 더 채워 넣는다. */
	struct queue_limits lim = {
		/*
		 * With page sized segments, each segment can be translated into
		 * one idaw/tidaw.
		 */
		/* [한국어] 세그먼트 하나의 최대 크기를 **한 페이지** 로 제한한다.
		 * 설정자: 이 초기화식.
		 * 읽는 자: 블록 계층의 bio 병합·분할 코드.
		 * 값 범위: PAGE_SIZE.
		 * 동기화: 큐 생성 이후 갱신 절차를 통해서만 바뀐다.
		 * 위 상류 주석이 이유를 말한다 — 세그먼트가 페이지 크기면 그 하나가 IDAW
		 * (Indirect Data Address Word) 또는 TIDAW 하나로 그대로 옮겨진다. s390 채널은
		 * 데이터 버퍼를 그 간접 주소 낱말의 목록으로 받으므로, 크기를 맞춰 두면
		 * 변환이 1:1 이 되어 계산이 단순해진다. IDAW/TIDAW 의 규격은 arch/s390
		 * 소관이라 이 트리에서 확인 못 함. */
		.max_segment_size = PAGE_SIZE,
		/* [한국어] 세그먼트가 넘어서는 안 되는 경계를 페이지 경계로 정한다.
		 * 설정자: 이 초기화식.
		 * 읽는 자: block/blk.h:700 에 있는 인라인 판정 함수가 이 마스크를 꺼내
		 * 두 조각을 하나로 합쳐도 되는지 정한다.
		 * 값 범위: PAGE_SIZE - 1. 하위 비트가 모두 선 마스크다.
		 * 동기화: 위와 같다.
		 * **최대 크기 제한만으로는 부족한 이유** 가 이 칸의 존재 이유다. 한 페이지
		 * 크기라도 페이지 경계에 걸쳐 놓이면 물리적으로 두 페이지에 나뉘어, 간접 주소
		 * 낱말 하나로 가리킬 수 없다. 이 마스크가 그런 조각의 병합을 막는다. */
		.seg_boundary_mask = PAGE_SIZE - 1,
		/* [한국어] 한 요청에 담을 수 있는 세그먼트 개수의 상한.
		 * 설정자: 이 초기화식.
		 * 읽는 자: 블록 계층이 요청에 bio 를 더 붙일지 판정할 때.
		 * 값 범위: USHRT_MAX(65535). 실질적으로 제한을 두지 않겠다는 뜻이다.
		 * 동기화: 위와 같다.
		 * 개수를 사실상 풀어 두는 대신 위 두 칸으로 **조각의 모양** 만 제약하는 것이
		 * 이 드라이버의 방식이다. 실제 한도는 채널 프로그램을 만드는 디시플린이
		 * 자기 형편에 맞춰 다시 건다. */
		.max_segments = USHRT_MAX,
	};
	/* [한국어] 만들어 받을 gendisk 포인터. 오류일 때는 오류 포인터가 담긴다. */
	struct gendisk *gdp;
	/* [한국어] 이 블록 장치를 소유한 DASD 장치. 색인·기능 비트·표지·CCW 장치를 여기서 얻는다. */
	struct dasd_device *base;
	/* [한국어] 이 장치의 색인 번호. 부번호와 이름을 만드는 근거가 된다. */
	unsigned int devindex;
	/* [한국어] 각 단계의 반환값을 받는 변수. */
	int rc;

	/* Make sure the minor for this device exists. */
	/* [한국어] 블록 장치에서 소유 장치로 거슬러 올라간다. struct dasd_block 과
	 * struct dasd_device 를 나눠 둔 이 드라이버의 구조상, 장치 쪽 정보는
	 * 언제나 이 고리를 통해 얻는다. */
	base = block->base;
	/* [한국어] 장치 색인을 꺼낸다. 위 상류 주석대로 부번호가 존재하는지 확인하려는 것이며,
	 * 아래 두 줄이 그 검사다. */
	devindex = base->devindex;
	/* [한국어] 색인이 주번호 하나가 담을 수 있는 장치 수를 넘는지 본다. 부번호 20비트를
	 * 파티션 몫과 장치 몫으로 나눈 결과가 그 한도이며, dasd_int.h:143 이
	 * DASD_PER_MAJOR 로 계산해 둔다. */
	if (devindex >= DASD_PER_MAJOR)
		/* [한국어] 넘으면 -EBUSY 로 거절한다. **아직 아무것도 만들지 않았으므로 되돌릴 것이
		 * 없다.** 이 값을 고른 이유는 자원이 부족한 것이 아니라 이미 다른 장치들이
		 * 번호 공간을 다 차지했다는 뜻이기 때문이다. */
		return -EBUSY;

	/* [한국어] 요청 처리 콜백 표를 건다. 실체는 dasd.c:3206 의 dasd_mq_ops 이며,
	 * 요청 제출(queue_rq), 완료(complete), 시간 초과(timeout), 하드웨어 큐
	 * 생성·해제(init_hctx/exit_hctx) 다섯 콜백을 담고 있다. 이 한 줄이
	 * blk-mq 와 이 드라이버를 잇는 매듭이다. */
	block->tag_set.ops = &dasd_mq_ops;
	/* [한국어] 요청 하나마다 blk-mq 가 함께 잡아 줄 드라이버 전용 공간의 크기를 정한다.
	 * **cqr 을 통째로 그 공간에 두겠다는 뜻** 이며, 덕분에 I/O 마다 cqr 머리를
	 * 따로 할당할 필요가 없다. 각 디시플린의 build_cp 이 그 공간의 주소를 얻어
	 * 요청 생성 함수에 넘기고(dasd_eckd.c:4029 등), 시간 초과 처리는 반대로
	 * 그 주소에서 cqr 을 되찾는다(dasd.c:3120). 장치의 정적 풀에서는 이제 CCW
	 * 사슬과 데이터 영역만 떼어 오면 된다(dasd.c 의 dasd_smalloc_request). */
	block->tag_set.cmd_size = sizeof(struct dasd_ccw_req);
	/* [한국어] 하드웨어 큐 개수를 모듈 매개변수 기본값으로 정한다. CPU 들이 서로 다른
	 * 큐에 요청을 넣게 해 잠금 경합을 줄이는 것이 목적이다. */
	block->tag_set.nr_hw_queues = nr_hw_queues;
	/* [한국어] 큐 깊이, 곧 태그 개수를 정한다. 하드웨어 큐 하나가 동시에 품을 수 있는
	 * 요청 수이며, 이것을 넘으면 blk-mq 가 제출을 막아 준다. */
	block->tag_set.queue_depth = queue_depth;
	/* [한국어] NUMA 노드를 지정하지 않는다. 메인프레임의 채널 부착 장치는 특정 노드에
	 * 매여 있지 않으므로, 블록 계층이 알아서 고르게 둔다. */
	block->tag_set.numa_node = NUMA_NO_NODE;
	/* [한국어] 태그 집합을 블록 계층에 등록한다. 이때 큐 개수와 깊이만큼의 요청 구조체와
	 * 태그 비트맵이 실제로 할당된다 — 위에서 정한 cmd_size 만큼의 여유 공간이
	 * 요청마다 함께 잡히는 것도 이 안에서다. */
	rc = blk_mq_alloc_tag_set(&block->tag_set);
	/* [한국어] 실패하면 */
	if (rc)
		/* [한국어] **아무것도 되돌리지 않고 그대로 돌아간다.** 태그 집합 할당 함수가 자기
		 * 실패 경로에서 스스로 정리하기 때문이며, 이 함수가 그 전에 만든 것도 없다. */
		return rc;

	/* [한국어] 태그 집합에서 요청 큐와 gendisk 를 한 번에 만든다. 세 번째 인자가
	 * **큐의 queuedata 로 들어가는 것** 이 요점이다 — 나중에 dasd.c:3021 의
	 * do_dasd_request() 와 dasd.c:3114 의 dasd_times_out() 이 그 자리에서
	 * struct dasd_block 을 되찾는다. 매크로가 정적 lock_class_key 를 하나 만들어
	 * 넘기므로(include/linux/blk-mq.h:730), lockdep 이 이 큐의 잠금을 다른
	 * 드라이버의 것과 구별해 본다. */
	gdp = blk_mq_alloc_disk(&block->tag_set, &lim, block);
	/* [한국어] 오류 포인터가 돌아왔으면 */
	if (IS_ERR(gdp)) {
		/* [한국어] 앞서 등록한 태그 집합을 되돌린다. **이 줄이 있어야** 실패 뒤 남는 것이
		 * 없다. 아래 이름 짓기 실패 경로와 견줘 보면 차이가 드러난다. */
		blk_mq_free_tag_set(&block->tag_set);
		/* [한국어] 오류 포인터에서 errno 를 꺼내 그대로 올린다. */
		return PTR_ERR(gdp);
	}

	/* Initialize gendisk structure. */
	/* [한국어] 주번호를 DASD 전용 값으로 정한다. 아래 dasd_gendisk_init() 이 등록해 둔
	 * 바로 그 번호이며, 그 위의 상류 주석이 94 라고 적어 두었다. */
	gdp->major = DASD_MAJOR;
	/* [한국어] 이 장치가 쓸 부번호 구간의 시작. 색인을 파티션 비트 수만큼 왼쪽으로 밀어
	 * **장치마다 파티션 몫의 번호를 통째로 예약한다.** 예를 들어 파티션 비트가
	 * 2 이면 장치 0 은 0~3, 장치 1 은 4~7 을 쓴다. DASD_PARTN_BITS 는 asm/dasd.h
	 * 소관이라 이 트리에서 확인 못 함. */
	gdp->first_minor = devindex << DASD_PARTN_BITS;
	/* [한국어] 그 구간의 길이. 위와 같은 비트 수에서 나오며, 파티션 하나당 부번호 하나에
	 * 디스크 전체 몫 하나를 더한 개수다. */
	gdp->minors = 1 << DASD_PARTN_BITS;
	/* [한국어] 블록 장치 연산표를 건다. 실체는 dasd.c:3299 의 dasd_device_operations 이며
	 * open, release, ioctl, getgeo, set_read_only 를 담고 있다. 사용자 공간이
	 * /dev/dasda 를 열고 다룰 때 지나가는 문이다.
	 * dasd_ioctl.c:715 는 이 표의 주소를 **DASD 인지 가려내는 표식** 으로도 쓴다. */
	gdp->fops = &dasd_device_operations;

	/* [한국어] 이름을 짓는다. 접두사 "dasd" 에 색인을 알파벳 표기로 붙여 gendisk 의
	 * 이름 칸에 곧바로 쓴다. 버퍼 크기를 sizeof 로 넘기므로 그 칸의 크기
	 * (32바이트)가 바뀌어도 함께 따라간다. */
	rc = dasd_name_format("dasd", devindex, gdp->disk_name, sizeof(gdp->disk_name));
	/* [한국어] 이름 짓기가 실패했으면 — 버퍼가 모자란 경우뿐이다. */
	if (rc) {
		/* [한국어] 장치 디버그 영역에 이유를 남긴다. 콘솔이 아니라 순환 버퍼로 가므로
		 * 부팅 로그를 어지럽히지 않는다. */
		DBF_DEV_EVENT(DBF_ERR, block->base,
			      "setting disk name failed, rc %d", rc);
		/* [한국어] 만들어 둔 디스크를 되돌린다. **아직 block->gdp 에 넣지 않았으므로**
		 * 아래 dasd_gendisk_free() 를 쓸 수 없어, 정적 도우미를 직접 부른다.
		 * 이것이 파일 앞머리에 그 함수의 전방 선언이 있는 이유다.
		 * [상류 코드 관찰] 이 경로만 blk_mq_free_tag_set() 을 부르지 않아, 위쪽
		 * 디스크 생성 실패 경로(331줄)나 아래 dasd_gendisk_free()(383줄)와 견주면
		 * 태그 집합이 되돌려지지 않고 남는다. 다만 상단 상류 주석의 표대로 네 자리
		 * 이름으로 47만 개 넘는 장치를 감당할 수 있고 이름 칸이 32바이트라, 이름
		 * 짓기가 실패하는 상황 자체가 실제로는 생기지 않는다.
		 * 원본(1f0e418bb6) 100줄·114줄·152줄을 견주어 확인했으며 코드는 고치지 않았다. */
		dasd_gd_free(gdp);
		/* [한국어] 이름 짓기의 오류 코드를 그대로 올린다. */
		return rc;
	}

	/* [한국어] 읽기 전용으로 다뤄야 하는지 **두 가지 경로** 로 확인한다. 하나는 사용자가
	 * 설정으로 요구한 기능 비트이고, 다른 하나는 하드웨어 자체가 읽기 전용이라고
	 * 알려 온 표지다. dasd_int.h:2165 의 상류 주석이 그 둘을 혼동하지 말라고
	 * 따로 적어 둘 만큼 성격이 다르다 — 앞은 정책, 뒤는 사실이다.
	 * 어느 쪽이든 쓰기를 막아야 하므로 논리합으로 묶었다. */
	if (base->features & DASD_FEATURE_READONLY ||
	    test_bit(DASD_FLAG_DEVICE_RO, &base->flags))
		/* [한국어] 블록 계층에 이 디스크가 읽기 전용임을 알린다. 이후 쓰기로 여는 시도가
		 * 블록 계층 단계에서 거절된다. */
		set_disk_ro(gdp, 1);
	/* [한국어] gendisk 에서 이 장치를 되찾을 수 있도록 연결을 심는다. dasd_devmap.c:929
	 * 가 busid 로 대응표 항목을 찾아 gendisk 의 private_data 에 넣어 주며,
	 * dasd_devmap.c 의 되찾기 함수가 그 반대 방향을 맡는다.
	 * **큐의 queuedata 와는 다른 연결** 이다 — 그쪽은 struct dasd_block 을
	 * 가리키고 이쪽은 busid 대응표 항목을 가리킨다. 둘을 나눠 둔 덕분에
	 * 블록 장치가 아직 없는 단계에서도 gendisk 만으로 장치를 찾을 수 있다. */
	dasd_add_link_to_gendisk(gdp, base);
	/* [한국어] 만든 디스크를 블록 장치 구조체에 매단다. **이 줄부터** 아래
	 * dasd_gendisk_free() 가 이 디스크를 알아보고 정리할 수 있게 된다. */
	block->gdp = gdp;
	/* [한국어] 용량을 0 으로 두고 시작한다. 아직 볼륨을 분석하지 않아 크기를 모르기
	 * 때문이며, 실제 값은 다음 상태 전이에서 dasd.c:347 이 블록 수와 섹터 변환
	 * 자리 수로 계산해 채운다. */
	set_capacity(block->gdp, 0);

	/* [한국어] 디스크를 블록 계층과 장치 모델에 등록한다. 이 호출이 끝나면 /dev 노드가
	 * 생기고 udev 가 알림을 받는다. 첫 인자로 CCW 장치를 부모로 주므로,
	 * sysfs 에서 이 디스크가 채널 장치 아래에 놓인다. 세 번째 인자는 추가
	 * 속성 그룹인데 필요 없어 NULL 이다. */
	rc = device_add_disk(&base->cdev->dev, block->gdp, NULL);
	/* [한국어] 등록이 실패했으면 */
	if (rc) {
		/* [한국어] 디스크와 태그 집합을 함께 되돌린다. 이번에는 block->gdp 를 이미 채워
		 * 두었으므로 감싼 함수를 그대로 쓸 수 있다. */
		dasd_gendisk_free(block);
		/* [한국어] 등록 함수가 준 오류 코드를 그대로 올린다. */
		return rc;
	}

	/* [한국어] 디스크가 사용자 공간에 보이게 되었음을 알린다. 이제 장치 상태 기계가
	 * BASIC 단계로 올라간다. */
	return 0;
}

/* [한국어]
 * dasd_gd_free - gendisk 하나를 등록 해제하고 참조를 놓는다
 * 
 * @gd: 되돌릴 gendisk.
 * @return: 없다.
 * 
 * 세 줄짜리 정적 도우미다. 이렇게 따로 뽑아 둔 이유는 **두 곳에서 불러야 하기
 * 때문** 이다 — 하나는 dasd_gendisk_alloc() 의 이름 짓기 실패 경로로, 그때는
 * 아직 block->gdp 가 채워지지 않아 아래 dasd_gendisk_free() 를 쓸 수 없다.
 * 다른 하나는 그 dasd_gendisk_free() 자신이다. 앞엣것이 이 함수보다 위에
 * 있으므로 파일 앞머리에 전방 선언이 필요했다.
 * 
 * 세 줄의 순서에 뜻이 있다.
 * 1. 등록을 먼저 해제한다. 이 시점에 파일시스템에 소멸이 통보되고, 새로 여는
 *    길이 막히고, 파티션이 정리된다.
 * 2. 그 다음에 사적 자료 칸을 비운다. dasd_gendisk_alloc() 이 심어 둔
 *    busid 대응표 연결을 끊는 것이며, 등록을 해제한 뒤라야 그 연결을 따라올
 *    길이 없다.
 * 3. 마지막으로 참조를 놓는다. 이 참조가 마지막이면 gendisk 메모리가 그때
 *    풀린다 — 등록 해제만으로는 메모리가 풀리지 않고, block/genhd.c:1411 의
 *    상류 주석이 그 점을 명시한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 등록 해제는 파일시스템까지 내려가는
 * 블로킹 경로라 잠들 수 있어야 한다(block/genhd.c 의 등록 해제 경로가
 * might_sleep 으로 그것을 못 박아 두었다).
 * 
 * caller: 이 파일의 dasd_gendisk_alloc()(345줄)과 dasd_gendisk_free()(381줄).
 * callee: del_gendisk(), put_disk()(include/linux/blkdev.h).
 * 
 * 에러 경로: 없다. 실패할 수 있는 연산이 하나도 없다.
 * 
 * 호출 체인:
 *   dasd_gendisk_alloc() / dasd_gendisk_free() → [이 함수]
 *     → del_gendisk() → put_disk() */
/*
 * Free gendisk structure
 */
static void dasd_gd_free(struct gendisk *gd)
{
	/* [한국어] 디스크의 등록을 해제한다. block/genhd.c:1419 가 정의처이며, 안에서
	 * 파티션을 모두 떼고 파일시스템에 소멸을 알린 뒤 /dev 노드를 없앤다.
	 * blk-mq 큐를 쓰는 디스크라 태그 집합의 큐 개수 변경과 겹치지 않도록
	 * 그쪽에서 읽기 잠금을 잡아 준다. */
	del_gendisk(gd);
	/* [한국어] 사적 자료 칸을 비운다. dasd_gendisk_alloc() 이 dasd_devmap.c 를 통해
	 * 심어 둔 busid 대응표 연결을 끊는 일이다. 등록을 해제한 **뒤** 에 하므로,
	 * 그 연결을 따라 장치를 찾던 코드와 경쟁하지 않는다. */
	gd->private_data = NULL;
	/* [한국어] gendisk 참조를 놓는다. 등록 해제만으로는 구조체가 풀리지 않으며,
	 * 마지막 참조가 사라질 때 블록 계층이 해제해 준다. */
	put_disk(gd);
}

/* [한국어]
 * dasd_gendisk_free - 이 블록 장치의 디스크와 태그 집합을 함께 되돌린다
 * 
 * @block: 정리할 블록 장치 구조체.
 * @return: 없다.
 * 
 * dasd_gendisk_alloc() 의 짝이다. 위 dasd_gd_free() 를 감싸면서 **태그 집합까지
 * 함께 놓는다는 점** 이 다르다. 디스크와 태그 집합은 생성 순서가 태그 집합
 * 먼저였으므로, 해제는 반대로 디스크 먼저다.
 * 
 * 첫 줄의 검사가 이 함수를 **여러 번 불러도 안전하게** 만든다. 디스크를 되돌린
 * 뒤 곧바로 block->gdp 를 비우므로, 두 번째 호출은 아무 일도 하지 않고 돌아간다.
 * 장치 상태 기계가 오르내리기를 반복해도 문제가 없어야 하기 때문에 필요한
 * 성질이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 장치가 BASIC 아래로 내려갈 때, 그리고
 * dasd_gendisk_alloc() 의 마지막 실패 경로에서 불린다.
 * 
 * caller: dasd.c 의 dasd_state_basic_to_known()(264줄)과, 이 파일의
 * dasd_gendisk_alloc()(358줄).
 * callee: dasd_gd_free()(이 파일 위), blk_mq_free_tag_set()
 * (include/linux/blk-mq.h).
 * 
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_state_basic_to_known() / dasd_gendisk_alloc() → [이 함수]
 *     → dasd_gd_free() → blk_mq_free_tag_set() */
/*
 * Unregister and free gendisk structure for device.
 */
void dasd_gendisk_free(struct dasd_block *block)
{
	/* [한국어] 디스크가 있을 때만 정리한다. 없으면 아직 만들지 않았거나 이미 정리한
	 * 것이므로 할 일이 없다. **이 검사가 이 함수를 거듭 불러도 안전하게 만든다.** */
	if (block->gdp) {
		/* [한국어] 디스크를 등록 해제하고 놓는다. 위의 세 줄짜리 도우미가 그 일을 한다. */
		dasd_gd_free(block->gdp);
		/* [한국어] 칸을 비워 두 번 정리하지 않게 한다. dasd.c:3599 처럼 다른 코드가 이 칸의
		 * 존재 여부로 디스크가 살아 있는지 판정하기도 하므로, 곧바로 비우는 것이
		 * 중요하다. */
		block->gdp = NULL;
		/* [한국어] 마지막으로 태그 집합을 놓는다. **순서가 뒤인 이유** 는 요청 큐가 태그
		 * 집합 위에 세워져 있어, 큐가 먼저 사라져야 태그 집합을 안전하게 풀 수 있기
		 * 때문이다. 위의 디스크 해제가 그 큐까지 정리해 준다. */
		blk_mq_free_tag_set(&block->tag_set);
	}
}

/* [한국어]
 * dasd_scan_partitions - 디스크를 스스로 열어 파티션 표를 읽게 한다
 * 
 * @block: 스캔할 블록 장치.
 * @return: 0 이면 성공. 디스크를 여는 데 실패하면 -ENODEV.
 *       **파티션 읽기 자체가 실패해도 0 을 돌려준다** — 로그만 남기고 계속
 *       진행한다. 파티션이 없거나 알 수 없는 형식이어도 디스크 전체는
 *       쓸 수 있어야 하기 때문이다.
 * 
 * 파티션은 커널이 디스크를 실제로 열어 첫 섹터들을 읽어야 알 수 있다. 그런데
 * 이 드라이버가 자기 디스크를 여는 순간 그 디스크의 열림 횟수가 하나 올라간다.
 * 이 함수의 어려움이 거기서 나온다 — **연 뒤 닫지 않고 그대로 들고 있어야 하며,
 * 그 사실을 오프라인 판정이 알아야 한다.**
 * 
 * 동작은 셋이다.
 * 1. 디스크를 읽기 전용으로 연다. 소유자 인자를 NULL 로 두어 독점 열기를
 *    요구하지 않으므로, 사용자 공간이 같은 디스크를 함께 열어도 막히지 않는다.
 * 2. 디스크 잠금을 잡고 재스캔을 요청한다. 무효화 인자를 거짓으로 주므로
 *    용량은 그대로 두고 파티션 배치만 다시 읽는다
 *    (block/partitions/core.c:1486 의 설명 참조).
 * 3. 연 파일을 블록 장치 구조체에 남겨 둔다. 그것이 **닫지 않았다는 표시** 이며,
 *    dasd.c:3554 가 그 칸을 보고 허용 열림 횟수를 0 에서 1 로 올려 준다.
 * 
 * 아래 상류 주석이 3번을 **재스캔 뒤에** 하는 이유를 적어 두었다 — 스캔이
 * 도는 동안에는 오프라인을 허용하면 안 되기 때문이다. 다만 그 주석은 짝이 되는
 * 열기 함수의 이름을 경로로 여는 판(bdev_file_open_by_path)으로 적어 두었는데,
 * 실제로 부르는 것은 장치 번호로 여는 판이다. 아래 그 줄에 따로 적었다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 파일 열기와 뮤텍스와 실제 디스크 읽기가
 * 모두 들어 있어 반드시 잠들 수 있어야 한다.
 * 
 * caller: dasd.c 의 dasd_state_basic_to_ready()(349줄) 한 곳뿐. 그 함수는 볼륨
 * 분석과 용량 설정을 마친 뒤 이 함수를 부른다.
 * callee: bdev_file_open_by_dev(), disk_devt()(include/linux/blkdev.h),
 * mutex_lock()/mutex_unlock(), bdev_disk_changed()(block/partitions/core.c:1486).
 * 
 * 에러 경로: 여는 데 실패하면 로그를 남기고 -ENODEV 를 돌려준다. 호출자는
 * 장치를 BASIC 단계로 되돌린다. 재스캔이 실패한 경우는 로그만 남기고 성공으로
 * 처리하므로, 이 함수의 반환값만 보면 두 실패를 구별할 수 없다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_state_basic_to_ready() → [이 함수]
 *     → bdev_file_open_by_dev() → bdev_disk_changed() */
/*
 * Trigger a partition detection.
 */
int dasd_scan_partitions(struct dasd_block *block)
{
	/* [한국어] 연 디스크를 나타낼 파일 포인터. 성공하면 이 값이 블록 장치 구조체에
	 * 그대로 남아 장치가 READY 인 동안 계속 살아 있다. */
	struct file *bdev_file;
	/* [한국어] 재스캔의 결과를 받는 변수. 반환값으로 쓰이지는 않는다. */
	int rc;

	/* [한국어] 자기 디스크를 스스로 연다. 첫 인자는 gendisk 에서 뽑은 장치 번호이고,
	 * 두 번째는 읽기 전용 열기다. 세 번째 소유자 인자가 NULL 이라 **독점 열기를
	 * 요구하지 않으므로**, 사용자 공간이 같은 디스크를 동시에 열어도 서로 막지
	 * 않는다. 네 번째는 열기 훅으로, 필요 없어 NULL 이다.
	 * **드라이버가 자기 장치를 파일로 여는 것** 이 낯설어 보이지만, 파티션 읽기는
	 * 블록 계층의 일반 경로를 그대로 타야 하므로 이 방법이 쓰인다. */
	bdev_file = bdev_file_open_by_dev(disk_devt(block->gdp), BLK_OPEN_READ,
				       NULL, NULL);
	/* [한국어] 여는 데 실패했으면 */
	if (IS_ERR(bdev_file)) {
		/* [한국어] 장치 디버그 영역에 이유를 남긴다.
		 * [상류 코드 관찰] 메시지의 함수 이름이 지금 부르는 것과 다르다 — 이 자리에
		 * 있던 옛 열기 함수의 이름이 문자열에 그대로 남아 있다. 문자열일 뿐이라
		 * 동작에는 영향이 없다. 원본(1f0e418bb6) 168줄에서 확인했으며 코드는
		 * 고치지 않았다. */
		DBF_DEV_EVENT(DBF_ERR, block->base,
			      "scan partitions error, blkdev_get returned %ld",
			      PTR_ERR(bdev_file));
		/* [한국어] 열지 못했으니 파티션도 볼 수 없다. 원래의 오류 코드를 그대로 올리지 않고
		 * -ENODEV 로 바꿔 돌려준다 — 호출자에게는 '이 장치를 READY 로 올릴 수 없다'
		 * 는 사실만 필요하기 때문이다. */
		return -ENODEV;
	}

	/* [한국어] 디스크 잠금을 잡는다. 재스캔 함수가 이 잠금을 이미 쥐고 있을 것을
	 * 요구하며(block/partitions/core.c 가 lockdep 으로 검사한다), 그 잠금이
	 * 파티션 표와 용량 갱신을 지킨다. */
	mutex_lock(&block->gdp->open_mutex);
	/* [한국어] 파티션 표를 다시 읽는다. 두 번째 인자가 거짓이므로 **용량은 건드리지 않고
	 * 파티션 배치만** 다시 인식한다. 참이면 미디어가 바뀐 것으로 보고 캐시와
	 * 용량까지 무효화하는데, 여기서는 방금 만든 디스크를 처음 읽는 것이라
	 * 무효화할 것이 없다. */
	rc = bdev_disk_changed(block->gdp, false);
	/* [한국어] 잠금을 놓는다. 재스캔이 끝났으므로 더 쥐고 있을 이유가 없다. */
	mutex_unlock(&block->gdp->open_mutex);
	/* [한국어] 재스캔이 실패했으면 */
	if (rc)
		/* [한국어] 로그만 남긴다. **오류로 돌아가지 않는 것이 중요하다** — 파티션 표가
		 * 없거나 알 수 없는 형식이어도 디스크 전체(dasda)는 그대로 쓸 수 있어야
		 * 하기 때문이다. */
		DBF_DEV_EVENT(DBF_ERR, block->base,
				"scan partitions error, rc %d", rc);

	/*
	 * Since the matching fput() call to the
	 * bdev_file_open_by_path() in this function is not called before
	 * dasd_destroy_partitions the offline open_count limit needs to be
	 * increased from 0 to 1. This is done by setting device->bdev_file
	 * (see dasd_generic_set_offline). As long as the partition detection
	 * is running no offline should be allowed. That is why the assignment
	 * to block->bdev_file is done AFTER the BLKRRPART ioctl.
	 */
	/* [한국어] 연 파일을 블록 장치 구조체에 남긴다. 위 상류 주석이 이 한 줄의 뜻을
	 * 길게 설명한다 — 이 칸이 채워져 있다는 것은 **드라이버 자신이 이 디스크를
	 * 한 번 열어 두고 있다** 는 뜻이고, dasd.c:3554 가 그것을 보고 오프라인을
	 * 허용할 열림 횟수 한도를 0 에서 1 로 올린다. 재스캔이 도는 동안에는 이 칸이
	 * 비어 있어 한도가 0 이므로, 스캔 중 오프라인이 막힌다.
	 * dasd_ioctl.c:534 도 같은 칸을 보고 사용자에게 보고할 열림 횟수를 보정한다. */
	block->bdev_file = bdev_file;
	/* [한국어] 성공을 알린다. 재스캔이 실패했더라도 여기까지 왔으면 0 이다. */
	return 0;
}

/* [한국어]
 * dasd_destroy_partitions - 파티션을 모두 없애고 열어 두었던 파일을 닫는다
 * 
 * @block: 정리할 블록 장치.
 * @return: 없다.
 * 
 * dasd_scan_partitions() 의 짝이며, 위 상류 주석이 목적을 적어 두었다 —
 * 시스템에 있는 이 장치의 아이노드를 모두 없애고, 파티션을 지우고, 크기를 0 으로
 * 만들어 더는 쓸 수 없게 한다. 세 가지가 모두 한 번의 무효화 재스캔으로
 * 이뤄진다.
 * 
 * 동작은 셋이다.
 * 1. 짝이 챙겨 둔 파일 포인터를 꺼내고 **먼저 칸을 비운다.** 그러면 오프라인
 *    허용 한도가 다시 0 으로 내려가, 이 정리가 끝나기 전에 다른 경로가 이
 *    장치를 열려 있다고 오해하지 않는다.
 * 2. 디스크 잠금을 잡고 **무효화** 재스캔을 건다. 이번에는 두 번째 인자가
 *    참이므로 파티션을 지우는 데 그치지 않고 캐시를 무효화하고 용량도 0 으로
 *    되돌린다.
 * 3. 열어 두었던 파일을 닫는다.
 * 
 * 1번의 순서가 이 함수의 요점이다. 파일 포인터를 꺼내고 칸을 비우는 두 줄
 * 사이에는 아무것도 끼지 않으며, 그래야 '이미 닫는 중' 이라는 사실이 다른
 * 경로에 즉시 보인다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스와 파일시스템 동기화가 들어 있어
 * 반드시 잠들 수 있어야 한다.
 * 
 * caller: dasd.c 의 dasd_state_ready_to_basic()(388줄) 한 곳뿐. 그 함수는
 * 블록 큐를 비운 뒤 이 함수를 부르고, 곧이어 블록 크기 정보를 0 으로 지운다.
 * callee: file_bdev()(include/linux/blkdev.h), mutex_lock()/mutex_unlock(),
 * bdev_disk_changed()(block/partitions/core.c:1486), fput().
 * 
 * 에러 경로: 없다. 재스캔의 반환값도 보지 않는다 — 어차피 없애는 길이라
 * 실패해도 할 수 있는 일이 없기 때문이다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_state_ready_to_basic() → [이 함수]
 *     → bdev_disk_changed() → fput() */
/*
 * Remove all inodes in the system for a device, delete the
 * partitions and make device unusable by setting its size to zero.
 */
void dasd_destroy_partitions(struct dasd_block *block)
{
	/* [한국어] 짝이 남겨 둔 파일 포인터를 받아 둘 지역 변수. */
	struct file *bdev_file;

	/*
	 * Get the bdev_file pointer from the device structure and clear
	 * device->bdev_file to lower the offline open_count limit again.
	 */
	/* [한국어] 열어 두었던 파일을 꺼낸다. 위 상류 주석이 그 목적을 적어 두었다.
	 * [상류 코드 관찰] 이 칸이 비어 있는지 검사하지 않는다. dasd_scan_partitions()
	 * 이 실패하면 채워지지 않는데, 그 경우 dasd.c 의 상태 전이가 장치를 READY 로
	 * 올리지 않으므로 이 함수까지 오지 않는다는 전제로 보인다.
	 * 원본(1f0e418bb6) 205줄과 dasd.c:349-352 를 견주어 확인했으며 코드는
	 * 고치지 않았다. */
	bdev_file = block->bdev_file;
	/* [한국어] **곧바로** 칸을 비운다. 이 한 줄로 오프라인 허용 열림 횟수 한도가 다시
	 * 0 으로 내려간다(dasd.c:3554). 아래의 무효화 재스캔이 시간을 꽤 쓰는 동안
	 * 다른 경로가 이 장치를 '드라이버가 열고 있는 중' 으로 보지 않게 하려면
	 * 지금 비워야 한다. */
	block->bdev_file = NULL;

	/* [한국어] 디스크 잠금을 잡는다. 파일에서 블록 장치를, 다시 그 디스크를 거슬러
	 * 올라가 잠금을 얻는다. 짝인 스캔 함수는 block->gdp 를 바로 썼는데 여기서는
	 * 파일을 거치는데, 결과는 같은 디스크다. */
	mutex_lock(&file_bdev(bdev_file)->bd_disk->open_mutex);
	/* [한국어] 무효화 재스캔을 건다. **두 번째 인자가 참** 이라는 것이 짝과의 유일한
	 * 차이이며, 그 한 값이 '파티션 배치만 다시 읽기' 를 '미디어가 사라졌다고
	 * 보고 전부 무효화하기' 로 바꾼다. 그 결과 파티션이 지워지고, 페이지 캐시가
	 * 버려지고, 용량이 0 이 되고, 사용자 공간에 변경 알림이 나간다
	 * (block/partitions/core.c:1486 의 설명 참조).
	 * 반환값을 보지 않는 것은, 없애는 길에서는 실패해도 할 수 있는 일이 없기
	 * 때문이다. */
	bdev_disk_changed(file_bdev(bdev_file)->bd_disk, true);
	/* [한국어] 잠금을 놓는다. */
	mutex_unlock(&file_bdev(bdev_file)->bd_disk->open_mutex);

	/* Matching blkdev_put to the blkdev_get in dasd_scan_partitions. */
	/* [한국어] 열어 두었던 파일을 닫는다. 옆의 상류 주석이 이것이 짝에서 연 것의 대응임을
	 * 적어 두었다(주석에 적힌 이름은 지금 쓰는 함수들의 옛 이름이다).
	 * 이 호출로 디스크의 열림 횟수가 하나 내려가, 드라이버가 잡고 있던 마지막
	 * 참조가 풀린다. */
	fput(bdev_file);
}

/* [한국어]
 * dasd_gendisk_init - DASD 전용 블록 장치 주번호를 등록한다
 * 
 * @return: 0 이면 성공. 등록이 실패하면 그쪽이 준 음수 errno.
 * 
 * 모듈이 실릴 때 딱 한 번 불린다. 하는 일은 **주번호 하나를 통째로 예약하는 것**
 * 뿐이다. 위쪽 상류 주석대로 이 드라이버는 번호를 그때그때 배정받지 않고
 * 고정된 값(94)을 쓴다 — 오래된 관례이며, 메인프레임 설치 환경에서 장치
 * 번호가 부팅마다 달라지지 않게 하려는 뜻이다.
 * 
 * 여기서 예약해 둔 주번호를 실제로 쓰는 곳이 dasd_gendisk_alloc() 이다.
 * 그 함수가 gendisk 마다 이 번호와 색인에서 계산한 부번호를 채운다.
 * 
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트.
 * 
 * caller: dasd.c 의 dasd_init()(4057줄). 그 앞에 dasd_devmap_init() 이 있고
 * 뒤에 dasd_parse() 가 이어지며, 실패하면 공통 정리 경로로 뛴다.
 * callee: register_blkdev(), pr_warn(). 이 스파스 체크아웃에 그 헤더가 없어
 * 선언을 확인 못 함.
 * 
 * 에러 경로: 등록이 실패하면 그 값을 콘솔에 남기고 그대로 올린다. 다른 드라이버가
 * 이미 같은 번호를 쓰고 있을 때가 대표적이며, 그러면 모듈 적재 자체가 실패한다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_init() → [이 함수] → register_blkdev() */
int dasd_gendisk_init(void)
{
	/* [한국어] 등록 결과를 받는 변수. */
	int rc;

	/* Register to static dasd major 94 */
	/* [한국어] 주번호를 예약한다. 두 번째 인자의 이름 "dasd" 가 /proc/devices 에
	 * 그대로 보인다. 옆의 상류 주석대로 값이 고정돼 있어, 번호를 알아서
	 * 배정받는 방식(0 을 넘기는 판)을 쓰지 않는다. */
	rc = register_blkdev(DASD_MAJOR, "dasd");
	/* [한국어] 등록이 실패했으면 — 이미 그 번호를 쓰는 드라이버가 있는 경우가 대표적이다. */
	if (rc != 0) {
		/* [한국어] 콘솔에 경고를 남긴다. 아직 장치가 하나도 없어 장치별 디버그 영역이
		 * 없으므로, 여기서는 콘솔밖에 쓸 곳이 없다. */
		pr_warn("Registering the device driver with major number %d failed\n",
			DASD_MAJOR);
		/* [한국어] 오류를 그대로 올린다. 호출자가 모듈 초기화를 중단한다. */
		return rc;
	}
	/* [한국어] 주번호를 잡았음을 알린다. 이제 장치가 붙을 때마다 이 번호 아래로
	 * 디스크가 만들어질 수 있다. */
	return 0;
}

/* [한국어]
 * dasd_gendisk_exit - 등록해 두었던 주번호를 놓는다
 * 
 * @return: 없다.
 * 
 * dasd_gendisk_init() 의 짝이며, 모듈이 내려갈 때 한 번 불린다. 한 줄뿐이고
 * 반환값도 검사하지 않는다 — 모듈이 내려가는 길이라 실패해도 할 수 있는 일이
 * 없기 때문이다.
 * 
 * **이 시점에는 이미 모든 디스크가 사라진 상태여야 한다.** 장치들은 그보다 앞서
 * 상태 기계를 타고 내려가며 dasd_gendisk_free() 로 정리되고, 이 함수는 마지막에
 * 번호만 반납한다.
 * 
 * 실행 컨텍스트: 모듈 해제. 프로세스 컨텍스트.
 * 
 * caller: dasd.c 의 dasd_exit()(3321줄). 그 앞에서 요청 캐시가 지워지고
 * 뒤에서 busid 대응표가 정리된다.
 * callee: unregister_blkdev(). 이 스파스 체크아웃에 그 헤더가 없어 선언을
 * 확인 못 함.
 * 
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   dasd.c 의 dasd_exit() → [이 함수] → unregister_blkdev() */
void dasd_gendisk_exit(void)
{
	/* [한국어] 주번호 예약을 되돌린다. 등록할 때와 **같은 번호, 같은 이름** 을 넘겨야
	 * 하며, 그 둘이 어긋나면 블록 계층이 경고를 낸다. 반환값을 보지 않는 것은
	 * 모듈이 내려가는 길이라 대응할 방법이 없기 때문이다. */
	unregister_blkdev(DASD_MAJOR, "dasd");
}
