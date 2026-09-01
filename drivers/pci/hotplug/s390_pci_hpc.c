// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Hot Plug Controller Driver for System z
 *
 * Copyright 2012 IBM Corp.
 *
 * Author(s):
 *   Jan Glauber <jang@linux.vnet.ibm.com>
 */

/* [한국어] 이 파일에서 나가는 모든 pr_* 메시지 앞에 "zpci: " 를 붙인다.
 * include 보다 먼저 정의해야 printk.h 의 기본 정의를 덮을 수 있다. */
/*
 * [한국어 설명] s390(IBM Z) PCI 핫플러그 컨트롤러 드라이버 (s390_pci_hpc.c)
 *
 * === 파일의 역할 ===
 * IBM Z 시스템의 PCI 기능(function)을 리눅스의 표준 핫플러그 인터페이스로
 * 노출한다. sysfs 의 /sys/bus/pci/slots/<FID>/power 에 1 이나 0 을 쓰면
 * 이 파일의 콜백이 불려 해당 기능이 붙거나 떨어진다.
 * 다른 아키텍처의 핫플러그 드라이버와 결정적으로 다른 점은, 여기서 "전원" 이
 * 물리적 전원이 아니라는 것이다. z 시스템에서 PCI 기능을 붙이고 떼는 주체는
 * 하이퍼바이저이고, 이 드라이버는 SCLP(Service Call Logical Processor)를 통해
 * 그것을 요청할 뿐이다. 그래서 enable 은 sclp_pci_configure() 요청 + 버스
 * 스캔이고, disable 은 그 반대다.
 * 또 하나의 구조적 차이는 슬롯과 기능이 1:1 이라는 점이다. 기능이 없으면
 * 슬롯 자체가 만들어지지 않으므로 "빈 슬롯" 이라는 상태가 존재하지 않고,
 * get_adapter_status() 가 조건 없이 1 을 반환하는 이유가 그것이다.
 * 파일 전체가 141줄, 함수 일곱 개로 작다. 다섯은 hotplug_slot_ops 콜백이고,
 * 나머지 둘(zpci_init_slot / zpci_exit_slot)이 바깥으로 열린 등록·해제
 * 진입점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 s390 PCI 코어와 리눅스 PCI 핫플러그 코어 사이에 놓인 얇은 어댑터다.
 * 아래에는 arch/s390/pci 의 zpci 계층이 있어 SCLP 통신, 기능 상태 기계,
 * 버스 스캔을 담당하고(이 트리는 drivers/{block,nvme,pci,s390,vfio} 로 잘려
 * 있어 arch/s390 는 포함되어 있지 않다), 위에는 drivers/pci/hotplug/pci_hotplug_core.c
 * 가 있어 sysfs 항목을 만들고 콜백을 중계한다.
 * 진입은 두 방향이다. 하나는 아래에서 위로 — s390 PCI 코어가 기능을 발견하면
 * zpci_init_slot() 을 불러 슬롯을 등록하고, 기능이 사라지면 zpci_exit_slot()
 * 으로 해제한다. 다른 하나는 위에서 아래로 — 사용자가 sysfs 를 건드리면
 * 핫플러그 코어가 hotplug_slot_ops 의 다섯 콜백 중 하나를 부르고, 그 안에서
 * 다시 zpci 계층의 함수를 호출한다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 인터럽트나 아토믹 컨텍스트에서
 * 불리는 경로가 없고, SCLP 호출과 버스 스캔은 잠든다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci_hotplug_core.c 의 pci_hp_register() / pci_hp_deregister() 와
 * struct hotplug_slot_ops 규약. 이 파일은 그 규약의 다섯 항목만 채운다 —
 * enable_slot, disable_slot, reset_slot, get_power_status, get_adapter_status.
 * set_attention_status 나 get_latch_status 를 채우지 않는 것은 z 시스템에
 * 대응하는 물리 개념이 없기 때문이다.
 * 아래쪽: sclp_pci_configure()(asm/sclp.h), zpci_scan_configured_device(),
 * zpci_deconfigure_device(), zpci_hot_reset_device(), zpci_is_device_configured(),
 * zpci_dbg()(asm/pci_debug.h). 이들은 arch/s390 쪽 코드이며 이 트리에는 없다.
 * 옆쪽: PCI 코어의 pci_get_slot() / pci_dev_put() / pci_num_vf() —
 * disable_slot() 의 SR-IOV 검사에만 쓴다.
 * 공유 상태: struct zpci_dev 하나다. 이 파일은 그 안의 hotplug_slot 필드를
 * 소유하고(콜백에서 container_of 로 되찾는 앵커다), state 필드를 읽고 쓰며,
 * state_lock 뮤텍스로 그 접근을 직렬화한다. fid / fh / zbus / devfn 은 읽기만
 * 한다. 전역 변수나 static 변수는 하나도 없다.
 * 데이터 흐름: sysfs 쓰기 → 핫플러그 코어 → 이 파일의 콜백 → SCLP 요청 →
 * 하이퍼바이저가 기능을 붙임 → zpci 스캔이 pci_dev 를 만듦 → 일반 PCI 드라이버가
 * 붙는다. 반대 방향도 대칭이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - enable_slot(): STANDBY → SCLP 설정 → CONFIGURED 표시 → 버스 스캔.
 *   순서가 고정되어 있으며, 실패하면 상태를 건드리지 않아 STANDBY 로 남는다.
 * - disable_slot(): CONFIGURED 확인 → SR-IOV VF 가 살아 있으면 -EBUSY →
 *   해제. pci_get_slot() 이 올린 참조를 out: 에서 반드시 놓는다.
 * - reset_slot(): 이 파일에서 유일하게 mutex_trylock 을 쓴다. 락을 못 잡았다는
 *   것은 상태 전이 중이라는 뜻이고 그때는 리셋할 수 없으므로 기다리지 않는다.
 *   probe 인자는 "실제로 하지 말고 가능한지만 답하라" 는 PCI 코어의 탐색 규약이다.
 * - get_power_status(): z 에서 전원 = 설정 여부. 상태를 1/0 으로 옮긴다.
 * - get_adapter_status(): 슬롯이 곧 기능이므로 조건 없이 1. 인자를 쓰지 않는
 *   유일한 콜백이다.
 * - s390_hotplug_slot_ops: 위 다섯 콜백을 묶은 const 표. zpci_init_slot() 에서
 *   zdev->hotplug_slot.ops 에 연결되며, 이후 모든 sysfs 접근의 관문이 된다.
 * - zpci_init_slot() / zpci_exit_slot(): 바깥으로 열린 등록·해제 진입점.
 *   슬롯 이름을 슬롯 번호가 아니라 FID 의 8자리 16진수로 짓는 것이 s390 방식으로,
 *   하이퍼바이저 쪽 도구가 쓰는 식별자와 sysfs 이름을 일치시킨다.
 * - SLOT_NAME_SIZE: 그 이름 버퍼의 크기(10). FID 8자 + 널 종료 + 여유 한 칸.
 */

#define pr_fmt(fmt) "zpci: " fmt

/* [한국어] container_of() 와 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] 이 파일은 직접 할당하지 않지만 하위 헤더 의존으로 남아 있다. */
#include <linux/slab.h>
/* [한국어] pci_get_slot() / pci_dev_put() / pci_num_vf(). */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot, struct hotplug_slot_ops, pci_hp_register/deregister —
 * 이 파일이 구현하는 콜백 규약의 정의처. */
#include <linux/pci_hotplug.h>
/* [한국어] zpci_dbg() 디버그 로그 매크로. */
#include <asm/pci_debug.h>
/* [한국어] SCLP(Service Call Logical Processor) 인터페이스 — sclp_pci_configure().
 * z 시스템에서 하이퍼바이저에 설정 요청을 보내는 통로다. */
#include <asm/sclp.h>

/* [한국어] 슬롯 이름 버퍼 크기. 이름이 FID 를 %08x 로 찍은 8자라 널 종료 포함 9자면
 * 충분하고, 10 은 여유 한 칸이다. */
#define SLOT_NAME_SIZE	10

/* [한국어]
 * enable_slot - 대기 상태의 PCI 기능을 설정하고 버스에 올린다
 *
 * @hotplug_slot: 핫플러그 코어가 넘긴 슬롯. container_of 로 zpci_dev 를 되찾는다.
 * @return: 0 = 성공, -EIO = 대기 상태가 아님, 그 밖에 SCLP/스캔이 낸 오류.
 *
 * sysfs 의 power 에 1 을 쓰면 이 콜백이 불린다.
 *
 * 다른 아키텍처의 핫플러그와 결정적으로 다른 점은, 여기서 하는 일이 물리적인
 * 전원 인가가 아니라는 것이다. z 시스템에서는 SCLP(Service Call Logical
 * Processor)를 통해 하이퍼바이저에 "이 FID 의 PCI 기능을 붙여 달라"고 요청한다.
 * 그 다음에야 버스를 스캔해 pci_dev 를 만든다.
 *
 * 순서가 중요하다. SCLP 설정 성공 → 상태를 CONFIGURED 로 표시 → 스캔.
 * 스캔이 그 상태를 전제로 동작하므로 뒤집을 수 없다.
 *
 * zdev->state_lock 을 잡는 이유는 sysfs 에서 enable 과 disable 이 동시에 들어올
 * 수 있기 때문이다. 락이 없으면 상태 검사와 실제 전이 사이에 다른 전이가
 * 끼어들 수 있다. disable_slot() 과 같은 락을 쓰고, reset_slot() 은 같은 락을
 * trylock 으로만 잡는다.
 *
 * 실행 컨텍스트: sysfs 쓰기, 프로세스 컨텍스트. SCLP 호출과 버스 스캔 모두
 * 잠들 수 있다.
 *
 * 에러 경로: 상태가 맞지 않거나 SCLP 가 실패하면 상태를 건드리지 않고 그대로
 * 빠져나간다. 즉 실패해도 STANDBY 로 남는다.
 *
 * 호출 체인:
 *   sysfs power 쓰기 → PCI 핫플러그 코어 → [이 함수]
 *     → sclp_pci_configure() → zpci_scan_configured_device()
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 콜백이 받은 hotplug_slot 포인터에서 그것을 품고 있는 zpci_dev 를 되찾는다.
	 * PCI 핫플러그 코어는 zpci_dev 를 모르므로, 슬롯 구조체를 장치 구조체 안에
	 * 박아 두고 container_of 로 거슬러 올라가는 것이 이 드라이버의 연결 방식이다. */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	/* [한국어] 반환할 오류 코드. */
	int rc;

	/* [한국어] 상태 전이를 직렬화한다. sysfs 에서 동시에 enable/disable 을 눌러도
	 * 아래 상태 검사와 실제 전이 사이에 다른 전이가 끼어들지 못하게 한다. */
	mutex_lock(&zdev->state_lock);
	/* [한국어] 대기(standby) 상태가 아니면 켤 것이 없다. 이미 configured 이거나 전이
	 * 중이라는 뜻이다. */
	if (zdev->state != ZPCI_FN_STATE_STANDBY) {
		/* [한국어] -EIO 로 거절한다. */
		rc = -EIO;
		/* [한국어] 락을 풀러 간다. */
		goto out;
	}

	/* [한국어] SCLP 로 하이퍼바이저에 이 기능(FID)의 설정을 요청한다. 실제 하드웨어
	 * 전원을 넣는 것이 아니라, z 시스템의 가상화 계층에 PCI 기능을 붙여 달라고
	 * 하는 것이다 — 이것이 s390 핫플러그가 다른 아키텍처와 다른 지점이다. */
	rc = sclp_pci_configure(zdev->fid);
	/* [한국어] 레벨 3 디버그 로그에 FID 와 결과를 남긴다. */
	zpci_dbg(3, "conf fid:%x, rc:%d\n", zdev->fid, rc);
	/* [한국어] SCLP 가 실패하면, */
	if (rc)
		/* [한국어] 상태를 바꾸지 않고 그대로 나간다. */
		goto out;
	/* [한국어] 성공했으니 상태를 configured 로 올린다. 순서가 중요하다 — 아래 스캔이
	 * 이 상태를 전제로 동작한다. */
	zdev->state = ZPCI_FN_STATE_CONFIGURED;

	/* [한국어] 버스를 스캔해 pci_dev 를 만들고 드라이버를 붙인다. 현재 함수 핸들(fh)을
	 * 함께 넘기는 이유는 SCLP 설정으로 핸들이 갱신되었을 수 있기 때문이다. */
	rc = zpci_scan_configured_device(zdev, zdev->fh);
out:
	/* [한국어] 성공이든 실패든 여기서 락을 푼다. */
	mutex_unlock(&zdev->state_lock);
	/* [한국어] 결과를 sysfs 쓰기의 반환값으로 올려보낸다. */
	return rc;
}

/* [한국어]
 * disable_slot - 설정된 PCI 기능을 버스에서 떼고 해제한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘긴 슬롯.
 * @return: 0 = 성공, -EIO = CONFIGURED 상태가 아님, -EBUSY = VF 가 살아 있음.
 *
 * sysfs 의 power 에 0 을 쓰면 불린다. enable_slot() 의 반대 방향이다.
 *
 * VF 검사가 이 함수의 특징이다. pci_get_slot() 으로 pci_dev 를 찾아
 * pci_num_vf() 가 0 이 아니면 -EBUSY 로 거절한다. SR-IOV 물리 기능(PF)을
 * 먼저 없애면 그 아래 가상 기능(VF)들이 부모 없이 남기 때문이다.
 *
 * pci_get_slot() 이 참조 카운트를 올려 반환하므로 out: 라벨에서 반드시
 * pci_dev_put() 해야 한다. 그 자리에 pdev 널 검사가 있는 이유는, 상태 검사에서
 * 빠져나오는 경로가 pdev 를 잡기 전에 out: 으로 뛰기 때문이다.
 *
 * 실행 컨텍스트: sysfs 쓰기, 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 어느 거절 경로든 상태를 바꾸지 않는다. 참조 누수도 없다.
 *
 * 호출 체인:
 *   sysfs power 쓰기 → PCI 핫플러그 코어 → [이 함수]
 *     → pci_get_slot() → pci_num_vf() → zpci_deconfigure_device() → pci_dev_put()
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 켜기 쪽과 같은 방식으로 zpci_dev 를 복원한다. */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	/* [한국어] 이 슬롯에 붙어 있는 pci_dev. 아래 VF 검사에 쓰며, 참조를 잡으므로
	 * 반드시 놓아 주어야 한다. */
	struct pci_dev *pdev = NULL;
	int rc;

	/* [한국어] 켜기와 같은 락으로 전이를 직렬화한다. */
	mutex_lock(&zdev->state_lock);
	/* [한국어] configured 가 아니면 끌 것이 없다. */
	if (zdev->state != ZPCI_FN_STATE_CONFIGURED) {
		rc = -EIO;
		goto out;
	/* [한국어] 거절하고 나간다. */
	}

	/* [한국어] 슬롯 번호(devfn)로 pci_dev 를 찾는다. 참조 카운트를 올려 반환하므로
	 * 아래 out: 에서 반드시 pci_dev_put 해야 한다. */
	pdev = pci_get_slot(zdev->zbus->bus, zdev->devfn);
	/* [한국어] 장치가 있고 그 위에 SR-IOV 가상 기능(VF)이 살아 있으면, */
	if (pdev && pci_num_vf(pdev)) {
		/* [한국어] -EBUSY 로 거절한다. PF 를 먼저 없애면 VF 들이 부모 없이 남기 때문이다. */
		rc = -EBUSY;
		/* [한국어] 거절 경로로. */
		goto out;
	}

	/* [한국어] 실제 해제. 드라이버를 떼고 pci_dev 를 지운 뒤 SCLP 로 하이퍼바이저에
	 * 해제를 알린다. */
	rc = zpci_deconfigure_device(zdev);
out:
	/* [한국어] 59번 줄에서 잡은 참조가 있으면, */
	if (pdev)
		/* [한국어] 놓는다. 이 검사가 필요한 이유는 54번 줄에서 pdev 를 잡기 전에
		 * 빠져나오는 경로가 있기 때문이다 — pdev 는 그때 아직 NULL 이다. */
		pci_dev_put(pdev);
	/* [한국어] 락 해제. */
	mutex_unlock(&zdev->state_lock);
	/* [한국어] 결과 반환. */
	return rc;
}

/* [한국어]
 * reset_slot - 이 기능을 리셋하거나, 리셋 가능 여부만 답한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘긴 슬롯.
 * @probe: true = 실제로 건드리지 말고 지원 여부만 답하라, false = 진짜 리셋하라.
 * @return: 0 = 성공(또는 probe 에서 지원함), -EIO = 상태가 맞지 않거나 전이 중.
 *
 * PCI 코어가 장치 리셋 방법을 찾을 때 두 번 부른다. 먼저 probe=true 로
 * "이 방법을 쓸 수 있나" 를 묻고, 쓸 수 있다면 probe=false 로 실제 리셋한다.
 * 그래서 probe 분기가 상태 검사 뒤, 실제 리셋 앞에 놓여 있다 —
 * 검사는 거치되 하드웨어는 건드리지 않는 자리다.
 *
 * enable/disable 과 달리 mutex_trylock 을 쓰는 것이 이 함수의 핵심이다.
 * 위 영어 주석이 밝히듯, 락을 잡지 못했다는 것은 상태 전이가 진행 중이라는
 * 뜻이고 그때는 어차피 리셋할 수 없으므로 기다릴 이유가 없다. 리셋이 오류 복구
 * 경로에서 불려 블로킹을 피해야 한다는 사정도 있다. rc 초기값이 -EIO 인 것은
 * 이 즉시 반환 경로를 위한 것이다.
 *
 * 실행 컨텍스트: PCI 코어의 리셋 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 락 실패와 상태 불일치 모두 -EIO. 락 실패 경로만 락을 풀지 않고
 * 반환하는데, 애초에 잡지 못했기 때문이다.
 *
 * 호출 체인:
 *   pci_reset_function() 계열 → PCI 핫플러그 코어의 reset_slot → [이 함수]
 *     → mutex_trylock() → zpci_hot_reset_device()
 */
static int reset_slot(struct hotplug_slot *hotplug_slot, bool probe)
{
	/* [한국어] zpci_dev 복원. */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);
	/* [한국어] 기본값을 실패로 둔다. 아래 trylock 실패 경로가 이 값을 그대로 반환한다. */
	int rc = -EIO;

	/*
	 * If we can't get the zdev->state_lock the device state is
	 * currently undergoing a transition and we bail out - just
	 * the same as if the device's state is not configured at all.
	 */
	/* [한국어] 켜기/끄기와 달리 mutex_lock 이 아니라 trylock 이다. 위 영어 주석이 그
	 * 이유를 밝힌다 — 락을 못 잡았다면 상태 전이가 진행 중이라는 뜻이고,
	 * 그것은 리셋할 수 없는 상황이므로 기다리지 말고 바로 거절한다.
	 * 리셋은 오류 복구 경로에서 불려 블로킹을 피해야 하는 것도 이유다. */
	if (!mutex_trylock(&zdev->state_lock))
		/* [한국어] -EIO 로 즉시 반환. 여기서는 락을 잡지 못했으므로 풀지 않는다. */
		return rc;

	/* We can reset only if the function is configured */
	/* [한국어] configured 가 아니면 리셋 대상이 아니다. */
	if (zdev->state != ZPCI_FN_STATE_CONFIGURED)
		goto out;

	/* [한국어] probe 는 "리셋이 가능한지만 물어보는" 호출이다. 실제로 건드리지 않고
	 * 지원 여부만 답한다 — PCI 코어의 리셋 방법 탐색 규약이다. */
	if (probe) {
		/* [한국어] 여기까지 왔다는 것은 configured 상태이므로 지원한다고 답한다. */
		rc = 0;
		/* [한국어] 실제 리셋은 하지 않고 나간다. */
		goto out;
	}

	/* [한국어] 진짜 리셋. 기능을 내렸다 올리는 동안 상태 락을 쥐고 있어
	 * 그 사이 enable/disable 이 끼어들 수 없다. */
	rc = zpci_hot_reset_device(zdev);
out:
	/* [한국어] 락 해제. */
	mutex_unlock(&zdev->state_lock);
	/* [한국어] 결과 반환. */
	return rc;
}

/* [한국어]
 * get_power_status - 이 기능이 설정되어 있는지를 전원 상태로 보고한다
 *
 * @hotplug_slot: 핫플러그 코어가 넘긴 슬롯.
 * @value: 결과를 담을 곳. 1 = 켜짐, 0 = 꺼짐.
 * @return: 언제나 0. 이 콜백은 실패하지 않는다.
 *
 * sysfs 의 power 를 읽을 때 불린다.
 *
 * z 시스템에는 슬롯 전원이라는 물리적 개념이 없다. 대신 하이퍼바이저가 그
 * 기능을 설정해 두었는지가 사용자에게 "전원" 으로 보인다. 그래서 설정 상태를
 * 그대로 1/0 으로 옮긴다 — enable_slot() 이 하는 일이 SCLP 설정인 것과 짝이다.
 *
 * 락을 잡지 않는다. 단일 상태 값을 읽어 보고할 뿐이고, 읽는 순간 이미 낡을 수
 * 있는 값이라 락이 의미가 없다.
 *
 * 실행 컨텍스트: sysfs 읽기, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sysfs power 읽기 → PCI 핫플러그 코어 → [이 함수]
 *     → zpci_is_device_configured()
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] zpci_dev 복원. */
	struct zpci_dev *zdev = container_of(hotplug_slot, struct zpci_dev,
					     hotplug_slot);

	/* [한국어] 이 아키텍처에서 "전원"은 물리 전원이 아니라 기능이 설정되었는지다.
	 * 설정 상태를 1/0 으로 그대로 매핑한다. */
	*value = zpci_is_device_configured(zdev) ? 1 : 0;
	/* [한국어] 이 콜백은 실패하지 않는다. */
	return 0;
}

/* [한국어]
 * get_adapter_status - 슬롯에 어댑터가 있는지 보고한다 (언제나 있다)
 *
 * @hotplug_slot: 핫플러그 코어가 넘긴 슬롯. 실제로는 쓰지 않는다.
 * @value: 결과를 담을 곳. 언제나 1.
 * @return: 언제나 0.
 *
 * sysfs 의 adapter 를 읽을 때 불린다.
 *
 * 위 영어 주석이 답을 다 담고 있다. z 시스템에서는 슬롯 하나가 곧 PCI 기능
 * 하나이고, 기능이 없으면 슬롯도 만들어지지 않는다. 즉 "빈 슬롯" 이라는 상태가
 * 존재하지 않으므로 언제나 1 이다. 인자를 쓰지 않는 유일한 콜백인 것도
 * 그 때문이다.
 *
 * 실행 컨텍스트: sysfs 읽기, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sysfs adapter 읽기 → PCI 핫플러그 코어 → [이 함수]
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 위 영어 주석대로 z 시스템에서는 슬롯이 곧 하나의 PCI 기능이라,
	 * 슬롯이 존재한다는 것 자체가 어댑터가 꽂혀 있다는 뜻이다.
	 * 빈 슬롯이라는 개념이 없으므로 언제나 1 이다. */
	/* if the slot exists it always contains a function */
	/* [한국어] 항상 성공. */
	*value = 1;
	return 0;
}

static const struct hotplug_slot_ops s390_hotplug_slot_ops = {
	/* [한국어] sysfs 의 power 에 1 을 쓰면 불린다. */
	.enable_slot =		enable_slot,
	/* [한국어] power 에 0 을 쓰면 불린다. */
	.disable_slot =		disable_slot,
	/* [한국어] PCI 코어가 이 장치를 리셋할 방법을 찾을 때 불린다. */
	.reset_slot =		reset_slot,
	/* [한국어] sysfs 의 power 를 읽을 때. */
	.get_power_status =	get_power_status,
	/* [한국어] sysfs 의 adapter 를 읽을 때. */
	.get_adapter_status =	get_adapter_status,
};

/* [한국어]
 * zpci_init_slot - zpci 기능 하나에 대응하는 핫플러그 슬롯을 등록한다
 *
 * @zdev: 이 슬롯이 대표할 zpci 기능.
 * @return: pci_hp_register() 의 결과. 0 = 성공.
 *
 * 이 파일의 바깥 진입점이다. 이 파일에는 module_init 도 probe 도 없다 —
 * s390 PCI 코어가 기능을 발견할 때마다 이 함수를 직접 부른다.
 *
 * 하는 일은 셋이다. 콜백 표를 zdev 안의 hotplug_slot 에 연결하고,
 * FID(Function ID)를 8자리 16진수로 찍어 슬롯 이름을 만들고, 코어에 등록한다.
 *
 * 슬롯 번호가 아니라 FID 로 이름을 짓는 것이 s390 방식이다. 사용자가
 * 하이퍼바이저 쪽 도구에서 보는 식별자와 sysfs 의 슬롯 이름이 그대로 맞아
 * 떨어지게 하려는 것이다.
 *
 * 이 함수가 반환하고 나면 sysfs 에 슬롯 디렉터리가 생기고, 위 다섯 콜백이
 * 언제든 불릴 수 있는 상태가 된다.
 *
 * 실행 컨텍스트: s390 PCI 장치 등록 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: pci_hp_register() 의 실패를 그대로 올려보낸다. 그 전까지의 작업은
 * zdev 안의 필드를 채운 것뿐이라 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   s390 PCI 코어의 기능 등록(이 트리에는 arch/s390 가 포함되어 있지 않다)
 *     → [이 함수] → snprintf() → pci_hp_register()
 */
int zpci_init_slot(struct zpci_dev *zdev)
{
	/* [한국어] 슬롯 이름 버퍼. 스택에 두는 이유는 pci_hp_register() 가 이름을 복사해
	 * 가기 때문이다. */
	char name[SLOT_NAME_SIZE];
	/* [한국어] 이 기능이 속한 zpci 버스. */
	struct zpci_bus *zbus = zdev->zbus;

	/* [한국어] 콜백 표를 연결한다. 이후 sysfs 접근이 모두 위 다섯 함수로 들어온다. */
	zdev->hotplug_slot.ops = &s390_hotplug_slot_ops;

	/* [한국어] 슬롯 이름을 FID(Function ID)의 8자리 16진수로 짓는다. 슬롯 번호가 아니라
	 * 기능 식별자를 쓰는 것이 s390 방식으로, 사용자가 하이퍼바이저 쪽에서 보는
	 * 식별자와 그대로 맞춰진다. */
	snprintf(name, SLOT_NAME_SIZE, "%08x", zdev->fid);
	/* [한국어] 핫플러그 코어에 등록한다. 이 호출이 끝나면 sysfs 에 슬롯 디렉터리가
	 * 생기고 위 콜백들이 열린다. */
	return pci_hp_register(&zdev->hotplug_slot, zbus->bus,
			       zdev->devfn, name);
}

/* [한국어]
 * zpci_exit_slot - 등록했던 핫플러그 슬롯을 해제한다
 *
 * @zdev: 슬롯을 품고 있는 zpci 기능.
 *
 * zpci_init_slot() 의 짝이다. 기능이 사라질 때 s390 PCI 코어가 부른다.
 *
 * pci_hp_deregister() 는 sysfs 항목을 없애고, 진행 중인 콜백이 끝날 때까지
 * 기다려 준다. 그래서 이 함수가 반환한 뒤에는 위 다섯 콜백이 더 불리지 않음이
 * 보장되고, zdev 를 해제해도 안전하다.
 *
 * 실행 컨텍스트: s390 PCI 장치 해제 경로, 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   s390 PCI 코어의 기능 해제 → [이 함수] → pci_hp_deregister()
 */
void zpci_exit_slot(struct zpci_dev *zdev)
{
	/* [한국어] 등록을 해제한다. sysfs 항목이 사라지고 진행 중인 콜백이 끝날 때까지
	 * 코어가 기다려 준다. */
	pci_hp_deregister(&zdev->hotplug_slot);
}
