// SPDX-License-Identifier: GPL-2.0+
/*
 * Interface for Dynamic Logical Partitioning of I/O Slots on
 * RPA-compliant PPC64 platform.
 *
 * John Rose <johnrose@austin.ibm.com>
 * October 2003
 *
 * Copyright (C) 2003 IBM.
 */
/* [한국어] kobject_create_and_add()/kobject_put() — 이 파일은 슬롯이 아니라
 * kobject 를 직접 만들어 sysfs 디렉터리를 하나 세운다. */
/*
 * [한국어 설명] DLPAR 슬롯 추가·제거 sysfs 인터페이스 (rpadlpar_sysfs.c)
 *
 * === 파일의 역할 ===
 * IBM POWER 의 DLPAR(Dynamic Logical Partitioning) — 동작 중인 논리 파티션에
 * I/O 슬롯을 넣고 빼는 기능 — 을 사용자 공간에 노출하는 sysfs 계층이다.
 * 만드는 것은 디렉터리 하나와 파일 둘뿐이며, 관리자가
 *   echo "<DRC 이름>" > /sys/bus/pci/slots/control/add_slot
 *   echo "<DRC 이름>" > /sys/bus/pci/slots/control/remove_slot
 * 로 슬롯을 추가·제거한다. DRC(Dynamic Reconfiguration Connector)는 펌웨어가
 * 각 슬롯에 붙여 둔 논리 식별자로, rpaphp 계열 드라이버가 공통으로 쓰는 이름이다.
 * 실제 작업은 전혀 하지 않는다 — 문자열을 다듬어 dlpar_add_slot() /
 * dlpar_remove_slot() 에 넘기는 것이 전부이고, 펌웨어(RTAS) 호출과 PCI 열거는
 * rpadlpar_core.c 가 담당한다. 이 파일의 실질적인 일은 세 가지다.
 * (1) sysfs 가 주는 buf 가 널로 끝나지 않을 수 있으므로 길이를 명시해 복사하고,
 * (2) echo 가 붙인 개행을 잘라 내고(그러지 않으면 DRC 이름 비교가 실패한다),
 * (3) 소비한 바이트 수를 돌려주어 사용자 공간이 쓰기 완료로 인식하게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DLPAR 스택은 세 층이다. 이 파일이 최상단의 사용자 인터페이스이고, 그 아래
 * rpadlpar_core.c 가 DRC 이름으로 슬롯을 찾아 추가·제거 절차를 진행하며,
 * 최하단에서 rpaphp 계열(rpaphp_core.c / rpaphp_pci.c / rpaphp_slot.c)이
 * RTAS 호출과 PCI 열거를 수행한다.
 * 다른 핫플러그 sysfs 와 결정적으로 다른 점은 이것이 슬롯 하나에 붙는 속성이
 * 아니라는 것이다. 슬롯이 아직 존재하지 않을 때도 추가를 요청할 수 있어야
 * 하므로, 개별 슬롯의 kobject 가 아니라 슬롯 kset(/sys/bus/pci/slots) 아래에
 * 별도 디렉터리를 만든다. 그 kset 이 PCI 서브시스템 내부 심볼이라 이 파일이
 * "../pci.h" 를 상대 경로로 포함해야 했다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. store 콜백은 사용자의 write()
 * 시스템 콜 문맥에서 불리고, 그 안에서 RTAS 호출과 PCI 열거·제거가 일어나므로
 * 오래 걸리고 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: sysfs/kobject 계층 — kobject_create_and_add(), sysfs_create_group(),
 * __ATTR() 매크로, struct kobj_attribute. 슬롯 kset 인 pci_slots_kset 은
 * drivers/pci/pci.h 소유라 내부 헤더를 포함해 가져온다.
 * 아래쪽: rpadlpar.h 의 dlpar_add_slot()/dlpar_remove_slot() 두 함수가
 * 이 파일과 DLPAR 코어를 잇는 전부다.
 * 옆쪽: rpaphp.h 의 MAX_DRC_NAME_LEN — 스택 버퍼 크기이자 입력 길이 상한이다.
 * 데이터 흐름은 단방향이다. 사용자가 쓴 DRC 이름 → 길이 검사 → 스택 버퍼로
 * 복사 → 개행 제거 → DLPAR 코어 → RTAS/PCI 열거. 반대 방향으로 돌아오는
 * 정보는 없다 — 두 show 콜백이 언제나 "0" 을 돌려주는 이유다.
 * 공유 상태: 파일 범위 전역 dlpar_kobj 하나뿐이며, 모듈 적재·해제 시점에만
 * 접근하므로 락이 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - add_slot_store() / remove_slot_store(): 이 파일의 본체. 구조가 완전히
 *   같고 마지막에 부르는 DLPAR 함수만 다르다 — 상류가 공통 부분을 헬퍼로
 *   빼지 않아 거의 복제된 코드다.
 * - add_slot_show() / remove_slot_show(): 언제나 "0" 을 돌려주는 자리 채우기.
 *   이 속성은 본질적으로 쓰기 전용 명령 인터페이스인데 __ATTR 이 권한을
 *   0644 로 두어 읽기도 허용하므로 show 콜백이 필요했다.
 * - dlpar_sysfs_init() / dlpar_sysfs_exit(): 디렉터리와 속성 그룹을 만들고
 *   지운다. 속성을 그룹으로 다루는 이유는 부분 실패 시 정리가 자동이기 때문이다.
 * - ADD_SLOT_ATTR_NAME / REMOVE_SLOT_ATTR_NAME: 위 영어 주석이 밝히듯
 *   __ATTR() 이 인자를 문자열로 바꾸므로 따옴표를 붙이면 안 되는 매크로다.
 * - [상류 코드 관찰, 수정하지 않음] 세 가지를 기록해 둔다. (a) 입력이 버퍼보다
 *   길면 오류가 아니라 0 을 돌려주어, 사용자 공간이 무한 재시도에 빠질 수 있다.
 *   (b) kobject 생성 실패에 -ENOMEM 이 아니라 -EINVAL 을 돌려준다.
 *   (c) 그룹 생성 실패 시 dlpar_kobj 를 NULL 로 되돌리지 않아, 그 뒤 exit 이
 *   불리면 해제된 포인터를 만지게 된다 — 호출자가 그러지 않는다는 전제다.
 */

#include <linux/kobject.h>
/* [한국어] strscpy()/strchr() 선언. */
#include <linux/string.h>
/* [한국어] PCI 코어 정의. */
#include <linux/pci.h>
/* [한국어] pci_slots_kset 선언 — /sys/bus/pci/slots 를 나타내는 kset 이며,
 * 이 파일이 만드는 디렉터리의 부모가 된다. */
#include <linux/pci_hotplug.h>
/* [한국어] rpaphp 자체 헤더 — MAX_DRC_NAME_LEN 이 여기서 온다. */
#include "rpaphp.h"
/* [한국어] DLPAR 헤더 — dlpar_add_slot()/dlpar_remove_slot() 선언. */
#include "rpadlpar.h"
/* [한국어] PCI 서브시스템 내부 헤더. 외부 모듈에 공개되지 않는 pci_slots_kset 을
 * 쓰기 위해 상대 경로로 포함한다. */
#include "../pci.h"

/* [한국어] 만들 sysfs 디렉터리 이름. 결과 경로는 /sys/bus/pci/slots/control 이 된다. */
#define DLPAR_KOBJ_NAME       "control"

/* Those two have no quotes because they are passed to __ATTR() which
 * stringifies the argument (yuck !)
 */
/* [한국어] 속성 파일 이름. 위 영어 주석이 밝히듯 __ATTR() 매크로가 인자를 문자열로
 * 바꾸므로 따옴표를 붙이면 안 된다. */
#define ADD_SLOT_ATTR_NAME    add_slot
/* [한국어] 제거 쪽 속성 이름. 같은 이유로 따옴표가 없다. */
#define REMOVE_SLOT_ATTR_NAME remove_slot

/* [한국어]
 * add_slot_store - sysfs 쓰기로 받은 DRC 이름의 슬롯을 논리 파티션에 추가한다
 *
 * @kobj: 속성이 붙은 kobject. 쓰지 않는다 — 이 인터페이스는 시스템 전역이라
 *       어느 슬롯에 대한 것인지 구분할 필요가 없다.
 * @attr: 어떤 속성이 쓰였는지. 역시 쓰지 않는다.
 * @buf: 사용자가 쓴 데이터. 널로 끝난다는 보장이 없다.
 * @nbytes: 그 길이.
 * @return: 성공 시 소비한 바이트 수(nbytes), 실패 시 음수 errno.
 *       길이 초과 시에는 0(아래 관찰 참조).
 *
 * DLPAR(Dynamic Logical Partitioning)은 IBM POWER 의 기능으로, 동작 중인
 * 논리 파티션에 I/O 슬롯을 넣고 뺄 수 있게 한다. 이 파일이 그 조작을 위한
 * 사용자 인터페이스이며, 관리자가
 *   echo "<DRC 이름>" > /sys/bus/pci/slots/control/add_slot
 * 로 슬롯을 추가한다.
 *
 * 동작 과정:
 *   1) 길이를 검사한다.
 *   2) strscpy 로 정확히 nbytes 바이트를 복사하고 널을 붙인다. sysfs 가 주는
 *      buf 는 널로 끝나지 않을 수 있어 길이를 명시해야 한다.
 *   3) echo 가 붙인 개행을 잘라 낸다. 이 처리가 없으면 DRC 이름 비교가 실패한다.
 *   4) dlpar_add_slot() 에 넘긴다 — 실제 펌웨어(RTAS) 호출과 PCI 열거는 거기서
 *      이루어진다.
 *
 * [상류 코드 관찰, 수정하지 않음] 길이 초과 시 오류가 아니라 0 을 돌려준다.
 * write() 반환값 0 은 "아무것도 쓰지 않았다"는 뜻이라 사용자 공간이 무한
 * 재시도에 빠질 수 있다. -EINVAL 이 더 적절해 보인다.
 *
 * 실행 컨텍스트: write() 시스템 콜 문맥, 프로세스 컨텍스트.
 * DLPAR 코어가 RTAS 호출과 PCI 열거를 하므로 오래 걸리고 잠들 수 있다.
 *
 * 에러 경로: dlpar_add_slot() 의 errno 를 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자의 echo > /sys/bus/pci/slots/control/add_slot
 *     → sysfs → kobj_attribute.store == [이 함수] → dlpar_add_slot()
 */
static ssize_t add_slot_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t nbytes)
{
	/* [한국어] DRC 이름을 담을 스택 버퍼. 크기를 MAX_DRC_NAME_LEN 으로 고정한다. */
	char drc_name[MAX_DRC_NAME_LEN];
	/* [한국어] 개행 문자를 찾을 포인터. */
	char *end;
	/* [한국어] dlpar_add_slot() 결과. */
	int rc;

	/* [한국어] 사용자가 버퍼보다 긴 문자열을 쓰면, */
	if (nbytes >= MAX_DRC_NAME_LEN)
		/* [한국어] [상류 코드 관찰] 오류가 아니라 0 을 돌려준다. write() 반환값 0 은
		 * "아무것도 쓰지 않았다"는 뜻이라, 사용자 공간이 무한 재시도에 빠질 수 있다.
		 * -EINVAL 이 더 적절해 보이지만 상류 그대로 둔다. */
		return 0;

	/* [한국어] nbytes + 1 을 크기로 넘겨 정확히 nbytes 바이트를 복사하고 널을 붙인다.
	 * sysfs 가 넘겨 주는 buf 는 널로 끝나지 않을 수 있어 길이를 명시해야 한다. */
	strscpy(drc_name, buf, nbytes + 1);

	/* [한국어] echo 로 쓰면 개행이 딸려 오므로 찾는다. */
	end = strchr(drc_name, '\n');
	/* [한국어] 있으면, */
	if (end)
		/* [한국어] 널로 바꿔 잘라 낸다. 이 처리가 없으면 DRC 이름 비교가 실패한다. */
		*end = '\0';

	/* [한국어] 실제 슬롯 추가를 DLPAR 코어에 맡긴다. 그 안에서 펌웨어(RTAS) 호출과
	 * PCI 열거가 이루어진다. */
	rc = dlpar_add_slot(drc_name);
	/* [한국어] 실패 검사. */
	if (rc)
		/* [한국어] errno 를 그대로 돌려준다 — 사용자의 write() 가 그 값으로 실패한다. */
		return rc;

	/* [한국어] 성공하면 소비한 바이트 수를 돌려준다. sysfs 규약상 이 값이 nbytes 와
	 * 같아야 사용자 공간이 쓰기 완료로 인식한다. */
	return nbytes;
}

/* [한국어]
 * add_slot_show - 읽기 요청에 언제나 "0" 을 돌려준다
 *
 * @kobj: 쓰지 않는다.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * [상류 코드 관찰] 이 속성은 본질적으로 쓰기 전용 명령 인터페이스인데,
 * __ATTR() 이 권한을 0644 로 두어 읽기도 허용한다. 그래서 자리를 채울 show
 * 콜백이 필요했고, 의미 있는 상태가 없으므로 고정값 "0" 을 돌려준다.
 * 읽어서 얻을 정보는 없다.
 *
 * 실행 컨텍스트: read() 시스템 콜 문맥.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 cat /sys/bus/pci/slots/control/add_slot
 *     → sysfs → kobj_attribute.show == [이 함수]
 */
static ssize_t add_slot_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	/* [한국어] 언제나 "0" 한 줄을 돌려준다. [상류 코드 관찰] show 콜백이 언제나 "0" 을 돌려준다. 이 속성은 쓰기 전용
	 * 인터페이스인데, __ATTR 이 권한을 0644 로 두어 읽기도 허용하므로 자리를
	 * 채울 함수가 필요했던 것으로 보인다. 읽어도 의미 있는 정보는 없다. */
	return sysfs_emit(buf, "0\n");
}

/* [한국어]
 * remove_slot_store - sysfs 쓰기로 받은 DRC 이름의 슬롯을 논리 파티션에서 제거한다
 *
 * @kobj: 쓰지 않는다.
 * @attr: 쓰지 않는다.
 * @buf: 사용자가 쓴 DRC 이름.
 * @nbytes: 그 길이.
 * @return: 성공 시 nbytes, 실패 시 음수 errno, 길이 초과 시 0.
 *
 * add_slot_store 와 구조가 완전히 같고 마지막에 부르는 함수만 다르다 —
 * 길이 검사, strscpy, 개행 제거, DLPAR 코어 호출 순서까지 동일하다.
 * 두 함수가 거의 복제인 것은 상류의 선택이며, 공통 부분을 헬퍼로 빼지 않았다.
 *
 * 실행 컨텍스트: write() 시스템 콜 문맥. 슬롯 제거는 하위 장치의 드라이버
 * remove 를 유발하므로 오래 걸릴 수 있다.
 *
 * 에러 경로: dlpar_remove_slot() 의 errno 를 그대로 전달한다.
 * 길이 초과 시 0 을 돌려주는 문제도 add 쪽과 같다.
 *
 * 호출 체인:
 *   사용자의 echo > /sys/bus/pci/slots/control/remove_slot
 *     → sysfs → kobj_attribute.store == [이 함수] → dlpar_remove_slot()
 */
static ssize_t remove_slot_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t nbytes)
{
	/* [한국어] DRC 이름 버퍼. */
	char drc_name[MAX_DRC_NAME_LEN];
	/* [한국어] dlpar_remove_slot() 결과. */
	int rc;
	/* [한국어] 개행 탐색용 포인터. add 쪽과 선언 순서만 다르고 내용은 같다. */
	char *end;

	/* [한국어] 길이 검사. */
	if (nbytes >= MAX_DRC_NAME_LEN)
		/* [한국어] add 쪽과 같은 0 반환. */
		return 0;

	/* [한국어] 정확한 길이만큼 복사한다. */
	strscpy(drc_name, buf, nbytes + 1);

	/* [한국어] 개행 탐색. */
	end = strchr(drc_name, '\n');
	/* [한국어] 있으면, */
	if (end)
		/* [한국어] 잘라 낸다. */
		*end = '\0';

	/* [한국어] 실제 슬롯 제거를 DLPAR 코어에 맡긴다. */
	rc = dlpar_remove_slot(drc_name);
	/* [한국어] 실패 검사. */
	if (rc)
		/* [한국어] errno 전달. */
		return rc;

	/* [한국어] 소비한 바이트 수 반환. */
	return nbytes;
}

/* [한국어]
 * remove_slot_show - 읽기 요청에 언제나 "0" 을 돌려준다
 *
 * @kobj: 쓰지 않는다.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * add_slot_show 와 완전히 같은 이유로 존재하는 자리 채우기 함수다.
 *
 * 실행 컨텍스트: read() 시스템 콜 문맥.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 cat /sys/bus/pci/slots/control/remove_slot
 *     → sysfs → kobj_attribute.show == [이 함수]
 */
static ssize_t remove_slot_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	/* [한국어] 언제나 "0". [상류 코드 관찰] show 콜백이 언제나 "0" 을 돌려준다. 이 속성은 쓰기 전용
	 * 인터페이스인데, __ATTR 이 권한을 0644 로 두어 읽기도 허용하므로 자리를
	 * 채울 함수가 필요했던 것으로 보인다. 읽어도 의미 있는 정보는 없다. */
	return sysfs_emit(buf, "0\n");
}

static struct kobj_attribute add_slot_attr =
	/* [한국어] add_slot 속성을 정의한다. 권한 0644 는 소유자 쓰기와 모두 읽기다 —
	 * 슬롯 추가가 시스템 구성을 바꾸는 일이라 root 만 쓸 수 있어야 한다. */
	__ATTR(ADD_SLOT_ATTR_NAME, 0644, add_slot_show, add_slot_store);

static struct kobj_attribute remove_slot_attr =
	/* [한국어] remove_slot 속성. 같은 권한이다. */
	__ATTR(REMOVE_SLOT_ATTR_NAME, 0644, remove_slot_show, remove_slot_store);

static struct attribute *default_attrs[] = {
	/* [한국어] 속성 배열에 add_slot 을 넣는다. .attr 로 내려가는 것은 kobj_attribute 가
	 * struct attribute 를 첫 멤버로 품고 있기 때문이다. */
	&add_slot_attr.attr,
	/* [한국어] remove_slot 도 넣는다. */
	&remove_slot_attr.attr,
	/* [한국어] 배열의 끝을 알리는 NULL. */
	NULL,
};

static const struct attribute_group dlpar_attr_group = {
	/* [한국어] 위 배열을 그룹으로 묶는다. 그룹 단위로 만들고 지우면 부분 실패 시
	 * 정리가 자동으로 되어 개별 생성보다 안전하다. */
	.attrs = default_attrs,
};

/* [한국어] 만든 디렉터리의 kobject. 파일 범위 전역이며 init 이 만들고 exit 이 놓는다.
 * 설정자: dlpar_sysfs_init().
 * 읽는 자: dlpar_sysfs_exit().
 * 값 범위: 유효 포인터 또는 NULL(아직 만들지 않음).
 * 동기화: 모듈 적재·해제 시점에만 접근하므로 락이 없다. */
static struct kobject *dlpar_kobj;

/* [한국어]
 * dlpar_sysfs_init - /sys/bus/pci/slots/control 디렉터리와 두 속성 파일을 만든다
 *
 * @return: 0 = 성공. -EINVAL = 디렉터리 생성 실패.
 *       그 밖의 음수 = 속성 그룹 생성 실패.
 *
 * 슬롯 하나에 붙는 속성이 아니라 시스템 전역 명령 인터페이스이므로, 개별
 * 슬롯의 kobject 가 아니라 슬롯 kset(/sys/bus/pci/slots) 아래에 별도
 * 디렉터리를 만든다. 그 kset 이 PCI 서브시스템 내부 심볼이라 이 파일이
 * "../pci.h" 를 포함해야 했다.
 *
 * 속성을 그룹으로 만드는 이유는 부분 실패 시 정리가 자동으로 되기 때문이다 —
 * 개별 생성이라면 첫 파일은 성공하고 둘째가 실패했을 때 직접 되돌려야 한다.
 *
 * [상류 코드 관찰, 수정하지 않음] kobject_create_and_add() 실패에 -EINVAL 을
 * 돌려주는데, 그 함수는 대개 메모리 부족으로 실패하므로 -ENOMEM 이 자연스럽다.
 * 또 그룹 생성 실패 시 kobject_put() 만 하고 dlpar_kobj 를 NULL 로 되돌리지
 * 않아, 그 뒤 exit 이 불리면 해제된 포인터를 만지게 된다 — 호출자가 init 실패
 * 시 exit 을 부르지 않는다는 전제 위에 있다.
 *
 * 실행 컨텍스트: 모듈 적재 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 errno 를 돌려준다.
 *
 * 호출 체인:
 *   rpadlpar_core.c 의 모듈 init → [dlpar_sysfs_init]
 *     → kobject_create_and_add() → sysfs_create_group()
 */
int dlpar_sysfs_init(void)
{
	/* [한국어] sysfs_create_group() 결과. */
	int error;

	/* [한국어] /sys/bus/pci/slots 아래에 "control" 디렉터리를 만든다. 슬롯 kset 을 부모로
	 * 삼는 이유는 이 인터페이스가 슬롯 관리에 속하기 때문이며, 그래서 PCI
	 * 서브시스템 내부 헤더를 포함해야 했다. */
	dlpar_kobj = kobject_create_and_add(DLPAR_KOBJ_NAME,
					    &pci_slots_kset->kobj);
	/* [한국어] 생성 실패. */
	if (!dlpar_kobj)
		/* [한국어] [상류 코드 관찰] kobject_create_and_add() 는 대개 메모리 부족으로 실패하므로
		 * -ENOMEM 이 자연스럽지만 -EINVAL 을 돌려준다. */
		return -EINVAL;

	/* [한국어] 두 속성 파일을 한꺼번에 만든다. */
	error = sysfs_create_group(dlpar_kobj, &dlpar_attr_group);
	/* [한국어] 실패하면, */
	if (error)
		/* [한국어] 방금 만든 kobject 의 참조를 놓는다. 마지막 참조라면 여기서 해제되고
		 * 디렉터리도 사라진다. 다만 dlpar_kobj 를 NULL 로 되돌리지는 않아,
		 * 실패 후 exit 이 불리면 이미 해제된 포인터를 만지게 된다 —
		 * 호출자가 init 실패 시 exit 을 부르지 않는다는 전제 위에 있다. */
		kobject_put(dlpar_kobj);
	/* [한국어] 성공이면 0, 실패면 sysfs 오류가 그대로 나간다. */
	return error;
}

/* [한국어]
 * dlpar_sysfs_exit - 속성 파일과 디렉터리를 제거한다
 *
 * init 의 짝이다. 순서가 중요하다 — 속성 파일을 먼저 지우고 디렉터리를
 * 나중에 없앤다. 반대로 하면 파일이 남아 있는 디렉터리를 해제하려는 셈이 된다.
 *
 * kobject_put() 은 참조 카운트를 내리는 것이라, 마지막 참조일 때만 실제로
 * 해제된다. 사용자가 그 디렉터리 안의 파일을 열어 두고 있으면 그 참조가
 * 풀릴 때까지 실제 해제가 미뤄진다.
 *
 * 실행 컨텍스트: 모듈 해제 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rpadlpar_core.c 의 모듈 exit → [dlpar_sysfs_exit]
 *     → sysfs_remove_group() → kobject_put()
 */
void dlpar_sysfs_exit(void)
{
	/* [한국어] 두 속성 파일을 제거한다. */
	sysfs_remove_group(dlpar_kobj, &dlpar_attr_group);
	/* [한국어] kobject 참조를 놓아 디렉터리를 없앤다. 파일을 먼저 지우고 디렉터리를
	 * 나중에 없애는 순서가 중요하다. */
	kobject_put(dlpar_kobj);
}
