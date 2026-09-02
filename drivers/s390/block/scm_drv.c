// SPDX-License-Identifier: GPL-2.0
/*
 * Device driver for s390 storage class memory.
 *
 * Copyright IBM Corp. 2012
 * Author(s): Sebastian Ott <sebott@linux.vnet.ibm.com>
 */

/*
 * [한국어 설명] s390 SCM 블록 드라이버의 버스 결합부 (scm_drv.c)
 *
 * === 파일의 역할 ===
 * SCM(Storage Class Memory) 블록 드라이버를 **EADM 버스에 등록하는 얇은
 * 결합부** 다. 이 파일에는 I/O 처리가 하나도 없다 — 장치가 붙고 떨어지고
 * 상태가 바뀌는 세 가지 사건만 다루고, 실제 블록 계층 작업은 모두
 * scm_blk.c 에 넘긴다.
 *
 * 그래서 이 파일을 읽는 요령은 **struct scm_driver 표 하나** 를 보는
 * 것이다. 그 표의 네 항목이 이 드라이버가 버스와 주고받는 접점 전부다.
 *   .probe   -> 장치가 붙었다. 상태 구조체를 만들어 블록 장치를 세운다.
 *   .remove  -> 장치가 떨어졌다. 그 반대로 정리한다.
 *   .notify  -> 장치 상태가 바뀌었다. 두 종류의 알림을 구별해 처리한다.
 *   .handler -> I/O 가 끝났다. **이것만 이 파일에 구현이 없고**
 *               scm_blk.c 의 함수를 곧바로 가리킨다.
 *
 * 마지막 항목이 이 파일의 성격을 잘 보여 준다. 완료 처리는 인터럽트
 * 문맥에서 도는 성능 경로라 결합부를 한 겹 두지 않고 직결한 것이다.
 *
 * **이 트리에서 확인할 수 없는 것.** struct scm_device, struct scm_driver,
 * enum scm_event, OP_STATE_GOOD, scm_driver_register()/unregister() 는
 * 모두 asm/eadm.h 소관인데, 그 헤더는 arch/s390 에 있고 이 트리는 sparse
 * checkout 이라 arch/ 가 없다. 아래 주석은 호출 자리에서 읽히는 쓰임새까지만
 * 적었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 모듈이 올라올 때:
 *   scm_blk.c 의 모듈 초기화 -> scm_drv_init() [이 파일]
 *     -> scm_driver_register() 로 EADM 버스에 표를 건다
 *
 * 장치가 붙을 때:
 *   EADM 버스 -> scm_probe() [이 파일]
 *     -> 상태를 확인하고 struct scm_blk_dev 를 0 으로 할당
 *       -> drvdata 에 매단 뒤 scm_blk_dev_setup() [scm_blk.c]
 *          -> blk-mq 태그 집합과 gendisk 를 만들어 블록 계층에 등록
 *
 * 상태가 바뀔 때:
 *   EADM 버스 -> scm_notify() [이 파일]
 *     -> SCM_CHANGE 면 기록만 남기고
 *     -> SCM_AVAIL 이면 scm_blk_set_available() 로 쓰기를 다시 허용
 *
 * I/O 가 끝날 때:
 *   EADM 버스 -> scm_blk_irq() [scm_blk.c, 이 파일을 거치지 않는다]
 *
 * 실행 컨텍스트: probe/remove/notify 는 프로세스 컨텍스트다. 할당과
 * 블록 장치 등록이 있어 잠들 수 있어야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 없다. 이 파일을 부르는 것은 EADM 버스뿐이다.
 * 아래쪽: scm_blk.c. scm_blk.h 가 선언한 네 함수
 *   (scm_blk_dev_setup, scm_blk_dev_cleanup, scm_blk_set_available,
 *   scm_blk_irq)로만 이어지며, 자료구조 내부는 건드리지 않는다.
 * 옆쪽: EADM 버스(asm/eadm.h). 이 트리에 없다.
 *
 * 데이터 흐름:
 *   struct scm_device (버스가 준다) -> scm_probe
 *     -> struct scm_blk_dev 를 만들어 drvdata 에 매단다
 *   이후 모든 콜백이 그 drvdata 를 되찾아 쓴다 — 즉 **drvdata 가 이
 *   파일과 scm_blk.c 를 잇는 유일한 끈** 이다.
 *
 * 공유 상태: 없다. 이 파일에는 전역 변수가 아래 드라이버 표 하나뿐이고
 *   그것은 상수처럼 쓰인다. 장치별 상태는 모두 drvdata 안에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * scm_probe()      : 상태를 확인하고 블록 장치를 세운다.
 * scm_remove()     : 그 반대로 정리한다.
 * scm_notify()     : 두 종류의 상태 변경 알림을 구별해 처리한다.
 * scm_drv_init()   : 버스에 드라이버 표를 등록한다.
 * scm_drv_cleanup(): 그 등록을 해제한다.
 * scm_drv          : 위 네 콜백을 담은 드라이버 표. 이 파일의 중심이다.
 */

/* [한국어] 커널 로그 접두사를 정한다. 이 파일과 scm_blk.c 의 모든 pr_ 계열
 * 출력 앞에 "scm_block: " 이 붙어, 다른 s390 드라이버의 로그와 구별된다. */
#define pr_fmt(fmt) "scm_block: " fmt

/* [한국어] MODULE_ 계열 선언과 __init 표시. */
#include <linux/module.h>
/* [한국어] kzalloc_obj/kfree — 장치별 상태 구조체를 할당하고 푼다. */
#include <linux/slab.h>
/* [한국어] EADM 인터페이스. struct scm_device, scm_driver, enum scm_event,
 * OP_STATE_GOOD, scm_driver_register() 가 모두 여기 있으며 arch/s390
 * 소관이라 이 트리에서 확인 못 함. */
#include <asm/eadm.h>
/* [한국어] 같은 디렉터리의 공용 헤더. struct scm_blk_dev 와 로그 매크로,
 * 그리고 scm_blk.c 가 구현하는 네 함수의 선언이 여기 있다. */
#include "scm_blk.h"

/* [한국어]
 * scm_notify - 장치 상태 변경 알림을 두 종류로 나눠 처리한다
 *
 * @scmdev: 알림을 낸 SCM 장치.
 * @event: 알림 종류. SCM_CHANGE 또는 SCM_AVAIL 이다.
 *
 * EADM 버스가 장치의 상태가 바뀌었다고 알릴 때 불린다.
 *
 * 두 갈래인데 **하는 일의 무게가 확연히 다르다.**
 *   - SCM_CHANGE: 증분의 능력(capabilities)이 바뀌었다. **기록만 남기고
 *     끝난다** — 커널 로그와 debug 기록 양쪽에 남기지만, 블록 계층에는
 *     아무것도 알리지 않는다.
 *   - SCM_AVAIL: 증분을 다시 쓸 수 있게 됐다. 이쪽은 기록에 더해
 *     scm_blk_set_available() 을 불러 **쓰기 금지 상태를 풀어 준다.**
 *
 * 즉 이 함수의 실질적인 일은 SCM_AVAIL 한 갈래뿐이고, 나머지는 관측용이다.
 *
 * 기록을 세 겹으로 남기는 것이 눈에 띈다 — 사람이 읽는 pr_info(SCM_CHANGE
 * 에만), 짧은 문자열 기록, 그리고 장치 상태를 담은 이진 기록이다. 마지막
 * 것이 나중에 문제를 되짚을 때 어느 장치가 어떤 상태였는지 알려 준다.
 *
 * [상류 코드 관찰] 함수 첫머리에서 drvdata 를 꺼내지만 **SCM_CHANGE
 * 갈래에서는 그 값을 쓰지 않는다.** 두 갈래 모두에 필요한 것처럼 앞에
 * 두었으나 실제로 쓰는 것은 SCM_AVAIL 뿐이다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] switch 에 default 갈래가 없다. asm/eadm.h 의 enum 에
 * 값이 더해지면 이 함수는 조용히 아무것도 하지 않게 된다. 다만 열거형을
 * 빠짐없이 다루면 컴파일러가 경고해 주므로 의도된 형태로 보인다.
 *
 * 실행 컨텍스트: EADM 버스의 알림 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 라 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   EADM 버스 → scm_driver.notify == [이 함수]
 *     → SCM_LOG() → SCM_LOG_STATE() → scm_blk_set_available()
 */
static void scm_notify(struct scm_device *scmdev, enum scm_event event)
{
	/* [한국어] drvdata 에서 이 장치의 블록 장치 상태를 되찾는다. probe 가 매달아 둔
	 * 값이며, 이 파일과 scm_blk.c 를 잇는 유일한 끈이다.
	 * [상류 코드 관찰] 아래 SCM_CHANGE 갈래에서는 이 값을 쓰지 않는다. */
	struct scm_blk_dev *bdev = dev_get_drvdata(&scmdev->dev);

	/* [한국어] 알림 종류로 갈린다. 두 값 모두 asm/eadm.h 가 정의한다. */
	switch (event) {
	/* [한국어] 증분의 능력이 바뀐 경우다. */
	case SCM_CHANGE:
		/* [한국어] 사람이 읽는 로그를 남긴다. **이 갈래에만 pr_info 가 있다** —
		 * 관리자가 알아야 할 변화로 본 것이다. */
		pr_info("%lx: The capabilities of the SCM increment changed\n",
			(unsigned long) scmdev->address);
		SCM_LOG(2, "State changed");
		/* [한국어] 장치 상태를 이진 기록으로 남긴다. 나중에 되짚을 때 쓴다. */
		SCM_LOG_STATE(2, scmdev);
		/* [한국어] 여기서 끝난다. **블록 계층에는 아무것도 알리지 않는다** — 능력이
		 * 바뀌었다는 사실만 관측용으로 남긴다. */
		break;
	case SCM_AVAIL:
		/* [한국어] 증분을 다시 쓸 수 있게 된 경우다. 이쪽은 짧은 기록만 남긴다. */
		SCM_LOG(2, "Increment available");
		SCM_LOG_STATE(2, scmdev);
		/* [한국어] **이 함수에서 유일하게 실질적인 일을 하는 줄.** 쓰기 금지 상태를
		 * 풀어 준다. 그 안에서 scm_blk.c 가 잠금을 잡고 state 를 SCM_OPER 로
		 * 되돌린다. */
		scm_blk_set_available(bdev);
		break;
	}
}

/* [한국어]
 * scm_probe - 장치를 확인하고 블록 장치를 세운다
 *
 * @scmdev: 붙은 SCM 장치.
 * @return: 0 = 성공, -EINVAL = 쓸 수 없는 상태, -ENOMEM = 할당 실패,
 *          그 밖은 블록 장치 등록의 오류.
 *
 * EADM 버스가 이 드라이버에 맞는 장치를 찾았을 때 불린다.
 *
 * 네 단계다.
 * 1. **들어온 사실을 먼저 기록한다.** 실패하더라도 어떤 상태의 장치가
 *    왔는지 debug 기록에 남게 하려는 배치다.
 * 2. **동작 상태를 확인한다.** OP_STATE_GOOD 이 아니면 쓸 수 없는
 *    증분이므로 곧바로 물러난다. 그 값의 정의는 asm/eadm.h 소관이라
 *    이 트리에서 확인 못 함.
 * 3. 상태 구조체를 **0 으로 채워** 할당한다. 그 0 초기화가 중요한데,
 *    scm_blk.h 의 여러 필드가 "아직 설정되지 않음" 을 NULL 이나 0 으로
 *    나타내기 때문이다.
 * 4. **drvdata 에 먼저 매단 뒤** 블록 장치를 세운다. 순서가 이래야 하는
 *    이유는 아래 setup 이 도중에 블록 계층을 등록하고, 그 뒤로는 다른
 *    콜백이 언제든 drvdata 를 찾을 수 있어야 하기 때문이다.
 *
 * 되감기가 조심스럽다 — setup 이 실패하면 drvdata 를 **NULL 로 되돌린
 * 뒤** 메모리를 푼다. 되돌리지 않으면 이미 해제된 곳을 가리키는 포인터가
 * 장치에 남는다.
 *
 * [상류 코드 관찰] 마지막의 `goto out` 이 바로 다음 줄인 out 라벨로
 * 뛴다 — 즉 아무 데도 가지 않는 것과 같다. 되감기 코드가 그 라벨 앞이
 * 아니라 if 블록 안에 있어서 생긴 형태이며, 지금은 라벨과 goto 가 모두
 * 없어도 동작이 같다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: EADM 버스의 probe. 프로세스 컨텍스트이며 할당과 블록
 * 장치 등록으로 잠들 수 있다.
 *
 * 에러 경로: 위 셋. 어느 경우에도 버스에 남기는 흔적이 없도록 정리한다.
 *
 * 호출 체인:
 *   EADM 버스 → scm_driver.probe == [이 함수]
 *     → SCM_LOG() → SCM_LOG_STATE() → kzalloc_obj()
 *     → dev_set_drvdata() → scm_blk_dev_setup()
 */
static int scm_probe(struct scm_device *scmdev)
{
	struct scm_blk_dev *bdev;
	int ret;
/* [한국어]  */

	SCM_LOG(2, "probe");
	/* [한국어] 어떤 상태의 장치가 들어왔는지 **먼저** 기록한다. 아래에서 물러나더라도
	 * 기록은 남게 하려는 배치다. */
	SCM_LOG_STATE(2, scmdev);
/* [한국어]  */

	if (scmdev->attrs.oper_state != OP_STATE_GOOD)
		/* [한국어] 쓸 수 없는 상태의 증분이다. 인자가 잘못됐다는 뜻으로 물러난다 —
		 * 아직 할당한 것이 없어 되감을 것도 없다. */
		return -EINVAL;

	bdev = kzalloc_obj(*bdev);
	/* [한국어] 메모리 부족이다. */
	if (!bdev)
		/* [한국어] 곧바로 물러난다. */
		return -ENOMEM;

	dev_set_drvdata(&scmdev->dev, bdev);
	/* [한국어] 블록 장치를 세운다. 이 안에서 blk-mq 태그 집합과 gendisk 를 만들어
	 * 블록 계층에 등록한다. */
	ret = scm_blk_dev_setup(bdev, scmdev);
	/* [한국어] 등록이 실패했다. */
	if (ret) {
		/* [한국어] **메모리를 풀기 전에** drvdata 를 되돌린다. 순서가 반대면 장치에
		 * 이미 해제된 곳을 가리키는 포인터가 남는다. */
		dev_set_drvdata(&scmdev->dev, NULL);
		/* [한국어] 상태 구조체를 푼다. */
		kfree(bdev);
		goto out;
	}

out:
	return ret;
}

/* [한국어]
 * scm_remove - 블록 장치를 내리고 상태 구조체를 푼다
 *
 * @scmdev: 떨어지는 SCM 장치.
 *
 * scm_probe() 의 정확한 역순이다 — 블록 장치를 내리고, drvdata 를 NULL 로
 * 되돌리고, 메모리를 푼다.
 *
 * **순서가 중요하다.** 블록 장치를 가장 먼저 내려야 한다. 그러지 않으면
 * 아직 살아 있는 gendisk 아래로 I/O 가 들어와 이미 해제된 구조체를
 * 건드리게 된다.
 *
 * drvdata 를 NULL 로 되돌리는 것이 메모리를 푸는 것보다 앞선 것도 같은
 * 이유다 — 그 사이에 다른 콜백이 drvdata 를 읽어도 NULL 을 보게 된다.
 *
 * probe 와 달리 상태 기록을 남기지 않는다. 떨어지는 장치의 상태는
 * 되짚을 일이 없다고 본 것으로 보인다.
 *
 * [상류 코드 관찰] drvdata 가 NULL 인 경우를 확인하지 않는다. probe 가
 * 실패한 장치에는 remove 가 불리지 않는다는 전제이며, 그것은 드라이버
 * 코어의 일반적인 규약이다.
 *
 * 실행 컨텍스트: EADM 버스의 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   EADM 버스 → scm_driver.remove == [이 함수]
 *     → scm_blk_dev_cleanup() → dev_set_drvdata(NULL) → kfree()
 */
static void scm_remove(struct scm_device *scmdev)
{
	struct scm_blk_dev *bdev = dev_get_drvdata(&scmdev->dev);

	scm_blk_dev_cleanup(bdev);
	dev_set_drvdata(&scmdev->dev, NULL);
	/* [한국어] 상태 구조체를 푼다. 위에서 drvdata 를 이미 NULL 로 돌려놓았으므로
	 * 남는 포인터가 없다. */
	kfree(bdev);
}

static struct scm_driver scm_drv = {
	/* [한국어] 이 드라이버의 이름과 소유 모듈. 버스 공통 부분이다. */
	.drv = {
		/* [한국어] sysfs 에 보일 이름. */
		.name = "scm_block",
		/* [한국어] 소유 모듈을 밝혀, 장치가 붙어 있는 동안 모듈이 내려가지 않게 한다. */
		.owner = THIS_MODULE,
	},
	/* [한국어] 상태 변경 알림을 받을 함수. */
	.notify = scm_notify,
	/* [한국어] 장치가 붙을 때 부를 함수. */
	.probe = scm_probe,
	/* [한국어] 장치가 떨어질 때 부를 함수. */
	.remove = scm_remove,
	/* [한국어] **완료 인터럽트 처리기.** 이 항목만 이 파일에 구현이 없고 scm_blk.c
	 * 의 함수를 곧바로 가리킨다 — 인터럽트 문맥에서 도는 성능 경로라
	 * 결합부를 한 겹 두지 않았다. */
	.handler = scm_blk_irq,
};

/* [한국어]
 * scm_drv_init - 드라이버 표를 EADM 버스에 등록한다
 *
 * @return: 등록 결과. 0 = 성공, 음수 오류.
 *
 * 한 줄짜리 껍데기이며, 아래 드라이버 표를 버스에 건다. 이 순간부터
 * 버스가 맞는 장치를 찾을 때마다 위 probe 가 불린다.
 *
 * **부르는 쪽이 이 파일이 아니다.** scm_blk.c 의 모듈 초기화가 debug
 * 기능과 요청 풀 같은 준비를 마친 뒤 마지막으로 이것을 부른다. 순서가
 * 그래야 하는 이유는 등록하는 즉시 probe 가 들어올 수 있어서, 그때
 * scm_blk.c 쪽 준비가 끝나 있어야 하기 때문이다.
 *
 * __init 표시가 붙어 있어 초기화가 끝나면 이 코드는 메모리에서 버려진다.
 *
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트.
 *
 * 에러 경로: 등록 실패를 그대로 올려보내며, 호출자가 모듈 적재를
 * 실패시킨다.
 *
 * 호출 체인:
 *   scm_blk.c 의 모듈 초기화 → [이 함수] → scm_driver_register()
 */
int __init scm_drv_init(void)
{
	return scm_driver_register(&scm_drv);
}

/* [한국어]
 * scm_drv_cleanup - 드라이버 표의 등록을 해제한다
 *
 * scm_drv_init() 의 짝이며 역시 한 줄이다. 해제하는 과정에서 버스가
 * 붙어 있던 장치마다 위 remove 를 부른다 — 즉 이 한 줄이 모든 SCM 블록
 * 장치의 정리를 이끈다.
 *
 * [상류 코드 관찰] 짝인 init 에는 __init 이 붙어 있는데 **이쪽에는
 * __exit 이 없다.** scm_blk.h 의 선언도 마찬가지다. 모듈 해제 경로에서만
 * 불리므로 붙일 수 있는 자리인데 붙어 있지 않다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 모듈 제거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 이고 해제는 실패하지 않는다.
 *
 * 호출 체인:
 *   scm_blk.c 의 모듈 해제 → [이 함수] → scm_driver_unregister()
 */
void scm_drv_cleanup(void)
{
	scm_driver_unregister(&scm_drv);
}
