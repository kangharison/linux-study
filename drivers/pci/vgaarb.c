// SPDX-License-Identifier: MIT
/*
 * vgaarb.c: Implements VGA arbitration. For details refer to
 * Documentation/gpu/vgaarbiter.rst
 *
 * (C) Copyright 2005 Benjamin Herrenschmidt <benh@kernel.crashing.org>
 * (C) Copyright 2007 Paulo R. Zanoni <przanoni@gmail.com>
 * (C) Copyright 2007, 2009 Tiago Vignatti <vignatti@freedesktop.org>
 */

/*
 * [한국어 설명] VGA 중재기(VGA arbiter) — 여러 그래픽 카드가 같은 레거시 VGA
 * 고정 자원을 안전하게 나눠 쓰도록 중재하는 코드 (drivers/pci/vgaarb.c)
 *
 * === 파일의 역할 ===
 * PCI 장치의 자원은 보통 BAR(Base Address Register)로 재배치가 가능하지만,
 * "레거시 VGA" 자원만은 예외다. ISA 시절부터 하드와이어된 고정 주소 -- 메모리
 * [0xa0000-0xbffff], I/O [0x3b0-0x3bb], [0x3c0-0x3df] 및 그 별칭(alias) -- 를
 * 그대로 쓴다. (이 범위의 근거는 include/linux/pci.h 의 pci_is_vga() 커널독
 * 주석이고, "하드 디코드된 ISA 주소"라는 설명의 근거는
 * Documentation/gpu/vgaarbiter.rst 이다.) 주소가 고정이므로 그래픽 카드가 두 장
 * 이상 꽂히면 같은 주소에 여러 장치가 동시에 응답할 수 있고, 그 상태에서
 * 레거시 VGA 레지스터를 건드리면 어느 카드가 반응할지 알 수 없다. 이 파일은
 * "한 번에 한 장치만 그 주소에 응답하도록" 만드는 중재 계층이다. 커널 내부
 * 사용자(GPU 드라이버)에게는 vga_get()/vga_put() 을, 유저스페이스에게는
 * /dev/vga_arbiter 문자 장치를 제공한다.
 *
 * 중재 수단은 두 가지 비트다. 첫째, 장치 자신의 PCI Configuration Space
 * COMMAND 레지스터(오프셋 0x04)의 PCI_COMMAND_IO / PCI_COMMAND_MEMORY 비트를
 * 꺼서 그 장치가 아예 응답하지 못하게 만든다. 둘째, 그 장치까지 내려오는 경로에
 * 있는 모든 PCI-to-PCI 브리지의 Bridge Control 레지스터(오프셋 0x3e)에 있는
 * PCI_BRIDGE_CTL_VGA("VGA Enable") 비트를 켜고 꺼서, 레거시 VGA 사이클이 그
 * 브리지 아래로 전달될지를 정한다. 실제 config space 쓰기는 이 파일이 직접 하지
 * 않고 drivers/pci/pci.c 의 pci_set_vga_state() 에 위임한다 -- __vga_tryget() 이
 * 그 함수를 두 번(빼앗을 카드에 false, 가져갈 카드에 true) 부른다.
 *
 * 브리지의 VGA Enable 은 IO 와 MEM 을 구분하지 않는 단 하나의 비트다. 그래서
 * 서로 다른 버스에 있는 두 카드가 다투면 IO 와 MEM 을 따로 줄 수 없고,
 * __vga_tryget() 은 그때 두 자원을 묶어서(lwants 에 IO|MEM 을 모두 세워서)
 * 처리한다. 이 파일의 대부분의 복잡함은 여기서 나온다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 커널 모듈 컨텍스트(프로세스 문맥)에서 도는 PCI 서브시스템의 부속
 * 서비스다. drivers/pci/Makefile 의 `obj-$(CONFIG_VGA_ARB) += vgaarb.o` 로
 * 빌드되며, 진입점 vga_arb_device_init() 은 subsys_initcall_sync() 로 등록된다.
 * 즉 PCI 열거(enumeration)가 끝난 뒤, 대부분의 장치 드라이버가 probe 되기 전에
 * 실행된다. 초기화는 (1) /dev/vga_arbiter misc 장치 등록, (2) pci_bus_type 에
 * 버스 알림자(bus notifier) 등록, (3) 이미 열거된 PCI 장치를 훑어 VGA 클래스인
 * 것을 모두 중재 대상에 추가, 이 세 단계다.
 *
 * 이후 흐름은 세 방향이다. (a) 핫플러그 -- PCI 코어가 장치를 추가/제거할 때마다
 * pci_notify() 가 불려 vga_arbiter_add_pci_device() / _del_ 로 목록을 갱신한다.
 * (b) 커널 클라이언트 -- GPU 드라이버가 vga_client_register() 로 콜백을 등록하고,
 * 레거시 레지스터를 만지기 전에 vga_get() 으로 잠근 뒤 vga_put() 으로 푼다.
 * (c) 유저스페이스 -- X 서버 같은 프로그램이 /dev/vga_arbiter 를 열고 "target",
 * "lock", "trylock", "unlock", "decodes" 명령을 write() 로 보낸다. 이 명령 문법은
 * 아래 "Char driver implementation" 영어 주석과 Documentation/gpu/vgaarbiter.rst
 * 에 동일하게 적혀 있다.
 *
 * 이 계층 위에는 콘솔이 있다. vga_default 로 지정된 "기본 VGA 장치"는 vgacon
 * (VGA 텍스트 콘솔)이 쓰는 카드이며, GPU 드라이버가 자기 하드웨어를 완전히
 * 장악하기 전에 vga_remove_vgacon() 으로 vgacon 을 떼어내는 데도 이 정보가
 * 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 drivers/pci/pci.c 의 pci_set_vga_state() 에 의존한다. 그 함수가
 * PCI_VGA_STATE_CHANGE_DECODES 플래그를 보고 장치의 COMMAND 레지스터를,
 * PCI_VGA_STATE_CHANGE_BRIDGE 플래그를 보고 조상 브리지들의 Bridge Control
 * 레지스터를 고친다. 또 아키텍처 훅 arch_set_vga_state 를 먼저 호출하는데,
 * 이 훅을 실제로 등록하는 곳은 이 트리에서 arch/x86/kernel/apic/x2apic_uv_x.c
 * (uv_set_vga_state) 하나뿐임을 확인했다. 그 밖에 PCI 코어의
 * pci_get_subsys(), pci_get_domain_bus_and_slot(), pci_dev_get()/pci_dev_put(),
 * pci_is_vga() 를 쓴다.
 *
 * 옆으로는 콘솔 계층(include/linux/console.h 의 vga_con, dummy_con,
 * do_take_over_console(), do_unregister_con_driver(), con_is_bound()),
 * 펌웨어 프레임버퍼 정보(include/linux/sysfb.h 의 sysfb_primary_display 와
 * include/linux/screen_info.h 의 screen_info_pci_dev()), ACPI(ACPI_COMPANION,
 * acpi_device_hid, ACPI_VIDEO_HID)에 연결된다.
 *
 * 위로는 include/linux/vgaarb.h 가 공개 API 를 정의한다. 이 트리의 git 객체를
 * 뒤져 확인한 실제 사용자는 drivers/gpu/drm/amd/amdgpu/amdgpu_device.c,
 * drivers/gpu/drm/i915/display/intel_vga.c, drivers/gpu/drm/nouveau/nouveau_vga.c,
 * drivers/gpu/drm/radeon/radeon_device.c, drivers/gpu/drm/qxl/qxl_drv.c,
 * drivers/gpu/drm/virtio/virtgpu_drv.c, drivers/gpu/drm/loongson/lsdc_drv.c,
 * drivers/video/aperture.c 이다. (이 작업 트리는 sparse checkout 이라 그 파일들이
 * 디스크에 없고 git 객체로만 있다.)
 *
 * 공유 상태는 전부 이 파일 안의 파일 스코프 변수다: 장치 목록 vga_list 와 그
 * 카운터 vga_count/vga_decode_count, 기본 장치 포인터 vga_default, 최초 사용
 * 여부 vga_arbiter_used 는 모두 스핀락 vga_lock 이 지키고, 대기자는 대기열
 * vga_wait_queue 에 걸린다. 유저스페이스 인스턴스 목록 vga_user_list 는 별도
 * 스핀락 vga_user_lock 이 지킨다.
 *
 * === NVMe 와의 관계 ===
 * 이 저장소의 학습 목표는 NVMe 이지만, **이 파일은 NVMe 와 직접 관련이 없다.**
 * drivers/nvme 아래 21개 소스에서 주석을 제거한 뒤 vga_get, vga_put,
 * vga_client_register, vga_default_device, pci_set_vga_state, pci_is_vga,
 * VGA_RSRC_LEGACY_IO, PCI_BRIDGE_CTL_VGA, 그리고 그냥 "vga"/"VGA" 토큰까지
 * 전수 검색했으나 등장 횟수는 모두 0 이었다. 억지로 엮지 않는다.
 *
 * 다만 학습 가치는 있다. NVMe 컨트롤러는 BAR0 를 재배치할 수 있는 "현대적"
 * PCIe 장치라 이런 중재가 필요 없지만, 여기서 볼 수 있는 패턴 -- 여러 장치가
 * 재배치 불가능한 고정 자원을 공유할 때 (1) 소유권(owns)/잠금(locks)/디코딩
 * 능력(decodes)을 분리해 추적하고, (2) 자원을 실제로 끄는 대신 게으르게
 * (lazy) 두었다가 다음 요청자가 나타날 때 빼앗고, (3) 잠금에 중첩 카운터를
 * 두어 재진입을 허용하고, (4) 유저스페이스 fd 가 닫힐 때 그 fd 가 잡고 있던
 * 잠금을 자동으로 되돌려 주는 방식 -- 은 큐/태그/컨트롤러 자원을 다루는
 * 스토리지 코드에서도 그대로 되풀이되는 설계다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct vga_device: VGA 클래스 PCI 장치 하나당 하나. decodes(이 카드가
 *   해석할 수 있는 자원), owns(지금 실제로 켜져 있는 자원), locks(지금 잠긴
 *   자원)의 세 비트마스크와 네 개의 중첩 카운터가 핵심이다. vga_list 에 매달린다.
 * - __vga_tryget(): 중재의 심장. 원하는 자원을 이미 소유한 다른 카드를 찾아
 *   pci_set_vga_state(false) 로 끄고, 자기 자신을 pci_set_vga_state(true) 로
 *   켠 뒤 잠금 카운터를 올린다. 충돌 카드를 반환하면 "지금은 못 준다"는 뜻이다.
 * - vga_get(): __vga_tryget() 을 감싸 충돌 시 vga_wait_queue 에서 잠들고 다시
 *   시도하는 블로킹 루프. 인터럽트 문맥에서 부르면 안 된다.
 * - __vga_put(): 카운터를 내리고 0 이 되면 잠금 비트를 지운 뒤 대기자를 깨운다.
 *   자원을 실제로 끄지는 않는다(게으른 해제).
 * - vga_is_boot_device(): 부팅 콘솔을 담당할 "기본 VGA 장치"를 고르는 우선순위
 *   판정. 펌웨어 프레임버퍼 > 레거시 VGA > 내장 GPU > 외장 GPU 순이다.
 * - vga_arb_write(): /dev/vga_arbiter 에 들어온 문자열 명령을 파싱해
 *   vga_get_uninterruptible()/vga_tryget()/vga_put()/__vga_set_legacy_decoding()
 *   으로 연결하는 유저스페이스 진입점.
 * - pci_notify(): PCI 버스 알림자 콜백. VGA 클래스 장치의 추가/제거를 목록에
 *   반영하고 클라이언트에게 알린다.
 */

/* [한국어] 이 파일에서 나가는 모든 printk 앞에 "vgaarb: " 를 자동으로 붙인다.
 * pr_fmt 은 include/linux/printk.h 의 pr_info 계열 매크로가 포맷 문자열을 감쌀
 * 때 참조하는 이름이므로, 반드시 printk.h 를 끌어오는 헤더보다 먼저 정의해야
 * 한다. 그래서 이 정의가 #include 목록보다 위에 있다. */
#define pr_fmt(fmt) "vgaarb: " fmt

/* [한국어] 장치 문맥이 있는 로그용 단축 매크로 3종.
 * pr_fmt 은 pr_ 계열만 접두사를 붙여 주므로, dev_ 계열(장치 이름을 함께 찍는
 * 쪽)에는 "vgaarb: " 를 문자열 리터럴로 직접 이어 붙여야 접두사가 통일된다.
 * `arg...` 는 GNU C 의 이름 붙은 가변 인자이고, `##arg` 는 가변 인자가 하나도
 * 없을 때 앞의 쉼표까지 함께 지워 주는 GCC 확장이다. 이게 없으면
 * vgaarb_dbg(dev, "x\n") 이 dev_dbg(dev, "..." , ) 로 전개돼 컴파일이 깨진다.
 * dbg 는 동적 디버그(CONFIG_DYNAMIC_DEBUG)로 평소엔 꺼져 있고, info/err 는 항상
 * 찍힌다. 이 파일은 중재 상태 변화를 dmesg 로 추적할 수 있게 이 셋을 즐겨 쓴다. */
#define vgaarb_dbg(dev, fmt, arg...)	dev_dbg(dev, "vgaarb: " fmt, ##arg)
#define vgaarb_info(dev, fmt, arg...)	dev_info(dev, "vgaarb: " fmt, ##arg)
#define vgaarb_err(dev, fmt, arg...)	dev_err(dev, "vgaarb: " fmt, ##arg)

/* [한국어] module.h -- 이 파일에는 MODULE_LICENSE 같은 모듈 메타 매크로가 하나도
 * 없다. 그런데도 필요한 이유는 module.h 가 <linux/export.h> 를 포함하기 때문이다
 * (include/linux/module.h 25행에서 확인). vga_default_device / vga_remove_vgacon /
 * vga_get / vga_put / vga_set_legacy_decoding / vga_client_register 를 외부 모듈에
 * 내보내는 EXPORT_SYMBOL, EXPORT_SYMBOL_GPL 이 거기서 온다. */
#include <linux/module.h>
/* [한국어] kernel.h -- 커널 공통 유틸리티 묶음. 이 파일에서는 sprintf/snprintf,
 * 그리고 kernel.h 가 끌어오는 printk/에러 코드 계열 선언에 기대고 있다. 개별
 * 심볼을 좁게 포함하는 대신 관례적으로 붙는 광범위 헤더라, "이 심볼 때문에
 * 반드시 필요하다"고 한 가지로 특정하기는 어렵다. */
#include <linux/kernel.h>
/* [한국어] pci.h -- 이 파일의 본체. struct pci_dev, struct pci_bus,
 * pci_read_config_word(), PCI_COMMAND(=config space 오프셋 0x04)와 그 안의
 * PCI_COMMAND_IO/PCI_COMMAND_MEMORY 비트, PCI_BRIDGE_CONTROL(=오프셋 0x3e)과
 * PCI_BRIDGE_CTL_VGA 비트, pci_set_vga_state()와 그 플래그
 * PCI_VGA_STATE_CHANGE_BRIDGE/_DECODES, pci_is_vga(), pci_dev_get/put(),
 * pci_get_subsys(), pci_get_domain_bus_and_slot(), PCI_DEVFN/PCI_SLOT/PCI_FUNC,
 * pci_bus_type, pci_name() 이 전부 여기서 온다. */
#include <linux/pci.h>
/* [한국어] errno.h -- 이 파일이 돌려주는 음수 오류 코드의 정의.
 * -ENODEV(중재 목록에 없는 장치), -EBUSY(trylock 실패), -EPROTO(유저스페이스가
 * 보낸 명령 문자열이 프로토콜에 없음), -EFAULT(copy_to_user/copy_from_user 실패),
 * -EINVAL, -ENOMEM, -ERESTARTSYS(시그널로 대기 중단), -EIO 를 쓴다. */
#include <linux/errno.h>
/* [한국어] init.h -- 초기화 전용 섹션 표시자. vga_arb_device_init() 에 붙은
 * __init(부팅 후 회수되는 .init.text 섹션에 넣으라는 표시)과, 그 함수를 커널
 * 초기화 순서 중 subsys 단계의 마지막(sync)에 실행하도록 등록하는
 * subsys_initcall_sync() 매크로가 여기 있다. */
#include <linux/init.h>
/* [한국어] list.h -- 커널의 이중 연결 리스트. 중재 대상 장치 목록 vga_list 와
 * 유저스페이스 인스턴스 목록 vga_user_list, 그리고 그 원소인 struct list_head
 * 필드, LIST_HEAD/list_add/list_add_tail/list_del/list_empty/list_for_each_entry
 * 가 여기서 온다. 이 파일은 해시나 트리 없이 선형 탐색만 하는데, 시스템에 꽂힌
 * 그래픽 카드 수가 많아야 몇 개라 그것으로 충분하기 때문이다. */
#include <linux/list.h>
/* [한국어] sched/signal.h -- vga_get() 이 인터럽트 가능(interruptible) 대기를 할 때
 * 시그널이 도착했는지 보는 signal_pending() 과, 현재 태스크를 가리키는 current
 * 매크로를 위해 필요하다. 시그널이 와 있으면 잠들지 않고 -ERESTARTSYS 로 빠져
 * 유저스페이스가 시스템 콜을 재시작하게 한다. */
#include <linux/sched/signal.h>
/* [한국어] wait.h -- 자원 충돌로 못 잠근 태스크가 잠들 대기열.
 * DECLARE_WAIT_QUEUE_HEAD(vga_wait_queue), wait_queue_entry_t,
 * init_waitqueue_entry(), add_wait_queue(), remove_wait_queue(),
 * set_current_state(), wake_up_all() 이 여기서 온다. */
#include <linux/wait.h>
/* [한국어] spinlock.h -- 이 파일의 모든 공유 상태를 지키는 잠금.
 * DEFINE_SPINLOCK 으로 vga_lock 과 vga_user_lock 을 만들고,
 * spin_lock_irqsave/spin_unlock_irqrestore 로 잡는다. 뮤텍스가 아니라 스핀락인
 * 이유는 vga_tryget()/vga_put() 이 "어떤 문맥에서도 호출 가능"해야 하기
 * 때문이다(vga_tryget 위의 영어 주석: "Can be called in any context"). */
#include <linux/spinlock.h>
/* [한국어] poll.h -- /dev/vga_arbiter 의 .poll 구현에 필요하다. __poll_t 타입,
 * poll_table, poll_wait(), 그리고 반환 비트 EPOLLIN 이 여기서 온다. 유저스페이스는
 * 이 fd 를 select/poll 로 감시해 카드 상태 변화 알림을 받는다. */
#include <linux/poll.h>
/* [한국어] miscdevice.h -- /dev/vga_arbiter 를 만드는 방법. struct miscdevice,
 * misc_register(), 그리고 마이너 번호를 커널이 알아서 고르게 하는
 * MISC_DYNAMIC_MINOR 가 여기 있다. 전용 메이저 번호를 받을 만큼 큰 장치가
 * 아니므로 misc 프레임워크(메이저 10)를 빌려 쓴다. */
#include <linux/miscdevice.h>
/* [한국어] slab.h -- 동적 할당. struct vga_device 를 만드는 kzalloc_obj(),
 * struct vga_arb_private 를 만드는 kzalloc_obj(), vga_arb_read() 가 1KB 임시
 * 버퍼를 잡는 kmalloc(), 그리고 kfree() 가 여기서 온다. kzalloc_obj(P, ...) 는
 * include/linux/slab.h 의 매크로로, typeof(P) 크기만큼 0 으로 초기화해 할당한다. */
#include <linux/slab.h>
/* [한국어] sysfb.h -- 펌웨어(BIOS/UEFI)가 부팅 시 켜 둔 프레임버퍼 정보를 담은
 * 전역 sysfb_primary_display 를 위해 필요하다. vga_is_firmware_default() 가
 * 그 안의 .screen(struct screen_info)을 screen_info_pci_dev() 에 넘겨,
 * "펌웨어가 실제로 화면을 띄우고 있던 PCI 장치"를 알아낸다. x86 에서만 쓴다. */
#include <linux/sysfb.h>
/* [한국어] vt.h -- 가상 터미널 관련 정의. 이 파일에서 실제로 쓰는 것은
 * MAX_NR_CONSOLES(=63, include/uapi/linux/vt.h) 하나이며,
 * vga_remove_vgacon() 이 do_take_over_console() 에 콘솔 범위를 넘길 때 쓴다. */
#include <linux/vt.h>
/* [한국어] console.h -- 콘솔 드라이버 교체 API. vga_remove_vgacon() 이 쓰는
 * console_lock()/console_unlock(), con_is_bound(), do_take_over_console(),
 * do_unregister_con_driver() 와, 콘솔 드라이버 객체 vga_con(VGA 텍스트 콘솔) 및
 * dummy_con(아무것도 하지 않는 더미 콘솔)이 여기 선언돼 있다. */
#include <linux/console.h>
/* [한국어] acpi.h -- vga_arb_integrated_gpu() 가 "이 GPU 가 내장(integrated)인가"를
 * 판정할 때, 장치에 붙은 ACPI 노드를 얻는 ACPI_COMPANION() 과 그 하드웨어 ID 를
 * 읽는 acpi_device_hid() 를 쓴다. 비교 대상인 ACPI_VIDEO_HID("LNXVIDEO",
 * include/acpi/acpi_drivers.h)도 acpi.h 가 acpi_drivers.h 를 포함하므로 함께 온다. */
#include <linux/acpi.h>
/* [한국어] uaccess.h -- 유저 공간 포인터와의 안전한 복사. vga_arb_read() 의
 * copy_to_user(), vga_arb_write() 의 copy_from_user() 에 필요하다. 커널이 유저
 * 포인터를 직접 역참조하면 안 되므로(잘못된 주소면 커널이 죽는다) 반드시 이
 * 헬퍼를 거친다. 실패 시 복사하지 못한 바이트 수를 돌려주고, 이 파일은 그것을
 * -EFAULT 로 바꾼다. */
#include <linux/uaccess.h>
/* [한국어] vgaarb.h -- 이 파일이 구현하는 공개 API 의 선언과, 자원 비트 정의.
 * VGA_RSRC_NONE(0x00), VGA_RSRC_LEGACY_IO(0x01), VGA_RSRC_LEGACY_MEM(0x02),
 * VGA_RSRC_LEGACY_MASK(=IO|MEM), VGA_RSRC_NORMAL_IO(0x04),
 * VGA_RSRC_NORMAL_MEM(0x08) 이 여기서 온다. 이 파일에서 vga_get() 을 0 으로
 * 감싼 인라인 vga_get_uninterruptible() 도 그 헤더에 있고, vga_arb_write() 가
 * 그것을 쓴다. */
#include <linux/vgaarb.h>

/* [한국어] 전방 선언. vga_arbiter_notify_clients() 의 정의는 파일 맨 아래쪽에
 * 있는데, 훨씬 위에 있는 vga_check_first_use() 가 그것을 부른다. C 는 위에서
 * 아래로 읽으므로 선언을 미리 해 두지 않으면 컴파일되지 않는다. 이 함수는
 * 등록된 모든 GPU 클라이언트에게 "이제 VGA 중재가 실제로 쓰이기 시작했으니
 * 필요하면 자원을 꺼도 된다"고 알린다. */
static void vga_arbiter_notify_clients(void);

/*
 * We keep a list of all VGA devices in the system to speed
 * up the various operations of the arbiter
 */
/*
 * [한국어] struct vga_device - 중재 대상 VGA 장치 하나의 상태
 *
 * VGA 클래스(pci_is_vga() 가 참) PCI 장치 하나마다 정확히 하나씩 만들어져
 * 전역 목록 vga_list 에 매달린다. 생성은 vga_arbiter_add_pci_device(),
 * 소멸은 vga_arbiter_del_pci_device() 다.
 *
 * 이 구조체의 핵심은 서로 다른 세 가지 개념을 **따로** 추적한다는 점이다.
 *   decodes -- 이 카드가 "해석할 수 있는" 자원. 하드웨어/드라이버의 능력.
 *   owns    -- 이 카드가 "지금 실제로 켜져 있는" 자원. config space 의 현재 상태.
 *   locks   -- 이 카드가 "지금 잠가 둔" 자원. 누군가 vga_get() 으로 예약한 것.
 * 셋은 독립적이다. 예를 들어 해석하지 못하는(decodes 에 없는) 자원도 잠글 수는
 * 있고(__vga_tryget() 의 영어 주석: "We can lock resources that are not
 * decoded"), 그래서 owns 에 decodes 밖의 비트가 설 수 있다.
 *
 * 모든 필드는 전역 스핀락 vga_lock 아래에서만 읽고 쓴다. 예외는 없다 --
 * list 는 물론이고 카운터들도 전부 그렇다.
 */
struct vga_device {
	struct list_head list;
	/* [한국어] 전역 장치 목록 vga_list 에 이 구조체를 매다는 연결 고리.
	 * 설정자: vga_arbiter_add_pci_device() 가 list_add_tail() 로 꼬리에 붙이고,
	 *   vga_arbiter_del_pci_device() 가 list_del() 로 뗀다.
	 * 읽는 자: vgadev_find(), __vga_tryget() 의 충돌 탐색 루프,
	 *   vga_arbiter_check_bridge_sharing(), vga_arbiter_notify_clients() 가
	 *   list_for_each_entry() 로 순회한다.
	 * 값 범위: 목록에 들어 있는 동안 유효한 이중 연결 리스트 노드.
	 * 동기화: vga_lock 스핀락(irqsave)으로 보호. 목록 순회도 전부 그 락 안에서만
	 *   일어나므로 RCU 같은 무잠금 순회는 쓰지 않는다. */

	struct pci_dev *pdev;
	/* [한국어] 이 항목이 대표하는 PCI 장치. 이 구조체와 하드웨어를 잇는 유일한 끈.
	 * 설정자: vga_arbiter_add_pci_device() 가 알림자에서 받은 pdev 를 그대로 저장.
	 * 읽는 자: vgadev_find() 의 비교 키, pci_set_vga_state() 에 넘길 대상,
	 *   pci_read_config_word() 로 COMMAND 레지스터를 읽을 대상, vgaarb_dbg 계열
	 *   로그가 찍을 장치 이름(&pdev->dev), 그리고 pdev->bus 로 브리지 계보를
	 *   거슬러 올라갈 출발점이다.
	 * 값 범위: 유효한 struct pci_dev 포인터. NULL 이 되는 경우는 없다.
	 * 동기화: 이 파일은 여기에 별도 참조 카운트를 잡지 않는다. 대신 PCI 버스
	 *   알림자(BUS_NOTIFY_DEL_DEVICE)가 장치 제거를 알려 줄 때 항목을 함께
	 *   없애는 것으로 수명을 맞춘다. (참조 카운트를 잡는 곳은 vga_default 와
	 *   유저스페이스 target 쪽이며, 그 둘은 pci_dev_get/put 을 쓴다.) */

	unsigned int decodes;		/* what it decodes */
	/* [한국어] 이 카드가 해석(decode)할 수 있다고 알려진 자원의 비트마스크.
	 * 설정자: vga_arbiter_add_pci_device() 가 처음에 네 비트를 모두 세운다
	 *   (LEGACY_IO|LEGACY_MEM|NORMAL_IO|NORMAL_MEM -- "일단 다 해석한다고 가정").
	 *   이후에는 vga_update_device_decodes() 만이 바꾼다. 그 함수를 부르는 길은
	 *   두 갈래로, 드라이버가 vga_set_legacy_decoding() 을 부르거나
	 *   유저스페이스가 "decodes" 명령을 쓰는 경로, 그리고
	 *   vga_arbiter_notify_clients() 가 set_decode 콜백의 반환값을 반영하는 경로다.
	 * 읽는 자: __vga_tryget() 이 "NORMAL 자원 요청을 LEGACY 요청으로 승격할지"를
	 *   판단할 때, __vga_put() 이 같은 승격을 되돌릴 때, __vga_tryget() 이 충돌
	 *   카드의 COMMAND 비트를 꺼야 하는지 볼 때 쓴다.
	 * 값 범위: VGA_RSRC_LEGACY_IO(0x01) | VGA_RSRC_LEGACY_MEM(0x02) |
	 *   VGA_RSRC_NORMAL_IO(0x04) | VGA_RSRC_NORMAL_MEM(0x08) 의 조합.
	 * 동기화: vga_lock 아래에서만. vga_update_device_decodes() 위의 영어 주석
	 *   "Called with the lock" 이 그 계약을 명시한다. */

	unsigned int owns;		/* what it owns */
	/* [한국어] 지금 이 카드에서 실제로 "켜져 있는" 자원의 비트마스크. 하드웨어의
	 *   현재 상태(COMMAND 비트와 경로상 브리지의 VGA Enable)를 소프트웨어가
	 *   그림자로 들고 있는 값이다.
	 * 설정자: vga_arbiter_add_pci_device() 가 초기값을 COMMAND 레지스터에서 읽어
	 *   만들고(IO 켜져 있으면 LEGACY_IO, MEM 켜져 있으면 LEGACY_MEM), 경로에
	 *   VGA Enable 이 꺼진 브리지가 하나라도 있으면 0 으로 지운다.
	 *   __vga_tryget() 이 자원을 빼앗을 때 충돌 카드의 owns 에서 비트를 지우고,
	 *   자기 자신의 owns 에 wants 를 더한다.
	 * 읽는 자: __vga_tryget() 이 "무엇을 더 얻어야 하는가"(wants = rsrc & ~owns)를
	 *   계산할 때, vga_is_boot_device() 가 "레거시 VGA 장치인가"를 판정할 때
	 *   ((owns & VGA_RSRC_LEGACY_MASK) == VGA_RSRC_LEGACY_MASK), vga_arb_read() 가
	 *   유저스페이스에 상태 문자열을 만들어 줄 때 읽는다.
	 * 값 범위: decodes 와 같은 네 비트의 조합. decodes 의 부분집합일 필요는 없다
	 *   -- 해석하지 못하는 자원도 소유(잠금)할 수 있기 때문이다.
	 * 동기화: vga_lock. */

	unsigned int locks;		/* what it locks */
	/* [한국어] 지금 이 카드에 걸려 있는 레거시 자원 잠금의 비트마스크.
	 *   owns 와 달리 "하드웨어 상태"가 아니라 "예약 상태"다. 잠겨 있으면 다른
	 *   카드가 그 자원을 빼앗아 갈 수 없다.
	 * 설정자: __vga_tryget() 의 lock_them 레이블이 `locks |= rsrc &
	 *   VGA_RSRC_LEGACY_MASK` 로 세우고, __vga_put() 이 대응 카운터가 0 이 될 때
	 *   해당 비트를 지운다. vga_update_device_decodes() 도 해석 능력이 사라진
	 *   자원의 잠금을 강제로 해제한다.
	 * 읽는 자: __vga_tryget() 의 충돌 탐색이 `conflict->locks & lwants` 로
	 *   "빼앗을 수 있는가"를 판정하는 데 쓴다. 잠겨 있으면 빼앗지 못하고 그
	 *   카드를 충돌자로 반환한다. vga_arb_read() 도 상태 표시에 쓴다.
	 * 값 범위: VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM 조합만. NORMAL 비트는
	 *   여기 들어오지 않는다(마스크로 걸러진다).
	 * 동기화: vga_lock. 이 값이 바뀌면 __vga_put() 이 vga_wait_queue 를 깨운다. */

	unsigned int io_lock_cnt;	/* legacy IO lock count */
	/* [한국어] 레거시 IO 잠금의 중첩(nesting) 횟수.
	 * 설정자: __vga_tryget() 이 VGA_RSRC_LEGACY_IO 요청마다 ++,
	 *   __vga_put() 이 0 보다 클 때만 --, vga_update_device_decodes() 는 해석
	 *   능력이 사라졌을 때 0 으로 강제 초기화한다.
	 * 읽는 자: __vga_put() 이 이 값이 0 이 되는 순간에만 locks 의 IO 비트를 지운다.
	 *   vga_arb_read() 가 진단용으로 유저스페이스에 노출한다.
	 * 값 범위: 0 이상. vga_get() 의 영어 주석이 말하는 "Nested calls are
	 *   supported (a per-resource counter is maintained)" 가 바로 이 카운터다.
	 * 동기화: vga_lock. 원자적 연산이 아니라 그냥 정수인 이유는 접근이 전부
	 *   그 스핀락 안에서 일어나기 때문이다. */

	unsigned int mem_lock_cnt;	/* legacy MEM lock count */
	/* [한국어] 레거시 MEM 잠금의 중첩 횟수. io_lock_cnt 의 메모리 쪽 짝.
	 * 설정자: __vga_tryget() 이 VGA_RSRC_LEGACY_MEM 요청마다 ++,
	 *   __vga_put() 이 0 보다 클 때만 --, vga_update_device_decodes() 가 0 으로 초기화.
	 * 읽는 자: __vga_put() 이 0 이 되는 순간 locks 의 MEM 비트를 지운다.
	 *   vga_arb_read() 가 진단용으로 노출한다.
	 * 값 범위: 0 이상.
	 * 동기화: vga_lock. */

	unsigned int io_norm_cnt;	/* normal IO count */
	/* [한국어] "정상"(비레거시) IO 자원 요청의 중첩 횟수. 카드가 자기 BAR 로
	 *   재배치한 IO 영역을 쓰겠다는 요청이다.
	 * 설정자: __vga_tryget() 이 VGA_RSRC_NORMAL_IO 요청마다 ++,
	 *   __vga_put() 이 0 보다 클 때만 --.
	 * 읽는 자: __vga_put() 만 읽는다. 이 카운터가 0 보다 커야 감소하고, 그때만
	 *   대응하는 레거시 IO 요청도 함께 반납한 것으로 친다.
	 * 값 범위: 0 이상. locks 비트마스크에는 반영되지 않는다(레거시만 잠근다).
	 * 동기화: vga_lock. */

	unsigned int mem_norm_cnt;	/* normal MEM count */
	/* [한국어] "정상"(비레거시) MEM 자원 요청의 중첩 횟수. io_norm_cnt 의 메모리 쪽 짝.
	 * 설정자: __vga_tryget() 이 VGA_RSRC_NORMAL_MEM 요청마다 ++,
	 *   __vga_put() 이 0 보다 클 때만 --.
	 * 읽는 자: __vga_put() 만.
	 * 값 범위: 0 이상.
	 * 동기화: vga_lock. */

	bool bridge_has_one_vga;
	/* [한국어] "이 장치의 바로 위 브리지 하나만으로 이 장치의 VGA 라우팅을
	 *   제어할 수 있는가". 참이면 그 브리지의 VGA Enable 비트만 껐다 켜면 되므로
	 *   장치의 COMMAND 레지스터는 건드릴 필요가 없다.
	 * 설정자: vga_arbiter_check_bridge_sharing() 이 장치를 등록할 때 계산한다.
	 *   그 함수는 새 장치와 기존 장치의 브리지 계보를 교차 검사해, 한 브리지
	 *   아래에 VGA 장치가 둘 이상 매달리면 **양쪽 모두** 거짓으로 내린다.
	 * 읽는 자: __vga_tryget() 이 두 곳에서 본다. 충돌 카드를 끌 때 거짓이면
	 *   PCI_VGA_STATE_CHANGE_DECODES 를 붙여 COMMAND 비트까지 끄고, 자기를 켤 때
	 *   거짓이면 마찬가지로 COMMAND 비트까지 켠다.
	 * 값 범위: true(브리지 제어 가능) / false(불가능). dmesg 에 "bridge control
	 *   possible" / "no bridge control possible" 로 찍힌다.
	 * 동기화: vga_lock. 계산 자체가 vga_arbiter_add_pci_device() 의 락 구간
	 *   안에서 일어난다. */

	bool is_firmware_default;	/* device selected by firmware */
	/* [한국어] "펌웨어(BIOS/UEFI)가 부팅 화면을 띄우던 바로 그 장치인가".
	 *   기본 VGA 장치 선정에서 가장 높은 우선순위를 갖는다.
	 * 설정자: vga_is_boot_device() 가 vga_is_firmware_default() 로 확인해 참이면
	 *   여기에 true 를 기록한다. 그 외에는 kzalloc_obj 로 0 초기화된 false 그대로다.
	 * 읽는 자: vga_is_boot_device() 자신이 다음 장치를 심사할 때 "이미 펌웨어
	 *   기본 장치를 찾았으면 더 볼 것 없다"고 조기 반환하는 데 쓴다.
	 * 값 범위: true / false. x86 이 아니면 항상 false 다
	 *   (vga_is_firmware_default() 가 #if defined CONFIG_X86 로 갈린다).
	 * 동기화: vga_lock. */

	unsigned int (*set_decode)(struct pci_dev *pdev, bool decode);
	/* [한국어] GPU 드라이버가 등록한 "디코딩 상태를 바꿔라" 콜백.
	 *   중재기가 이 카드에게 레거시 자원 해석을 켜라/꺼라 요청하고, 드라이버는
	 *   실제로 적용된 결과를 decodes 비트마스크로 되돌려 준다.
	 * 설정자: vga_client_register() 가 vga_lock 아래에서 대입한다. NULL 을 넘기면
	 *   등록 해제이며, vgaarb.h 의 인라인 vga_client_unregister() 가 그렇게 부른다.
	 * 읽는 자: vga_arbiter_notify_clients() 가 등록된 모든 카드에 대해 호출하고
	 *   반환값을 vga_update_device_decodes() 로 반영한다.
	 *   __vga_set_legacy_decoding() 도 이 필드가 NULL 이 아니면 "커널 드라이버가
	 *   관리 중"으로 보고 유저스페이스의 decodes 명령을 무시한다.
	 * 값 범위: NULL(등록 안 됨) 또는 드라이버 함수 포인터. 이 트리의 git 객체로
	 *   확인한 실제 등록자는 amdgpu, i915, nouveau, radeon, qxl, virtio-gpu,
	 *   loongson lsdc, 그리고 drivers/video/aperture.c 다.
	 * 동기화: 필드 자체의 읽기/쓰기는 vga_lock 아래. 주의할 점은 콜백이 **스핀락을
	 *   잡은 채** 불린다는 것이다(vga_arbiter_notify_clients() 참조). 따라서
	 *   드라이버 구현은 잠들 수 없다. */
};

/* [한국어] 중재 대상 VGA 장치 전체 목록의 머리. 시스템의 모든 VGA 클래스 PCI
 * 장치가 여기에 매달린다. LIST_HEAD 는 이름만 있는 빈 리스트를 컴파일 시점에
 * 만들어 주는 매크로다.
 * 설정자/읽는 자: 이 파일의 거의 모든 함수. 반드시 vga_lock 아래에서 다룬다. */
static LIST_HEAD(vga_list);
/* [한국어] vga_count -- vga_list 에 들어 있는 장치 수. 등록/해제 시 증감하며,
 * vga_arbiter_notify_clients() 가 "카드가 한 장뿐이면(new_state=true) 굳이
 * 디코딩을 끌 필요가 없다"고 판단하는 유일한 근거다.
 * vga_decode_count -- 그중 레거시 자원을 해석하는 장치 수. 등록 시 무조건 1
 * 늘리고(초기 decodes 가 레거시를 포함하므로), decodes 가 레거시를 잃거나 얻을
 * 때 vga_update_device_decodes() 가 조정한다. vga_arb_read() 가 유저스페이스에
 * "count:" 로 노출한다.
 * 동기화: 둘 다 vga_lock 아래에서만 증감한다. */
static int vga_count, vga_decode_count;
/* [한국어] "VGA 중재가 한 번이라도 실제로 쓰였는가"를 기억하는 한 번만 켜지는
 * 플래그. 부팅 직후에는 거짓이고, 처음으로 vga_get()/vga_tryget() 이 불릴 때
 * vga_check_first_use() 가 참으로 바꾸면서 클라이언트들에게 알린다.
 * 왜 필요한가: vga_check_first_use() 위의 영어 주석이 이유를 밝힌다 -- 중재를 쓰지도 않는데
 * 미리 자원을 꺼 버리면, 부팅 장치가 VGA 가 아니라고 착각한 옛 X 서버들이
 * 오동작한다. 그래서 "누군가 실제로 중재를 요구할 때까지는 아무것도 끄지
 * 않는다"는 게으른 정책을 쓴다.
 * 읽는 자: vga_check_first_use() 와 vga_arbiter_notify_clients().
 * 동기화: bool 한 개이고 켜지는 방향으로만 바뀌므로 락 없이 읽힌다. 켤 때는
 * vga_check_first_use() 가 vga_lock 을 잡지 않은 상태에서 바꾸는데, 최악의
 * 경우 알림이 두 번 갈 뿐 상태가 깨지지는 않는다. */
static bool vga_arbiter_used;
/* [한국어] 이 파일의 중재 상태 전체(vga_list, 각 vga_device 의 모든 필드,
 * vga_count, vga_decode_count, vga_default)를 지키는 전역 스핀락.
 * 왜 뮤텍스가 아닌가: vga_tryget() 과 vga_put() 은 "어떤 문맥에서도" 불릴 수
 * 있어야 해서 잠들 수 없다. 그래서 spin_lock_irqsave 로 지역 인터럽트까지 막고
 * 짧게 잡는다. 다만 그 안에서 pci_set_vga_state() 가 config space 접근을 하므로
 * 임계 구역이 아주 짧다고는 할 수 없다.
 * 주의: vga_get() 은 대기 전에 반드시 이 락을 놓는다 -- 스핀락을 쥔 채
 * schedule() 을 부르면 데드락이다. */
static DEFINE_SPINLOCK(vga_lock);
/* [한국어] 자원 충돌로 잠금을 얻지 못한 태스크들이 잠들어 기다리는 대기열.
 * 깨우는 쪽: __vga_put() 이 잠금 비트가 실제로 바뀌었을 때, 그리고
 * vga_arbiter_del_pci_device() 가 장치를 제거했을 때 wake_up_all() 을 부른다.
 * 기다리는 쪽: vga_get() 의 재시도 루프. /dev/vga_arbiter 의 .poll 도 이 대기열에
 * poll_wait() 로 걸려, 유저스페이스가 상태 변화를 감지할 수 있게 한다.
 * 설계 메모: 장치별 대기열이 아니라 전역 하나뿐이라, 관계없는 카드가 자원을
 * 놓아도 모든 대기자가 깨어나 다시 시도한다(thundering herd). vga_get() 안의
 * 영어 주석이 이 점을 인정하며 장치별 대기열로 나눌 수 있다고 적어 두었다. */
static DECLARE_WAIT_QUEUE_HEAD(vga_wait_queue);

/*
 * [한국어]
 * vga_iostate_to_str - 자원 비트마스크를 사람이 읽을 문자열로 바꾼다
 *
 * @iostate: VGA_RSRC_* 비트들의 조합. 보통 vga_device 의 decodes / owns / locks
 *   중 하나를 그대로 넘긴다. NORMAL 비트가 섞여 있어도 된다 -- 여기서 걸러진다.
 * @return: "io+mem", "io", "mem", "none" 넷 중 하나를 가리키는 정적 문자열
 *   포인터. 호출자는 이것을 printf 계열의 %s 에 그대로 넘기며, 해제할 필요가 없다.
 *
 * 왜 필요한가: 중재 상태는 비트마스크라 dmesg 나 /dev/vga_arbiter 로 그대로
 * 내보내면 사람이 못 읽는다. 이 함수가 유일한 표기 규약을 만들어, 커널 로그와
 * 유저스페이스 ABI 양쪽이 같은 어휘를 쓰게 한다. 유저스페이스 쪽 역변환은
 * vga_str_to_iostate() 이며, 둘의 어휘 집합이 정확히 같아야 한다.
 *
 * 실행 컨텍스트: 순수 함수라 어떤 문맥에서도 안전하다. 실제로는 호출자
 * 대부분이 vga_lock 을 쥔 상태에서 부른다(vga_arb_read(),
 * vga_arbiter_add_pci_device(), vga_update_device_decodes()). 인자를 값으로
 * 받으므로 락 안에서 불러도 문제될 것이 없다.
 *
 * 에러 경로: 없다. 어떤 입력이 와도 위 네 문자열 중 하나로 떨어진다.
 *
 * 호출 체인:
 *   vga_arb_read() / vga_arbiter_add_pci_device() / vga_update_device_decodes()
 *     -> [vga_iostate_to_str]
 */
static const char *vga_iostate_to_str(unsigned int iostate)
{
	/* [한국어] 레거시 두 비트만 남기고 NORMAL 비트는 버린다. 이 문자열 표기는
	 * "레거시 자원을 어떻게 다루고 있는가"만 표현하기 때문이다.
	 * 참고: 바로 위 상류 영어 주석은 VGA_RSRC_IO / VGA_RSRC_MEM 이라는 이름을
	 * 들고 있는데, include/linux/vgaarb.h 를 전수 확인한 결과 그런 이름의 매크로는
	 * 존재하지 않는다. 실제로 걸러지는 것은 VGA_RSRC_NORMAL_IO(0x04) 와
	 * VGA_RSRC_NORMAL_MEM(0x08) 이다. 상류 주석은 손대지 않고 그대로 둔다. */
	/* Ignore VGA_RSRC_IO and VGA_RSRC_MEM */
	iostate &= VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
	/* [한국어] 남은 두 비트의 조합은 네 가지뿐이라 switch 로 전부 나열한다. */
	switch (iostate) {
	/* [한국어] 두 비트가 모두 선 경우 -- IO 와 MEM 을 함께 다루고 있다. */
	case VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM:
		return "io+mem";
	/* [한국어] IO 만 선 경우. */
	case VGA_RSRC_LEGACY_IO:
		return "io";
	/* [한국어] MEM 만 선 경우. */
	case VGA_RSRC_LEGACY_MEM:
		return "mem";
	}
	/* [한국어] 남은 유일한 경우는 두 비트 모두 0 -- 레거시 자원과 무관한 상태. */
	return "none";
}

/*
 * [한국어]
 * vga_str_to_iostate - 유저스페이스가 보낸 자원 이름 문자열을 비트마스크로 바꾼다
 *
 * @buf: /dev/vga_arbiter 에 write() 된 명령에서 자원 이름이 시작하는 위치.
 *   vga_arb_write() 가 "lock ", "unlock ", "trylock ", "decodes " 접두사를 이미
 *   건너뛴 뒤의 포인터를 넘긴다. 커널 스택의 kbuf 안이며 널 종료가 보장돼 있다.
 * @str_size: 남은 바이트 수. **현재 구현은 이 값을 전혀 쓰지 않는다** -- 바로
 *   아래 상류 주석 "XXX We're not checking the str_size!" 가 그 사실을 인정한다.
 *   안전한 이유는 호출자가 kbuf[count] = 0 으로 널 종료를 보장하기 때문이다.
 * @io_state: 결과를 받아 갈 출력 파라미터. 성공했을 때만 값이 쓰인다.
 * @return: 1 = 아는 문자열이라 해석에 성공, 0 = 모르는 문자열. 호출자는 0 이면
 *   -EPROTO 로 write() 를 거절한다.
 *
 * 왜 필요한가: 유저스페이스 ABI 는 문자열 기반이라(Documentation/gpu/vgaarbiter.rst
 * 의 "io_state is of the form {io,mem,io+mem,none}") 커널이 문자열을 파싱해야
 * 한다. vga_iostate_to_str() 의 역방향이며, 그 함수가 만들어 낸 문자열을 그대로
 * 되돌려 넣을 수 있어야 한다.
 *
 * 중요한 설계 결정: "io" 만 요청하든 "mem" 만 요청하든 결과는 언제나 IO 와 MEM
 * 둘 다다(모두 both 레이블로 모인다). 함수 첫머리의 상류 영어 주석이 이유를
 * 밝힌다 -- 유저스페이스에 IO 와 MEM 을 따로 내주면 두 프로세스가 서로 엇갈려
 * 하나씩 쥔 채 상대를 기다리는 데드락이 생길 수 있다. 그래서 유저스페이스에는
 * 항상 묶어서만 준다. 커널 내부 클라이언트(vga_get)는 이런 제약이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(write 시스템 콜). 락을 잡지 않으며 잡을 필요도
 * 없다 -- 스택 버퍼와 출력 파라미터만 만진다.
 *
 * 에러 경로: 아는 접두사가 없으면 0 을 돌려주고 *io_state 는 손대지 않는다.
 *
 * 호출 체인:
 *   vga_arb_write() -> [vga_str_to_iostate] -> strncmp()
 */
static int vga_str_to_iostate(char *buf, int str_size, unsigned int *io_state)
{
	/*
	 * In theory, we could hand out locks on IO and MEM separately to
	 * userspace, but this can cause deadlocks.
	 */
	/* [한국어] "none" 은 특별 취급이다. 아무 자원도 가리키지 않는 유일한 값이라
	 * 아래의 "무조건 IO|MEM" 규칙에서 빠져야 한다. 4 는 "none" 의 길이.
	 * 호출자 쪽에서 lock 명령에 대해서는 VGA_RSRC_NONE 을 다시 -EPROTO 로
	 * 거절한다("none" 을 잠그라는 요청은 무의미하므로). */
	if (strncmp(buf, "none", 4) == 0) {
		/* [한국어] 0x00. 어떤 레거시 자원도 지정하지 않았음을 뜻한다. */
		*io_state = VGA_RSRC_NONE;
		/* [한국어] 1 = 해석 성공. 문자열을 알아봤다는 뜻이지, 요청이 유효하다는
		 * 뜻은 아니다. 유효성 판단은 호출자 몫이다. */
		return 1;
	}

	/* XXX We're not checking the str_size! */
	/* [한국어] "io+mem" 을 가장 먼저 비교한다. 길이 6 으로 앞에서 걸러야
	 * 뒤의 "io"(길이 2) 비교에 먹히지 않는다 -- 어차피 결과가 같아 순서가
	 * 결과를 바꾸지는 않지만, 의도를 드러내는 순서다. */
	if (strncmp(buf, "io+mem", 6) == 0)
		goto both;
	/* [한국어] "io" 만 요청. 그래도 both 로 간다(위 데드락 회피 정책). */
	else if (strncmp(buf, "io", 2) == 0)
		goto both;
	/* [한국어] "mem" 만 요청. 역시 both 로 간다. */
	else if (strncmp(buf, "mem", 3) == 0)
		goto both;
	/* [한국어] 넷 중 어느 것도 아니면 프로토콜 위반. 0 을 돌려주면 호출자가
	 * -EPROTO 로 write() 를 실패시킨다. */
	return 0;
both:
	/* [한국어] 어떤 이름으로 요청했든 레거시 IO 와 MEM 을 함께 지정한다.
	 * 0x01 | 0x02 = 0x03. 이것이 유저스페이스에 내주는 유일한 잠금 단위다. */
	*io_state = VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
	/* [한국어] 해석 성공. */
	return 1;
}

/* This is only used as a cookie, it should not be dereferenced */
/* [한국어] 시스템의 "기본 VGA 장치" -- 부팅 시 콘솔(vgacon)이 쓰는 카드.
 * 상류 영어 주석이 "쿠키로만 쓰라, 역참조하지 말라"고 못박은 이유는, 이 값이
 * vgadev_find() 의 비교 키와 vga_arb_open() 의 초기 target 같은 **동일성 판정**
 * 용도로만 쓰이기 때문이다. 다만 참조 카운트는 제대로 관리한다 --
 * vga_set_default_device() 가 pci_dev_get() 으로 올리고 이전 값에는
 * pci_dev_put() 을 걸어, 이 포인터가 가리키는 pci_dev 가 사라지지 않게 한다.
 * 설정자: vga_set_default_device() 하나뿐. 그것을 부르는 곳은
 *   vga_arbiter_add_pci_device()(vga_is_boot_device() 가 참일 때)와
 *   vga_arbiter_del_pci_device()(기본 장치가 뽑히면 NULL 로), 그리고 외부
 *   드라이버(EXPORT 되어 있지 않으므로 커널 내장 코드만).
 * 읽는 자: vga_default_device() 를 통해 간접적으로, 그리고
 *   vga_remove_vgacon()/vga_arbiter_del_pci_device() 가 직접 비교한다.
 * 값 범위: NULL(아직 못 골랐거나 뽑힘) 또는 유효한 pci_dev 포인터.
 * 동기화: 갱신 경로가 vga_lock 안(add/del)과 밖(외부 호출) 모두에 있어
 *   이 파일만으로는 완전한 락 규약을 확정할 수 없다. 확인할 수 있는 사실은
 *   vga_set_default_device() 자체가 어떤 락도 잡지 않는다는 것뿐이다. */
static struct pci_dev *vga_default;

/*
 * [한국어]
 * vgadev_find - PCI 장치에 대응하는 중재 항목을 목록에서 찾는다
 *
 * @pdev: 찾고자 하는 PCI 장치. NULL 이 들어와도 안전하다 -- 목록의 pdev 는
 *   모두 NULL 이 아니므로 그냥 못 찾은 것이 된다. 실제로 vga_is_boot_device() 가
 *   vga_default_device() 의 반환값(NULL 일 수 있다)을 그대로 넘긴다.
 * @return: 찾으면 struct vga_device 포인터, 없으면 NULL. 호출자는 대부분 NULL 을
 *   -ENODEV 로 바꾸거나 조용히 빠져나간다.
 *
 * 왜 필요한가: 이 파일의 거의 모든 진입점은 "PCI 장치"를 받아 "중재 상태"를
 * 다뤄야 하므로, 둘을 잇는 변환이 한 곳에 필요하다. 자료구조가 선형 리스트인
 * 이유는 시스템의 VGA 카드 수가 많아야 몇 개이기 때문이다 -- 해시를 둘 만한
 * 규모가 아니다.
 *
 * 실행 컨텍스트: **호출자가 반드시 vga_lock 을 쥐고 있어야 한다.** 이 함수는
 * 락을 잡지 않으며, 목록을 순회하는 동안 다른 CPU 가 list_del() 을 하면
 * 포인터가 깨진다. 실제 호출자들(vga_get, vga_tryget, vga_put,
 * __vga_set_legacy_decoding, vga_client_register, vga_arb_read,
 * vga_arbiter_add/del_pci_device)은 모두 spin_lock_irqsave(&vga_lock) 안에서
 * 부른다. 예외는 vga_arb_write() 의 "target" 처리인데, 이 트리의 코드만으로는
 * 그 지점에서 vga_lock 이 잡혀 있지 않음이 확인된다(아래 해당 위치 주석 참고).
 *
 * 에러 경로: 없음. 못 찾으면 NULL 을 돌려줄 뿐이다.
 *
 * 호출 체인:
 *   vga_get() / vga_tryget() / vga_put() / vga_arb_read() / ...
 *     -> [vgadev_find] -> list_for_each_entry()
 */
/* Find somebody in our list */
static struct vga_device *vgadev_find(struct pci_dev *pdev)
{
	/* [한국어] 순회용 커서. list_for_each_entry 가 매 반복마다 채워 준다. */
	struct vga_device *vgadev;

	/* [한국어] vga_list 의 각 노드에서 struct vga_device 의 list 필드를 기준으로
	 * container_of 를 적용해 바깥 구조체를 복원하며 훑는다. */
	list_for_each_entry(vgadev, &vga_list, list)
		/* [한국어] pci_dev 포인터의 동일성으로 판정한다. 도메인/버스/devfn 을
		 * 따로 비교할 필요가 없는 이유는, PCI 코어가 장치 하나당 pci_dev 를
		 * 하나만 만들기 때문이다. */
		if (pdev == vgadev->pdev)
			/* [한국어] 찾았다. 반환 포인터는 vga_lock 을 놓는 순간부터
			 * 유효성을 보장할 수 없으므로 호출자는 락 안에서 다 써야 한다. */
			return vgadev;
	/* [한국어] 목록 끝까지 없었다 -- 중재 대상이 아닌 장치이거나 이미 제거됐다. */
	return NULL;
}

/**
 * vga_default_device - return the default VGA device, for vgacon
 *
 * This can be defined by the platform. The default implementation is
 * rather dumb and will probably only work properly on single VGA card
 * setups and/or x86 platforms.
 *
 * If your VGA default device is not PCI, you'll have to return NULL here.
 * In this case, I assume it will not conflict with any PCI card. If this
 * is not true, I'll have to define two arch hooks for enabling/disabling
 * the VGA default device if that is possible. This may be a problem with
 * real _ISA_ VGA cards, in addition to a PCI one. I don't know at this
 * point how to deal with that card. Can their IOs be disabled at all? If
 * not, then I suppose it's a matter of having the proper arch hook telling
 * us about it, so we basically never allow anybody to succeed a vga_get().
 */
/*
 * [한국어]
 * vga_default_device - 시스템의 기본 VGA 장치를 돌려준다
 *
 * @return: 기본 VGA 장치의 pci_dev 포인터, 아직 정해지지 않았거나 기본 장치가
 *   PCI 장치가 아니면 NULL. 호출자는 NULL 을 반드시 처리해야 한다 -- vga_get()
 *   은 NULL 이면 "중재할 것이 없다"고 보고 0(성공)을 돌려준다.
 *
 * 왜 필요한가: 전역 변수 vga_default 를 파일 밖으로 노출하지 않으면서 값만
 * 읽게 하는 접근자다. 바로 위 상류 커널독이 설명하듯 이 선택 로직은 플랫폼이
 * 재정의할 수도 있는 것으로 상정돼 있고, 기본 구현은 단순해서 VGA 카드가 한 장뿐인
 * 시스템이나 x86 에서나 제대로 동작한다.
 *
 * @return 값에 참조 카운트를 더해 주지 않는다는 점이 중요하다. 호출자는
 * 이것을 "쿠키"로만 써야 하며, 오래 붙들려면 스스로 pci_dev_get() 을 해야 한다
 * (vga_arb_write() 의 "target default" 경로가 실제로 그렇게 한다).
 *
 * 실행 컨텍스트: 락을 잡지 않는 한 줄 접근자라 어떤 문맥에서도 부를 수 있다.
 * vga_get()/vga_tryget()/vga_put() 이 스핀락을 잡기 **전에** 부르고,
 * vga_is_boot_device() 는 vga_lock 을 쥔 상태에서 부른다.
 *
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   vga_get() / vga_put() / vga_arb_open() / vgacon 등 -> [vga_default_device]
 */
struct pci_dev *vga_default_device(void)
{
	/* [한국어] 전역 값을 그대로 돌려준다. 잠금도, 참조 카운트 증가도 없다. */
	return vga_default;
}
/* [한국어] GPL 심볼로 내보낸다. vgacon 과 GPU 드라이버가 "내가 부팅 콘솔
 * 카드인가"를 물을 때 쓴다. EXPORT_SYMBOL 이 아니라 _GPL 인 것은 상류의
 * 선택이며 코드에서 확인되는 사실 그대로다. */
EXPORT_SYMBOL_GPL(vga_default_device);

/*
 * [한국어]
 * vga_set_default_device - 기본 VGA 장치를 바꾸고 참조 카운트를 옮긴다
 *
 * @pdev: 새 기본 장치. NULL 을 넘기면 "기본 장치 없음"으로 만든다
 *   (vga_arbiter_del_pci_device() 가 기본 장치가 뽑혔을 때 그렇게 한다).
 * @return: 없음.
 *
 * 왜 필요한가: vga_default 는 단순한 포인터가 아니라 pci_dev 의 참조를 하나
 * 붙들고 있는 자리다. 그냥 대입해 버리면 (a) 옛 장치의 참조가 새지고,
 * (b) 새 장치가 그 사이에 해제되면 vga_default 가 허상 포인터가 된다. 이
 * 함수는 그 짝맞춤을 한 곳에 모아 둔 것이다.
 *
 * 동작 단계: (1) 값이 그대로면 아무것도 하지 않는다 -- 여기서 조기 반환하지
 * 않으면 같은 포인터에 put 을 먼저 걸어 참조가 0 이 될 수 있어 위험하다.
 * (2) 옛 값의 참조를 하나 내린다(pci_dev_put 은 NULL 을 받아도 안전하다).
 * (3) 새 값의 참조를 하나 올려 저장한다(pci_dev_get 도 NULL 을 받으면 NULL 을
 * 돌려주므로 NULL 대입이 자연스럽게 처리된다).
 *
 * 실행 컨텍스트: 이 함수 자체는 어떤 락도 잡지 않는다. 호출자
 * vga_arbiter_add_pci_device() / vga_arbiter_del_pci_device() 는 vga_lock 을 쥔
 * 채로 부르므로, 그 경로에서는 스핀락 안에서 참조 카운트 조작이 일어난다
 * (atomic 연산이라 잠들지 않아 허용된다).
 *
 * 에러 경로: 없음. 실패할 수 있는 연산이 없다.
 *
 * 호출 체인:
 *   vga_arbiter_add_pci_device() / vga_arbiter_del_pci_device()
 *     -> [vga_set_default_device] -> pci_dev_put() / pci_dev_get()
 */
void vga_set_default_device(struct pci_dev *pdev)
{
	/* [한국어] 이미 같은 장치면 할 일이 없다. 이 조기 반환이 없으면 아래에서
	 * 같은 포인터에 put 을 먼저 걸어 참조 카운트를 잠시 떨어뜨리게 되는데,
	 * 그 순간 다른 CPU 가 마지막 참조를 놓으면 장치가 해제돼 버린다. */
	if (vga_default == pdev)
		return;

	/* [한국어] 옛 기본 장치의 참조를 반납한다. vga_default 가 NULL 이어도
	 * pci_dev_put(NULL) 은 아무 일도 하지 않으므로 검사할 필요가 없다. */
	pci_dev_put(vga_default);
	/* [한국어] 새 장치의 참조를 하나 잡고 전역에 기록한다. 이 참조 덕분에
	 * vga_default 는 다음 vga_set_default_device() 호출 때까지 유효하다.
	 * pdev 가 NULL 이면 pci_dev_get() 이 NULL 을 돌려주므로 결과적으로
	 * "기본 장치 없음" 상태가 된다. */
	vga_default = pci_dev_get(pdev);
}

/**
 * vga_remove_vgacon - deactivate VGA console
 *
 * Unbind and unregister vgacon in case pdev is the default VGA device.
 * Can be called by GPU drivers on initialization to make sure VGA register
 * access done by vgacon will not disturb the device.
 *
 * @pdev: PCI device.
 */
/*
 * [한국어]
 * vga_remove_vgacon - 이 카드가 기본 VGA 장치라면 VGA 텍스트 콘솔을 떼어낸다
 *
 * @pdev: VGA 콘솔에서 떼어내고 싶은 PCI 장치. 보통 GPU 드라이버가 자기 자신을
 *   넘긴다. 이 장치가 vga_default 가 아니면 아무 일도 하지 않는다.
 * @return: 0 = 성공(또는 할 일이 없었음). 음수 = 실패.
 *   -ENODEV 는 더미 콘솔이 없어 vgacon 을 대체할 수단이 아예 없다는 뜻이다.
 *   호출자(GPU 드라이버)는 실패하면 보통 probe 를 중단하거나 경고를 남긴다.
 *
 * 왜 필요한가: vgacon 은 화면에 글자를 찍으려고 레거시 VGA 레지스터를 계속
 * 건드린다. GPU 드라이버가 같은 하드웨어를 자기 방식으로 프로그래밍하기
 * 시작하면 두 코드가 같은 레지스터를 두고 싸우게 되고, 화면이 깨지거나 장치가
 * 멈춘다. 그래서 드라이버 초기화 시점에 콘솔을 먼저 떼어낸다. 위 상류 커널독의
 * "make sure VGA register access done by vgacon will not disturb the device" 가
 * 바로 그 뜻이다.
 *
 * 이 함수는 커널 설정에 따라 세 가지 구현 중 하나만 컴파일된다 -- 아래 각
 * 갈래의 주석 참고.
 *
 * 실행 컨텍스트: 잠들 수 있는 프로세스 문맥 전용이다. console_lock() 은
 * 뮤텍스 계열의 잠금이라 인터럽트나 스핀락 안에서 부르면 안 된다. vga_lock 은
 * 잡지 않으며, vga_default 를 락 없이 읽는다.
 *
 * 에러 경로: do_take_over_console() 이 실패하면 그 오류를 그대로 돌려주고
 * vgacon 등록 해제는 시도하지 않는다(콘솔을 대체하지 못한 채 떼면 출력이
 * 사라지므로). do_unregister_con_driver() 의 -ENODEV 는 "이미 해제됨"이라
 * 성공으로 접는다.
 *
 * 호출 체인:
 *   GPU 드라이버 probe -> [vga_remove_vgacon]
 *     -> console_lock() -> con_is_bound() -> do_take_over_console()
 *     -> do_unregister_con_driver() -> console_unlock()
 */
/* [한국어] 갈래 1 -- CONFIG_VGA_CONSOLE 이 꺼져 있어 vgacon 자체가 커널에
 * 없는 경우. 떼어낼 콘솔이 없으므로 아무 일도 하지 않고 성공을 돌려준다.
 * 이 빈 구현이 있어야 호출하는 GPU 드라이버들이 #ifdef 없이 그냥 부를 수 있다. */
#if !defined(CONFIG_VGA_CONSOLE)
int vga_remove_vgacon(struct pci_dev *pdev)
{
	/* [한국어] vgacon 이 존재하지 않으니 "이미 목적이 달성된" 상태다. */
	return 0;
}
/* [한국어] 갈래 2 -- vgacon 은 있는데 CONFIG_DUMMY_CONSOLE 이 꺼져 있는 경우.
 * vgacon 을 떼려면 그 자리를 대신할 콘솔 드라이버가 있어야 하는데, 그 대역이
 * 바로 dummy_con 이다. 대체재가 없으면 떼는 순간 콘솔 출력이 사라지므로
 * 아예 시도하지 않고 실패를 알린다. */
#elif !defined(CONFIG_DUMMY_CONSOLE)
int vga_remove_vgacon(struct pci_dev *pdev)
{
	/* [한국어] -ENODEV = "그럴 장치(대체 콘솔)가 없다". 호출자는 이 값을 보고
	 * vgacon 을 떼지 못한 채로 진행할지 결정한다. */
	return -ENODEV;
}
/* [한국어] 갈래 3 -- vgacon 도 있고 dummy_con 도 있는 정상 경우. 실제로
 * 콘솔을 갈아 끼운다. */
#else
int vga_remove_vgacon(struct pci_dev *pdev)
{
	/* [한국어] 콘솔 API 들의 반환값을 모으는 변수. 0 으로 시작해야
	 * "con_is_bound() 가 거짓이라 아무것도 안 했다"는 경우가 성공으로 흐른다. */
	int ret = 0;

	/* [한국어] 기본 VGA 장치가 아니면 vgacon 과 무관한 카드다. vgacon 은
	 * vga_default 하나만 건드리므로, 그 외 카드는 떼어낼 것이 없다.
	 * 참고: 여기서 vga_default 를 vga_lock 없이 직접 읽는다. */
	if (pdev != vga_default)
		return 0;
	/* [한국어] 무엇을 왜 하는지 dmesg 에 남긴다. 콘솔이 갑자기 사라지는
	 * 현상을 나중에 추적할 수 있게 하는 흔적이다. */
	vgaarb_info(&pdev->dev, "deactivate vga console\n");

	/* [한국어] 콘솔 서브시스템 전역 잠금. 콘솔 드라이버 등록/해제와 화면 전환은
	 * 이 락 안에서만 해야 한다. 잠들 수 있는 락이므로 이 아래 구간에서는
	 * 스핀락을 잡거나 원자적 문맥에 들어가서는 안 된다. */
	console_lock();
	/* [한국어] vga_con 이 지금 실제로 어떤 가상 콘솔에 묶여 있는지 확인한다.
	 * 묶여 있지 않다면 화면을 넘겨받을 필요 없이 등록 해제만 하면 된다. */
	if (con_is_bound(&vga_con))
		/* [한국어] 0번부터 MAX_NR_CONSOLES-1(=62)번까지 모든 가상 콘솔의
		 * 소유권을 더미 콘솔로 넘긴다. 마지막 인자 1 은 "이 드라이버를 기본
		 * 콘솔로 삼으라"는 뜻이다. dummy_con 은 출력을 버리기만 하므로,
		 * 이 시점 이후 VGA 레지스터를 건드리는 콘솔 코드는 없어진다. */
		ret = do_take_over_console(&dummy_con, 0,
					   MAX_NR_CONSOLES - 1, 1);
	/* [한국어] 화면 인계에 성공했을 때만(또는 애초에 묶여 있지 않았을 때만)
	 * 드라이버 등록을 해제한다. 실패했는데도 해제하면 콘솔이 사라진다. */
	if (ret == 0) {
		/* [한국어] vga_con 을 콘솔 드라이버 목록에서 완전히 뺀다. */
		ret = do_unregister_con_driver(&vga_con);

		/* Ignore "already unregistered". */
		/* [한국어] 다른 경로에서 이미 해제됐다면 목적은 달성된 것이므로
		 * 오류로 보지 않는다. GPU 드라이버가 두 번 부르는 경우 등이 여기 걸린다. */
		if (ret == -ENODEV)
			ret = 0;
	}
	/* [한국어] 콘솔 잠금 해제. 위 두 API 는 반드시 이 락 안에서 짝지어 불려야 한다. */
	console_unlock();

	/* [한국어] 0 이면 vgacon 이 확실히 떨어져 나갔다는 뜻이다. */
	return ret;
}
#endif
/* [한국어] 세 갈래 중 어느 것이 컴파일되든 이름은 같으므로 EXPORT 는 한 번만
 * 한다. _GPL 이 아닌 EXPORT_SYMBOL 이라 라이선스 제약 없이 모듈이 쓸 수 있다. */
EXPORT_SYMBOL(vga_remove_vgacon);

/*
 * If we don't ever use VGA arbitration, we should avoid turning off
 * anything anywhere due to old X servers getting confused about the boot
 * device not being VGA.
 */
/*
 * [한국어]
 * vga_check_first_use - VGA 중재가 처음 쓰이는 순간을 감지해 클라이언트에게 알린다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 바로 위 상류 영어 주석이 정책을 설명한다 -- 중재를 아무도 쓰지
 * 않는데 미리 카드들의 디코딩을 꺼 버리면, "부팅 장치가 VGA 가 아니다"라고
 * 오해한 옛 X 서버들이 혼란에 빠진다. 그래서 중재기는 부팅 직후에는 아무것도
 * 끄지 않고 가만히 있다가, 누군가 실제로 vga_get()/vga_tryget() 으로 자원을
 * 요구하는 첫 순간에만 "이제부터 중재를 시작한다"고 GPU 드라이버들에게 알린다.
 * 알림을 받은 드라이버는 set_decode 콜백에서 자기 카드의 레거시 디코딩을
 * 꺼도 좋다고 판단할 수 있다.
 *
 * 동작 단계: 플래그가 아직 거짓이면 참으로 올리고 vga_arbiter_notify_clients()
 * 를 부른다. 이미 참이면 아무 일도 하지 않는다 -- 단 한 번만 일어나는 사건이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **vga_lock 을 잡지 않은 상태에서** 불린다
 * (vga_get()/vga_tryget() 모두 스핀락을 잡기 전에 부른다). 이것이 중요한 이유는
 * vga_arbiter_notify_clients() 가 내부에서 vga_lock 을 직접 잡기 때문이다 --
 * 락을 쥔 채 불렀다면 자기 자신을 두 번 잡는 데드락이 된다.
 *
 * 경쟁 조건: 플래그 검사와 설정이 원자적이지 않아, 두 CPU 가 동시에 첫 사용을
 * 하면 알림이 두 번 갈 수 있다. 알림 자체는 멱등(각 클라이언트의 현재 상태를
 * 다시 반영할 뿐)이라 상태가 깨지지는 않는다.
 *
 * 호출 체인:
 *   vga_get() / vga_tryget() -> [vga_check_first_use]
 *     -> vga_arbiter_notify_clients() -> vgadev->set_decode()
 */
static void vga_check_first_use(void)
{
	/*
	 * Inform all GPUs in the system that VGA arbitration has occurred
	 * so they can disable resources if possible.
	 */
	/* [한국어] 아직 한 번도 중재가 쓰이지 않았다면 -- 즉 이번이 처음이라면. */
	if (!vga_arbiter_used) {
		/* [한국어] 먼저 플래그를 올린다. 알림 함수가 이 플래그를 다시 보고
		 * 조기 반환하기 때문에, 순서를 바꾸면 알림이 아무 일도 하지 않는다
		 * (vga_arbiter_notify_clients() 첫 줄의 `if (!vga_arbiter_used) return;`
		 * 참고). */
		vga_arbiter_used = true;
		/* [한국어] 등록된 모든 GPU 클라이언트에게 set_decode 콜백을 걸어
		 * 현재 카드 수에 맞는 디코딩 상태를 적용시킨다. */
		vga_arbiter_notify_clients();
	}
}

/*
 * [한국어]
 * __vga_tryget - 요청한 VGA 자원을 실제로 확보하고 잠근다 (중재의 심장)
 *
 * @vgadev: 자원을 원하는 카드의 중재 항목. 호출자가 vgadev_find() 로 이미 찾아
 *   놓은 것이며, NULL 이 아님이 보장된다.
 * @rsrc: 원하는 자원의 비트마스크(VGA_RSRC_* 조합). 값으로 받으므로 이 함수
 *   안에서 승격(NORMAL -> LEGACY 추가)해도 호출자에게는 영향이 없다.
 * @return: 세 가지다.
 *   NULL          -- 성공. 자원을 확보했고 잠금 카운터도 올렸다.
 *   유효한 포인터 -- 실패. 그 카드가 자원을 잠그고 있어 지금은 줄 수 없다.
 *                    vga_get() 은 이 값을 보고 대기열에서 잠들었다 재시도하고,
 *                    vga_tryget() 은 곧바로 -EBUSY 로 돌아선다.
 *   ERR_PTR(err)  -- pci_set_vga_state() 가 하드웨어 설정에 실패했다.
 *                    호출자는 IS_ERR() 로 이 경우를 먼저 걸러야 한다.
 *
 * 왜 필요한가: 레거시 VGA 자원은 주소가 고정이라 여러 카드가 동시에 응답할 수
 * 있다. 그래서 "내가 쓰겠다"는 것만으로는 부족하고, 같은 자원을 켜 둔 다른
 * 카드를 먼저 꺼야 한다. 이 함수가 그 "빼앗고 켜기"를 한 번에 수행한다.
 *
 * 동작 단계:
 *  (1) 승격 -- NORMAL 자원을 요청했는데 이 카드가 대응하는 레거시도 해석한다면,
 *      레거시 자원까지 함께 요청한 것으로 취급한다. 그러지 않으면 NORMAL 접근이
 *      레거시 주소로도 응답해 다른 카드와 충돌하기 때문이다.
 *  (2) wants 계산 -- 이미 소유(owns)한 것을 뺀 나머지가 새로 얻어야 할 자원이다.
 *      아무것도 없으면 곧장 lock_them 으로 뛰어 카운터만 올린다.
 *  (3) 충돌 처리 -- 레거시 자원이 필요하면 vga_list 를 훑어 그 자원을 소유한
 *      카드를 찾는다. 상대가 잠금을 쥐고 있으면 빼앗지 못하고 그 카드를 반환한다.
 *      잠금이 없으면 pci_set_vga_state(false) 로 꺼서 빼앗는다.
 *  (4) 자기 켜기 -- pci_set_vga_state(true) 로 자기 경로를 열고 owns 를 갱신한다.
 *  (5) 잠금 -- locks 비트를 세우고 자원별 중첩 카운터를 하나씩 올린다.
 *
 * 실행 컨텍스트: **반드시 vga_lock 을 쥔 채로 불러야 한다.** vga_list 를
 * 순회하고 여러 vga_device 의 필드를 고치기 때문이다. 그 안에서
 * pci_set_vga_state() 가 config space 접근(pci_read/write_config_word)을 하므로,
 * 스핀락을 쥔 채 상대적으로 느린 하드웨어 접근이 일어난다. 잠들지는 않으므로
 * 규칙 위반은 아니다.
 *
 * 부분 실패에 대한 경고: (3)에서 여러 카드를 차례로 껐는데 (4)에서 자기 자신을
 * 켜다 실패하면, 이미 꺼 버린 카드들을 되돌리지 않고 ERR_PTR 로 빠진다. 이
 * 트리의 코드에는 그 복구 경로가 없다 -- 사실 그대로 적어 둔다.
 *
 * 호출 체인:
 *   vga_get() / vga_tryget() -> [__vga_tryget]
 *     -> pci_set_vga_state() [drivers/pci/pci.c]
 *          -> pci_write_config_word(PCI_COMMAND) 와
 *             경로상 브리지의 pci_write_config_word(PCI_BRIDGE_CONTROL)
 */
static struct vga_device *__vga_tryget(struct vga_device *vgadev,
				       unsigned int rsrc)
{
	/* [한국어] 로그 매크로(vgaarb_dbg)에 넘길 struct device 포인터를 미리 꺼내 둔다.
	 * pci_dev 안에 embed 된 device 구조체의 주소이며, 별도 참조를 잡지 않는다 --
	 * vgadev->pdev 가 살아 있는 동안만 유효하고, 그 수명은 호출자가 보장한다. */
	struct device *dev = &vgadev->pdev->dev;
	/* [한국어] wants        -- 아직 소유하지 못해 새로 얻어야 하는 자원 비트.
	 * legacy_wants -- 그중 레거시 자원만 남긴 것. 충돌 탐색이 필요한지 판단하는 기준.
	 * match        -- 충돌 카드가 실제로 소유하고 있어 빼앗아야 할 자원 비트. */
	unsigned int wants, legacy_wants, match;
	/* [한국어] vga_list 순회 커서 겸, 빼앗을 수 없는 카드를 만났을 때 호출자에게
	 * 돌려줄 "충돌자" 포인터. 이름 그대로 두 역할을 겸한다. */
	struct vga_device *conflict;
	/* [한국어] pci_set_vga_state() 에 넘길 PCI COMMAND 레지스터 비트 모음.
	 * PCI_COMMAND_IO(bit 0) 와 PCI_COMMAND_MEMORY(bit 1) 만 들어간다. 초기화되지
	 * 않은 채 선언되지만, 쓰기 전에 항상 0 으로 대입되므로 문제는 없다. */
	unsigned int pci_bits;
	/* [한국어] pci_set_vga_state() 의 동작 범위를 지정하는 플래그.
	 * PCI_VGA_STATE_CHANGE_DECODES(1<<1) 이면 장치의 COMMAND 레지스터를 고치고,
	 * PCI_VGA_STATE_CHANGE_BRIDGE(1<<0) 이면 조상 브리지들의 VGA Enable 을 고친다.
	 * 여기서 0 으로 초기화하는 이유는 lock_them 으로 곧장 뛰는 경로에서도
	 * 정의된 값을 갖게 하기 위해서다. */
	u32 flags = 0;
	/* [한국어] pci_set_vga_state() 의 반환값을 받는다. 0 이 아니면 하드웨어 설정
	 * 실패이며 ERR_PTR 로 감싸 호출자에게 전달한다. */
	int err;

	/*
	 * Account for "normal" resources to lock. If we decode the legacy,
	 * counterpart, we need to request it as well
	 */
	/* [한국어] 승격 규칙 1 -- "정상 IO"를 요청했는데 이 카드가 레거시 IO 도
	 * 해석한다면, 레거시 IO 까지 함께 요청한 것으로 친다. 이유는 하드웨어가 한
	 * 덩어리이기 때문이다: 카드의 IO 디코더를 켜면 BAR 로 잡은 정상 IO 뿐 아니라
	 * 레거시 0x3b0 대역에도 함께 응답한다. 레거시 쪽을 중재에 넣지 않으면 다른
	 * 카드와 조용히 충돌한다. */
	if ((rsrc & VGA_RSRC_NORMAL_IO) &&
	    (vgadev->decodes & VGA_RSRC_LEGACY_IO))
		rsrc |= VGA_RSRC_LEGACY_IO;
	/* [한국어] 승격 규칙 2 -- 위와 같은 논리의 메모리 쪽 짝. 정상 MEM 요청이
	 * 레거시 0xa0000 대역까지 열어 버리므로 함께 요청한 것으로 취급한다. */
	if ((rsrc & VGA_RSRC_NORMAL_MEM) &&
	    (vgadev->decodes & VGA_RSRC_LEGACY_MEM))
		rsrc |= VGA_RSRC_LEGACY_MEM;

	/* [한국어] 동적 디버그 로그. 승격까지 끝난 최종 요청 비트를 찍는다.
	 * __func__ 는 컴파일러가 넣어 주는 현재 함수 이름 문자열이다. */
	vgaarb_dbg(dev, "%s: %d\n", __func__, rsrc);
	/* [한국어] 요청 직전의 소유 상태를 함께 찍어 둔다. 아래 wants 계산 결과를
	 * dmesg 만 보고도 재현할 수 있게 하는 진단용 짝이다. */
	vgaarb_dbg(dev, "%s: owns: %d\n", __func__, vgadev->owns);

	/* Check what resources we need to acquire */
	/* [한국어] 새로 얻어야 할 자원 = 요청한 것 중 아직 소유하지 않은 것.
	 * ~vgadev->owns 로 소유 비트를 반전시켜 AND 하면 "요청했지만 없는 것"만 남는다.
	 * 이미 켜져 있는 자원을 다시 켜려고 config space 를 건드리지 않기 위한 계산이다. */
	wants = rsrc & ~vgadev->owns;

	/* We already own everything, just mark locked & bye bye */
	/* [한국어] 얻을 것이 하나도 없다 -- 즉 요청한 자원을 이미 전부 소유하고 있다.
	 * 하드웨어는 손댈 필요가 없으므로 잠금 카운터만 올리러 곧장 lock_them 으로 뛴다.
	 * vga_get() 의 영어 주석이 말하는 "If the card already owns the resources, the
	 * function succeeds" 가 바로 이 지름길이다. */
	if (wants == 0)
		goto lock_them;

	/*
	 * We don't need to request a legacy resource, we just enable
	 * appropriate decoding and go.
	 */
	/* [한국어] 새로 얻어야 할 것 중 레거시 자원만 골라낸다.
	 * VGA_RSRC_LEGACY_MASK 는 IO(0x01)|MEM(0x02) = 0x03 이다. */
	legacy_wants = wants & VGA_RSRC_LEGACY_MASK;
	/* [한국어] 레거시 자원이 필요 없다면(= 순수 NORMAL 자원만 새로 얻으면 된다면)
	 * 다른 카드와 다툴 일이 없다. 정상 자원은 BAR 로 분리돼 있어 주소가 겹치지
	 * 않기 때문이다. 그래서 충돌 탐색 루프를 통째로 건너뛰고 자기 디코딩만 켠다. */
	if (legacy_wants == 0)
		goto enable_them;

	/* Ok, we don't, let's find out who we need to kick off */
	/* [한국어] 레거시 자원이 필요하다 -- 이제 그것을 쥐고 있는 카드를 찾아
	 * 꺼야 한다. vga_list 전체를 훑는다. 호출자가 vga_lock 을 쥐고 있으므로
	 * 순회 중 목록이 바뀌지 않는다. */
	list_for_each_entry(conflict, &vga_list, list) {
		/* [한국어] 이번 상대에게 요구할 자원. 기본값은 legacy_wants 지만, 아래에서
		 * 버스가 다르면 IO|MEM 둘 다로 넓어진다. 루프 변수로 매 반복 초기화된다. */
		unsigned int lwants = legacy_wants;
		/* [한국어] 이 상대를 끄기 위해 브리지의 VGA Enable 비트까지 손대야 하는가.
		 * 같은 버스에 있으면 브리지를 건드릴 필요가 없다(브리지를 끄면 나까지 꺼진다). */
		unsigned int change_bridge = 0;

		/* Don't conflict with myself */
		/* [한국어] 자기 자신은 충돌 상대가 아니다. 이 검사가 없으면 자기 자원을
		 * 스스로 빼앗아 끄는 자기모순이 일어난다. */
		if (vgadev == conflict)
			continue;

		/*
		 * We have a possible conflict. Before we go further, we must
		 * check if we sit on the same bus as the conflicting device.
		 * If we don't, then we must tie both IO and MEM resources
		 * together since there is only a single bit controlling
		 * VGA forwarding on P2P bridges.
		 */
		/* [한국어] 상대가 나와 다른 PCI 버스에 있는 경우. P2P 브리지의 VGA Enable 은
		 * IO 와 MEM 을 구분하지 않는 단 하나의 비트라(위 상류 주석 참조), 브리지를
		 * 조작하는 순간 두 자원이 함께 움직인다. 그래서 요구 범위를 둘 다로 넓힌다. */
		if (vgadev->pdev->bus != conflict->pdev->bus) {
			/* [한국어] 이 상대를 끌 때 pci_set_vga_state() 에
			 * PCI_VGA_STATE_CHANGE_BRIDGE 를 붙이겠다는 표시. */
			change_bridge = 1;
			/* [한국어] 브리지 비트가 IO/MEM 을 나누지 못하므로 요구도 나눌 수 없다.
			 * 둘 다 요구해야 아래 잠금 검사와 owns 검사가 올바르게 보수적으로 동작한다. */
			lwants = VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
		}

		/*
		 * Check if the guy has a lock on the resource. If he does,
		 * return the conflicting entry.
		 */
		/* [한국어] 상대가 내가 원하는 자원을 **잠가** 두었는가. 잠금은 "지금 쓰는
		 * 중이니 건드리지 말라"는 예약이므로 빼앗을 수 없다. 이때는 그 카드를 그대로
		 * 돌려주어, vga_get() 이 대기열에서 자고 vga_tryget() 이 -EBUSY 를 내게 한다.
		 * 여기서 곧장 return 하므로 이미 처리한 앞쪽 카드들의 상태는 그대로 남는다
		 * (앞쪽에서 무언가를 껐다면 되돌리지 않는다). */
		if (conflict->locks & lwants)
			return conflict;

		/*
		 * Ok, now check if it owns the resource we want.  We can
		 * lock resources that are not decoded; therefore a device
		 * can own resources it doesn't decode.
		 */
		/* [한국어] 상대가 실제로 켜 두고 있는(owns) 자원 중 내가 원하는 것만 남긴다.
		 * 위 상류 주석이 강조하듯, owns 는 decodes 의 부분집합이 아니다 -- 해석하지
		 * 못하는 자원도 소유(잠금)할 수 있으므로 decodes 가 아니라 owns 로 판정한다. */
		match = lwants & conflict->owns;
		/* [한국어] 겹치는 것이 없으면 이 상대와는 다툴 일이 없다. 다음 카드로. */
		if (!match)
			continue;

		/*
		 * Looks like he doesn't have a lock, we can steal them
		 * from him.
		 */

		/* [한국어] 이 상대를 끄기 위한 pci_set_vga_state() 인자를 여기서부터 새로
		 * 조립한다. 루프 밖에서 선언된 변수라 이전 반복의 값이 남아 있을 수 있으므로
		 * 반드시 0 으로 초기화해야 한다. */
		flags = 0;
		/* [한국어] COMMAND 레지스터 비트도 마찬가지 이유로 초기화한다. */
		pci_bits = 0;

		/*
		 * If we can't control legacy resources via the bridge, we
		 * also need to disable normal decoding.
		 */
		/* [한국어] 상대의 라우팅을 브리지 하나로 제어할 수 없는 경우 -- 그 브리지
		 * 아래에 VGA 장치가 둘 이상 매달려 있어서, 브리지를 끄면 애먼 카드까지 꺼진다.
		 * 그러니 브리지 대신 상대 장치 자신의 COMMAND 비트를 꺼야 한다. */
		if (!conflict->bridge_has_one_vga) {
			/* [한국어] 빼앗을 자원 중 상대가 실제로 "해석"까지 하는 메모리가 있으면
			 * PCI_COMMAND_MEMORY(config space 0x04 의 bit 1)를 끄기 대상에 넣는다.
			 * decodes 로 한 번 더 거르는 이유: 해석하지도 않는 자원 때문에 상대의 메모리
			 * 디코딩을 통째로 꺼 버리면 그 장치의 정상 BAR 접근까지 죽기 때문이다. */
			if ((match & conflict->decodes) & VGA_RSRC_LEGACY_MEM)
				pci_bits |= PCI_COMMAND_MEMORY;
			/* [한국어] 같은 논리의 IO 쪽. PCI_COMMAND_IO 는 config space 0x04 의 bit 0. */
			if ((match & conflict->decodes) & VGA_RSRC_LEGACY_IO)
				pci_bits |= PCI_COMMAND_IO;

			/* [한국어] 끌 COMMAND 비트가 하나라도 정해졌다면, pci_set_vga_state() 에게
			 * "장치의 COMMAND 레지스터를 고쳐라"라고 지시하는 플래그를 붙인다. 비트가
			 * 없으면 이 플래그도 붙이지 않아 불필요한 config space 쓰기를 피한다. */
			if (pci_bits)
				flags |= PCI_VGA_STATE_CHANGE_DECODES;
		}

		/* [한국어] 버스가 달랐던 경우에만 브리지의 VGA Enable 까지 끈다.
		 * 같은 버스였다면 브리지를 끄는 순간 나 자신도 함께 꺼지므로 절대 붙이면 안 된다. */
		if (change_bridge)
			flags |= PCI_VGA_STATE_CHANGE_BRIDGE;

		/* [한국어] 실제로 상대를 끈다. decode=false 이므로 pci_set_vga_state() 는
		 * pci_bits 에 든 COMMAND 비트를 지우고(CHANGE_DECODES 가 있을 때),
		 * 경로상 브리지들의 PCI_BRIDGE_CTL_VGA 를 지운다(CHANGE_BRIDGE 가 있을 때).
		 * 이 호출이 이 파일에서 하드웨어를 실제로 바꾸는 두 지점 중 하나다. */
		err = pci_set_vga_state(conflict->pdev, false, pci_bits, flags);
		/* [한국어] 하드웨어 설정 실패. ERR_PTR 로 오류 코드를 포인터에 실어 돌려준다.
		 * 호출자는 IS_ERR() 로 이 경우를 "충돌 카드 반환"과 구분한다. 앞서 끈 카드들은
		 * 복구하지 않는다 -- 이 트리에 그 복구 코드는 없다. */
		if (err)
			return ERR_PTR(err);
		/* [한국어] 소프트웨어 그림자 상태를 하드웨어와 맞춘다. 방금 끈 자원을
		 * 상대의 owns 에서 지운다. 이 갱신을 빠뜨리면 다음 중재가 이미 꺼진 자원을
		 * 또 끄려 하거나, 상대가 여전히 소유 중이라고 오판한다. */
		conflict->owns &= ~match;

		/* If we disabled normal decoding, reflect it in owns */
		/* [한국어] COMMAND 의 MEMORY 비트를 껐다면 그 장치의 정상 메모리 디코딩도
		 * 함께 죽은 것이다. 레거시만 지운 위 줄로는 부족하므로 NORMAL_MEM 도 지운다. */
		if (pci_bits & PCI_COMMAND_MEMORY)
			conflict->owns &= ~VGA_RSRC_NORMAL_MEM;
		/* [한국어] IO 쪽 짝. COMMAND 의 IO 비트를 껐으면 NORMAL_IO 도 지운다. */
		if (pci_bits & PCI_COMMAND_IO)
			conflict->owns &= ~VGA_RSRC_NORMAL_IO;
	}

/* [한국어] 여기부터는 "빼앗기"가 끝나고 "내 것 켜기" 단계다. 레거시가 필요
 * 없어 충돌 탐색을 건너뛴 경로(legacy_wants == 0)도 이 레이블로 들어온다. */
enable_them:
	/*
	 * Ok, we got it, everybody conflicting has been disabled, let's
	 * enable us.  Mark any bits in "owns" regardless of whether we
	 * decoded them.  We can lock resources we don't decode, therefore
	 * we must track them via "owns".
	 */
	/* [한국어] 이번에는 나 자신을 켜기 위한 인자를 조립한다. 위 루프에서 쓰던
	 * 값이 남아 있으므로 반드시 초기화한다. */
	flags = 0;
	/* [한국어] 같은 이유로 COMMAND 비트도 초기화. */
	pci_bits = 0;

	/* [한국어] 내 라우팅을 브리지 하나로 제어할 수 없다면, 브리지만 열어서는
	 * 내 디코더가 켜지지 않는다. 그러니 내 COMMAND 비트를 직접 켜야 한다. */
	if (!vgadev->bridge_has_one_vga) {
		/* [한국어] 조건 없이 CHANGE_DECODES 를 붙인다 -- 끌 때(pci_bits 가 0 이면
		 * 붙이지 않던 것)와 달리, 켜는 쪽은 pci_bits 가 0 이어도 플래그를 세운다.
		 * 코드에서 확인되는 사실 그대로 적는다. */
		flags |= PCI_VGA_STATE_CHANGE_DECODES;
		/* [한국어] 새로 얻을 자원에 메모리가 포함되면 PCI_COMMAND_MEMORY 를 켠다.
		 * 여기서는 레거시와 정상을 함께 본다 -- 어느 쪽이든 메모리 디코더가 필요하다.
		 * 빼앗을 때(match & conflict->decodes)와 달리 여기서는 decodes 로 한 번 더
		 * 거르지 않는다는 비대칭이 있다. 그 이유를 밝힌 주석은 이 트리 안에 없어
		 * 확인할 수 없다 -- 코드가 그렇게 되어 있다는 사실만 적어 둔다. */
		if (wants & (VGA_RSRC_LEGACY_MEM|VGA_RSRC_NORMAL_MEM))
			pci_bits |= PCI_COMMAND_MEMORY;
		/* [한국어] IO 쪽 짝. 레거시든 정상이든 IO 가 필요하면 PCI_COMMAND_IO 를 켠다. */
		if (wants & (VGA_RSRC_LEGACY_IO|VGA_RSRC_NORMAL_IO))
			pci_bits |= PCI_COMMAND_IO;
	}
	/* [한국어] 레거시 자원을 새로 얻어야 한다면, 내 경로의 브리지들에서 VGA
	 * Enable 을 켜 레거시 사이클이 나에게까지 내려오게 만들어야 한다.
	 * 정상 자원만 필요하면 브리지는 건드리지 않는다 -- 그것은 BAR 윈도로 이미
	 * 라우팅되기 때문이다. */
	if (wants & VGA_RSRC_LEGACY_MASK)
		flags |= PCI_VGA_STATE_CHANGE_BRIDGE;

	/* [한국어] 나 자신을 켠다. decode=true 이므로 COMMAND 비트를 세우고
	 * 경로상 모든 브리지의 PCI_BRIDGE_CTL_VGA 를 세운다. 하드웨어를 바꾸는 두 번째
	 * 지점이다. pci_set_vga_state() 는 브리지가 VGA Enable 쓰기를 지원하지 않아
	 * 되읽었을 때 비트가 서지 않으면 -EIO 를 돌려준다. */
	err = pci_set_vga_state(vgadev->pdev, true, pci_bits, flags);
	/* [한국어] 켜기에 실패하면 오류를 포인터에 실어 반환한다. 앞서 꺼 버린
	 * 충돌 카드들은 여기서도 복구되지 않는다. */
	if (err)
		return ERR_PTR(err);

	/* [한국어] 하드웨어를 켰으니 그림자 상태에도 반영한다. 위 상류 주석이
	 * 강조하듯 decodes 여부와 무관하게 wants 를 전부 owns 에 세운다 -- 해석하지
	 * 못하는 자원도 "내가 잠갔다"는 사실은 owns 로 추적해야 하기 때문이다. */
	vgadev->owns |= wants;
/* [한국어] 여기부터는 잠금 회계다. 하드웨어는 이미 원하는 상태이고, 남은
 * 일은 "누가 얼마나 잠갔는지"를 기록하는 것뿐이다. wants == 0 지름길도
 * 여기로 뛰어든다. */
lock_them:
	/* [한국어] 잠금 비트에는 레거시 자원만 기록한다. 정상 자원은 주소가 겹치지
	 * 않아 배타적 소유가 필요 없으므로 잠금 대상이 아니다(카운터만 센다). */
	vgadev->locks |= (rsrc & VGA_RSRC_LEGACY_MASK);
	/* [한국어] 레거시 IO 잠금 중첩 카운터를 하나 올린다. 같은 카드에 대한
	 * vga_get() 이 여러 번 겹쳐도 안전하도록, 비트가 아니라 카운터로 센다.
	 * __vga_put() 이 이 값을 0 까지 내려야 비로소 locks 비트가 풀린다. */
	if (rsrc & VGA_RSRC_LEGACY_IO)
		vgadev->io_lock_cnt++;
	/* [한국어] 레거시 MEM 쪽 짝. */
	if (rsrc & VGA_RSRC_LEGACY_MEM)
		vgadev->mem_lock_cnt++;
	/* [한국어] 정상 IO 요청 횟수. locks 비트에는 반영되지 않지만, __vga_put() 이
	 * "정상 자원 반납이 레거시 반납까지 동반하는지"를 판단하는 데 필요하다. */
	if (rsrc & VGA_RSRC_NORMAL_IO)
		vgadev->io_norm_cnt++;
	/* [한국어] 정상 MEM 쪽 짝. */
	if (rsrc & VGA_RSRC_NORMAL_MEM)
		vgadev->mem_norm_cnt++;

	/* [한국어] NULL = 성공. 충돌 카드도, 오류도 없다는 뜻이다. */
	return NULL;
}

/*
 * [한국어]
 * __vga_put - 잠금 카운터를 내리고, 0 이 되면 잠금을 풀어 대기자를 깨운다
 *
 * @vgadev: 자원을 반납하는 카드의 중재 항목. 호출자가 vgadev_find() 로 찾아
 *   놓은 것이며 NULL 이 아니다.
 * @rsrc: 반납하는 자원 비트마스크. 값으로 받으므로 안에서 레거시 비트를
 *   덧붙여도 호출자에게는 보이지 않는다.
 * @return: 없음. 반납은 실패하지 않는다.
 *
 * 왜 필요한가: __vga_tryget() 이 올린 중첩 카운터를 짝맞춰 내리는 함수다.
 * 같은 카드에 vga_get() 을 여러 번 겹쳐 부를 수 있으므로, 마지막 하나가
 * 풀릴 때까지 잠금을 유지해야 한다.
 *
 * 핵심 설계 -- "게으른 해제(lazy)": 이 함수는 하드웨어를 전혀 건드리지 않는다.
 * 자원을 실제로 끄지 않고 소프트웨어 잠금 비트만 지운다. 위 vga_put() 커널독의
 * "The resources aren't disabled right away, so that a subsequent vga_get() on
 * the same card will succeed immediately" 가 그 이유다. 실제로 끄는 일은 다음에
 * 다른 카드가 그 자원을 요구할 때 __vga_tryget() 이 대신 해 준다. 그래서
 * 같은 카드를 반복해서 쓰는 흔한 경우에 config space 접근이 아예 없다.
 *
 * 동작 단계: (1) 정상 자원 카운터를 내리고, 이 카드가 대응 레거시를 해석한다면
 * 레거시 반납도 함께 요청된 것으로 승격한다(__vga_tryget 의 승격을 되돌리는
 * 짝이다). (2) 레거시 잠금 카운터를 내린다. (3) 카운터가 0 이 된 자원의 잠금
 * 비트를 지운다. (4) 잠금 비트가 실제로 바뀌었을 때만 대기열을 깨운다.
 *
 * 실행 컨텍스트: **호출자가 vga_lock 을 쥐고 있어야 한다.** vgadev 의 카운터와
 * 비트를 고치기 때문이다. wake_up_all() 은 스핀락 안에서 불러도 되는 연산이다
 * (깨어난 태스크는 락이 풀린 뒤에 진행한다).
 *
 * 에러 경로: 없다. 모든 감소가 "0 보다 클 때만"이라, 짝이 맞지 않는 반납이
 * 들어와도 카운터가 음수로 내려가 언더플로하지 않는다.
 *
 * 호출 체인:
 *   vga_put() / vga_update_device_decodes() -> [__vga_put] -> wake_up_all()
 */
static void __vga_put(struct vga_device *vgadev, unsigned int rsrc)
{
	/* [한국어] 로그용 device 포인터. vgaarb_dbg 에만 쓴다. */
	struct device *dev = &vgadev->pdev->dev;
	/* [한국어] 함수에 들어올 때의 잠금 비트를 기억해 둔다. 맨 아래에서 이 값과
	 * 비교해 "잠금이 실제로 풀렸는가"를 판정하고, 풀렸을 때만 대기열을 깨운다.
	 * 이 비교가 없으면 카운터만 줄고 잠금은 그대로인 경우에도 대기자를 전부 깨워
	 * 헛되이 재시도시키게 된다. */
	unsigned int old_locks = vgadev->locks;

	/* [한국어] 반납이 일어났다는 흔적을 동적 디버그 로그에 남긴다. */
	vgaarb_dbg(dev, "%s\n", __func__);

	/*
	 * Update our counters and account for equivalent legacy resources
	 * if we decode them.
	 */
	/* [한국어] 정상 IO 반납 요청이고, 실제로 올려 둔 카운터가 남아 있을 때만
	 * 처리한다. 카운터 검사가 없으면 짝이 맞지 않는 vga_put() 이 카운터를
	 * 0 에서 UINT_MAX 로 감아 버린다(unsigned int 언더플로). */
	if ((rsrc & VGA_RSRC_NORMAL_IO) && vgadev->io_norm_cnt > 0) {
		/* [한국어] 정상 IO 사용 횟수를 하나 줄인다. */
		vgadev->io_norm_cnt--;
		/* [한국어] __vga_tryget() 의 승격을 되돌리는 자리다. 얻을 때 "정상 IO 를
		 * 요청했고 레거시 IO 도 해석하니 레거시까지 잠갔다"고 했으므로, 반납할 때도
		 * 레거시 IO 를 함께 반납해야 카운터가 맞는다. */
		if (vgadev->decodes & VGA_RSRC_LEGACY_IO)
			/* [한국어] 반납 요청에 레거시 IO 를 덧붙인다. 아래 레거시 카운터 감소가
			 * 이 비트를 보고 동작한다. */
			rsrc |= VGA_RSRC_LEGACY_IO;
	}
	/* [한국어] 메모리 쪽의 같은 처리. 정상 MEM 카운터가 남아 있을 때만. */
	if ((rsrc & VGA_RSRC_NORMAL_MEM) && vgadev->mem_norm_cnt > 0) {
		/* [한국어] 정상 MEM 사용 횟수를 하나 줄인다. */
		vgadev->mem_norm_cnt--;
		/* [한국어] 이 카드가 레거시 MEM 도 해석한다면 승격을 되돌린다. */
		if (vgadev->decodes & VGA_RSRC_LEGACY_MEM)
			/* [한국어] 반납 요청에 레거시 MEM 을 덧붙인다. */
			rsrc |= VGA_RSRC_LEGACY_MEM;
	}
	/* [한국어] 레거시 IO 잠금 중첩 카운터를 내린다. 위에서 승격돼 들어온 몫도
	 * 여기서 함께 처리된다. `> 0` 검사는 언더플로 방지이자, 짝이 맞지 않는
	 * 반납을 조용히 무시하는 방어다. */
	if ((rsrc & VGA_RSRC_LEGACY_IO) && vgadev->io_lock_cnt > 0)
		vgadev->io_lock_cnt--;
	/* [한국어] 레거시 MEM 쪽 짝. */
	if ((rsrc & VGA_RSRC_LEGACY_MEM) && vgadev->mem_lock_cnt > 0)
		vgadev->mem_lock_cnt--;

	/*
	 * Just clear lock bits, we do lazy operations so we don't really
	 * have to bother about anything else at this point.
	 */
	/* [한국어] IO 잠금이 마지막 하나까지 풀렸으면 잠금 비트를 지운다.
	 * 위 상류 주석이 말하듯 여기서 하는 일은 비트를 지우는 것뿐이다 --
	 * 하드웨어(COMMAND/브리지 VGA Enable)는 켜진 채로 남는다. */
	if (vgadev->io_lock_cnt == 0)
		vgadev->locks &= ~VGA_RSRC_LEGACY_IO;
	/* [한국어] MEM 쪽 짝. */
	if (vgadev->mem_lock_cnt == 0)
		vgadev->locks &= ~VGA_RSRC_LEGACY_MEM;

	/*
	 * Kick the wait queue in case somebody was waiting if we actually
	 * released something.
	 */
	/* [한국어] 잠금 비트가 실제로 바뀐 경우에만 대기열을 깨운다. 아무것도
	 * 풀리지 않았다면 깨워 봐야 모두 다시 잠들 뿐이다. */
	if (old_locks != vgadev->locks)
		/* [한국어] vga_wait_queue 의 **모든** 대기자를 깨운다. 전역 대기열 하나로
		 * 모든 카드를 처리하므로, 어느 카드가 무엇을 풀었는지 알 수 없어 전부 깨운 뒤
		 * 각자 __vga_tryget() 을 다시 시도하게 한다. /dev/vga_arbiter 를 poll() 로
		 * 감시하던 유저스페이스도 이 호출로 깨어난다. */
		wake_up_all(&vga_wait_queue);
}

/**
 * vga_get - acquire & lock VGA resources
 * @pdev: PCI device of the VGA card or NULL for the system default
 * @rsrc: bit mask of resources to acquire and lock
 * @interruptible: blocking should be interruptible by signals ?
 *
 * Acquire VGA resources for the given card and mark those resources
 * locked. If the resources requested are "normal" (and not legacy)
 * resources, the arbiter will first check whether the card is doing legacy
 * decoding for that type of resource. If yes, the lock is "converted" into
 * a legacy resource lock.
 *
 * The arbiter will first look for all VGA cards that might conflict and disable
 * their IOs and/or Memory access, including VGA forwarding on P2P bridges if
 * necessary, so that the requested resources can be used. Then, the card is
 * marked as locking these resources and the IO and/or Memory accesses are
 * enabled on the card (including VGA forwarding on parent P2P bridges if any).
 *
 * This function will block if some conflicting card is already locking one of
 * the required resources (or any resource on a different bus segment, since P2P
 * bridges don't differentiate VGA memory and IO afaik). You can indicate
 * whether this blocking should be interruptible by a signal (for userland
 * interface) or not.
 *
 * Must not be called at interrupt time or in atomic context.  If the card
 * already owns the resources, the function succeeds.  Nested calls are
 * supported (a per-resource counter is maintained)
 *
 * On success, release the VGA resource again with vga_put().
 *
 * Returns:
 *
 * 0 on success, negative error code on failure.
 */
/*
 * [한국어]
 * vga_get - VGA 자원을 확보하고 잠근다 (필요하면 잠들어 기다린다)
 *
 * @pdev: 자원을 원하는 카드. NULL 이면 시스템 기본 VGA 장치를 대신 쓴다.
 * @rsrc: 원하는 자원 비트마스크(VGA_RSRC_* 조합).
 * @interruptible: 0 이 아니면 대기 중 시그널에 깨어나 -ERESTARTSYS 로 빠진다.
 *   유저스페이스 경로에서는 참을, 커널 내부 경로에서는 보통 거짓을 쓴다.
 *   vgaarb.h 의 인라인 vga_get_interruptible()/vga_get_uninterruptible() 이
 *   각각 1 과 0 을 넘기는 얇은 껍데기다.
 * @return: 0 = 성공(중재 대상 장치가 없어 할 일이 없었던 경우도 0),
 *   -ENODEV = 그 장치가 중재 목록에 없음,
 *   -ERESTARTSYS = 대기 중 시그널을 받아 중단(유저스페이스가 시스템 콜을
 *     재시작하게 된다), 그 밖의 음수 = pci_set_vga_state() 실패.
 *
 * 왜 필요한가: __vga_tryget() 은 "지금 안 되면 충돌 카드를 알려 준다"까지만
 * 한다. 실제 사용자는 자원이 날 때까지 기다리고 싶으므로, 이 함수가 그 위에
 * 재시도 루프와 대기열 잠들기를 얹는다.
 *
 * 동작 단계: (1) 첫 사용이면 클라이언트에게 알린다. (2) 대상 장치를 정한다.
 * (3) 무한 루프 안에서 vga_lock 을 잡고 __vga_tryget() 을 시도한다. 성공하면
 * 빠져나가고, 충돌이면 락을 놓은 뒤 대기열에 자신을 걸고 schedule() 로 잠든다.
 * 누군가 자원을 놓아 깨우면 루프 처음으로 돌아가 다시 시도한다.
 *
 * 실행 컨텍스트: **인터럽트 문맥이나 원자적 문맥에서 부르면 안 된다** -- 위
 * 상류 커널독의 "Must not be called at interrupt time or in atomic context" 가
 * 그 계약이다. schedule() 로 잠들기 때문이다. 잠들기 전에 반드시 vga_lock 을
 * 놓는다는 점이 중요하다 -- 스핀락을 쥔 채 잠들면 그 락을 기다리는 다른 CPU 가
 * 영원히 돌게 되어 시스템이 멈춘다.
 *
 * 중첩 호출이 지원된다(자원별 카운터). 성공했다면 반드시 같은 rsrc 로
 * vga_put() 을 불러 짝을 맞춰야 한다.
 *
 * 호출 체인:
 *   GPU 드라이버 / vga_arb_write()(vga_get_uninterruptible 경유)
 *     -> [vga_get] -> vga_check_first_use()
 *                  -> spin_lock_irqsave(vga_lock) -> vgadev_find()
 *                  -> __vga_tryget() -> pci_set_vga_state()
 *                  -> schedule() (충돌 시)
 */
int vga_get(struct pci_dev *pdev, unsigned int rsrc, int interruptible)
{
	/* [한국어] vgadev  -- 대상 카드의 중재 항목.
	 * conflict -- __vga_tryget() 이 돌려주는 값. NULL(성공) / 충돌 카드 / ERR_PTR
	 *   세 가지 의미를 겸하므로 IS_ERR() 로 먼저 걸러야 한다. */
	struct vga_device *vgadev, *conflict;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 플래그(EFLAGS 등)를 담는 자리.
	 * 락을 잡기 전의 지역 인터럽트 활성 상태를 보존했다가 unlock 때 복원한다. */
	unsigned long flags;
	/* [한국어] 이 태스크를 vga_wait_queue 에 매다는 대기 항목. 스택에 두는 것이
	 * 안전한 이유는, 잠에서 깬 뒤 이 함수를 벗어나기 전에 반드시
	 * remove_wait_queue() 로 떼어 내기 때문이다. */
	wait_queue_entry_t wait;
	/* [한국어] 최종 반환값. 0(성공)으로 시작해, 실패 경로에서만 바뀐다. */
	int rc = 0;

	/* [한국어] 중재가 실제로 쓰이기 시작하는 첫 순간을 감지해 GPU 클라이언트에게
	 * 알린다. vga_lock 을 잡기 **전에** 불러야 한다 -- 그 안에서 다시 vga_lock 을
	 * 잡기 때문이다. */
	vga_check_first_use();
	/* The caller should check for this, but let's be sure */
	/* [한국어] 호출자가 NULL 을 넘겼다면 "시스템 기본 카드"를 뜻한다.
	 * 위 영어 주석대로 호출자가 챙겨야 할 일이지만 방어적으로 여기서 처리한다. */
	if (pdev == NULL)
		pdev = vga_default_device();
	/* [한국어] 기본 장치조차 없다면(예: VGA 카드가 아예 없는 시스템) 중재할
	 * 대상이 없다. 오류가 아니라 성공(0)으로 돌아간다 -- 호출자는 짝이 되는
	 * vga_put() 을 그대로 불러도 되고, 그쪽도 같은 방식으로 조용히 빠진다. */
	if (pdev == NULL)
		return 0;

	/* [한국어] 재시도 루프. 자원을 얻거나 오류가 날 때까지 반복한다.
	 * 빠져나가는 길은 네 개의 break 뿐이다. */
	for (;;) {
		/* [한국어] 중재 상태 전체를 지키는 스핀락을 잡는다. irqsave 를 쓰는 이유는
		 * 이 락을 잡는 다른 경로가 인터럽트를 막은 문맥에서도 불릴 수 있기 때문이며,
		 * 같은 CPU 에서 인터럽트가 끼어들어 같은 락을 다시 잡으면 데드락이다. */
		spin_lock_irqsave(&vga_lock, flags);
		/* [한국어] 락을 잡은 뒤에 찾는다. 락 밖에서 찾아 두면 그 사이 장치가
		 * 제거돼 포인터가 허상이 될 수 있다. 그래서 매 재시도마다 다시 찾는다. */
		vgadev = vgadev_find(pdev);
		/* [한국어] 이 PCI 장치는 중재 대상이 아니다(VGA 클래스가 아니거나 이미
		 * 제거됨). 기다려 봐야 달라질 것이 없으므로 루프를 끝낸다. */
		if (vgadev == NULL) {
			/* [한국어] break 로 나가기 전에 반드시 락을 푼다. 이 줄이 없으면
			 * 스핀락을 쥔 채 함수를 벗어나 시스템이 멈춘다. */
			spin_unlock_irqrestore(&vga_lock, flags);
			/* [한국어] -ENODEV = 그런 장치가 없다. */
			rc = -ENODEV;
			break;
		}
		/* [한국어] 실제 확보 시도. 락 안에서만 부를 수 있다. */
		conflict = __vga_tryget(vgadev, rsrc);
		/* [한국어] 결과 판정 전에 락부터 푼다. 아래 대기 경로가 schedule() 을
		 * 부르므로, 여기서 놓지 않으면 스핀락을 쥔 채 잠드는 치명적 오류가 된다.
		 * conflict 포인터를 락 밖에서 쓰지만 NULL 여부와 IS_ERR 판정에만 쓰고
		 * 역참조하지 않으므로 안전하다. */
		spin_unlock_irqrestore(&vga_lock, flags);
		/* [한국어] ERR_PTR 로 감싼 오류인가 -- pci_set_vga_state() 가 실패한 경우다.
		 * 이 검사를 conflict == NULL 검사보다 먼저 해야 한다. ERR_PTR 값은 NULL 이
		 * 아니므로, 순서를 바꾸면 오류를 "충돌"로 오인해 영원히 재시도하게 된다. */
		if (IS_ERR(conflict)) {
			/* [한국어] 포인터에 실려 온 음수 오류 코드를 꺼내 반환값으로 삼는다. */
			rc = PTR_ERR(conflict);
			break;
		}
		/* [한국어] NULL = 충돌 없이 확보 성공. 루프를 끝내고 0 을 돌려준다. */
		if (conflict == NULL)
			break;

		/*
		 * We have a conflict; we wait until somebody kicks the
		 * work queue. Currently we have one work queue that we
		 * kick each time some resources are released, but it would
		 * be fairly easy to have a per-device one so that we only
		 * need to attach to the conflicting device.
		 */
		/* [한국어] 현재 태스크(current)를 가리키는 대기 항목을 초기화한다.
		 * 기본 깨우기 함수가 붙어, wake_up_all() 이 이 태스크를 실행 가능 상태로 만든다. */
		init_waitqueue_entry(&wait, current);
		/* [한국어] 전역 대기열에 자신을 매단다. 이 시점부터 wake_up_all() 의
		 * 대상이 된다. 반드시 잠들기 **전에** 매달아야 한다 -- 순서를 바꾸면
		 * 매달기 직전에 온 깨우기를 놓쳐 영원히 잠드는 lost wakeup 이 된다. */
		add_wait_queue(&vga_wait_queue, &wait);
		/* [한국어] 태스크 상태를 대기 상태로 바꾼다. interruptible 이면
		 * TASK_INTERRUPTIBLE(시그널에 깨어남), 아니면 TASK_UNINTERRUPTIBLE(시그널을
		 * 무시하고 자원이 날 때까지만 깨어남)이다. schedule() 을 부르기 전에 상태를
		 * 먼저 바꿔야, 그 사이에 온 깨우기가 상태를 RUNNING 으로 되돌려 schedule() 이
		 * 곧바로 반환되게 만든다(역시 lost wakeup 방지). */
		set_current_state(interruptible ?
				  TASK_INTERRUPTIBLE :
				  TASK_UNINTERRUPTIBLE);
		/* [한국어] 인터럽트 가능 대기인데 이미 처리해야 할 시그널이 와 있다면,
		 * 잠들지 말고 곧바로 빠져나간다. 잠들었다가 깨는 것보다 낫고, 무엇보다
		 * 시그널이 이미 와 있으면 schedule() 이 즉시 반환해 바쁜 루프가 된다. */
		if (interruptible && signal_pending(current)) {
			/* [한국어] 대기 상태를 되돌려 이 태스크를 다시 실행 가능으로 만든다.
			 * __ 접두사는 메모리 배리어 없이 상태만 쓰는 가벼운 버전이라는 뜻이며,
			 * 이미 실행 중인 자기 자신의 상태를 되돌리는 자리라 배리어가 필요 없다. */
			__set_current_state(TASK_RUNNING);
			/* [한국어] 대기열에서 자신을 뗀다. 스택에 있는 wait 가 함수와 함께
			 * 사라지므로, 떼지 않으면 대기열이 사라진 메모리를 가리키게 된다. */
			remove_wait_queue(&vga_wait_queue, &wait);
			/* [한국어] -ERESTARTSYS = "시그널 때문에 중단했으니 시스템 콜을 재시작하라".
			 * 시그널 핸들러가 SA_RESTART 로 설정돼 있으면 커널이 자동으로 재시도한다. */
			rc = -ERESTARTSYS;
			break;
		}
		/* [한국어] 여기서 실제로 잠든다. 누군가 __vga_put() 이나
		 * vga_arbiter_del_pci_device() 에서 wake_up_all() 을 부를 때까지 CPU 를 놓는다. */
		schedule();
		/* [한국어] 깨어났으니 대기열에서 자신을 뗀다. 그리고 루프 처음으로 돌아가
		 * 다시 __vga_tryget() 을 시도한다 -- 전역 대기열이라 나와 무관한 자원이
		 * 풀렸을 수도 있으므로 반드시 재확인해야 한다. */
		remove_wait_queue(&vga_wait_queue, &wait);
	}
	/* [한국어] 0 이면 자원을 확보했다는 뜻이고, 호출자는 나중에 vga_put() 으로
	 * 짝을 맞춰야 한다. 음수면 아무것도 잠기지 않았으므로 put 을 부르면 안 된다. */
	return rc;
}
/* [한국어] 모듈에서 쓸 수 있도록 내보낸다. GPU 드라이버들이 레거시 VGA
 * 레지스터를 만지기 직전에 이 함수를 부른다. */
EXPORT_SYMBOL(vga_get);

/**
 * vga_tryget - try to acquire & lock legacy VGA resources
 * @pdev: PCI device of VGA card or NULL for system default
 * @rsrc: bit mask of resources to acquire and lock
 *
 * Perform the same operation as vga_get(), but return an error (-EBUSY)
 * instead of blocking if the resources are already locked by another card.
 * Can be called in any context.
 *
 * On success, release the VGA resource again with vga_put().
 *
 * Returns:
 *
 * 0 on success, negative error code on failure.
 */
/* [한국어]
 * vga_tryget - 블로킹 없이 VGA 자원 확보를 시도한다
 *
 * @pdev: 대상 카드. NULL 이면 기본 VGA 장치.
 * @rsrc: 원하는 자원 비트마스크.
 * @return: 0 = 확보 성공(또는 중재할 장치가 없음), -ENODEV = 중재 목록에 없음,
 *   -EBUSY = 다른 카드가 그 자원을 잠그고 있어 지금은 줄 수 없음.
 *
 * 왜 필요한가: vga_get() 은 자원이 날 때까지 잠들지만, 잠들 수 없는 문맥이나
 * "지금 안 되면 그만두겠다"는 정책이 필요한 곳도 있다. 위 상류 커널독이
 * "Can be called in any context" 라고 밝힌 것이 이 함수의 존재 이유다.
 *
 * vga_get() 과의 차이는 딱 하나 -- 충돌이 나면 기다리지 않고 -EBUSY 로 돌아선다.
 * 그래서 대기열도, schedule() 도, 재시도 루프도 없다.
 *
 * 실행 컨텍스트: 어떤 문맥에서도 가능하다. 임계 구역 전체가 하나의
 * spin_lock_irqsave 구간이며 그 안에서 잠들지 않는다.
 *
 * static 인 이유: 커널 외부에 노출하지 않는다. 유일한 호출자는 아래
 * vga_arb_write() 의 "trylock" 명령 처리다.
 *
 * 주의: 반환값 관례가 유저스페이스 쪽 호출부와 어긋나 보인다 -- 이 함수는
 * 성공 시 0 을 돌려주는데, vga_arb_write() 는 `if (vga_tryget(...))` 가
 * 참일 때를 성공으로 취급한다. 코드에 있는 그대로 적어 두며, 그 지점의
 * 주석에서 다시 언급한다.
 *
 * 호출 체인:
 *   vga_arb_write()("trylock") -> [vga_tryget] -> __vga_tryget()
 */
static int vga_tryget(struct pci_dev *pdev, unsigned int rsrc)
{
	/* [한국어] 대상 카드의 중재 항목. 락 안에서만 유효하다. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 반환값. 성공 경로에서는 0 그대로 나간다. */
	int rc = 0;

	/* [한국어] vga_get() 과 마찬가지로 첫 사용을 감지해 알린다. 이 호출도
	 * 반드시 vga_lock 을 잡기 전에 있어야 한다. */
	vga_check_first_use();

	/* The caller should check for this, but let's be sure */
	/* [한국어] NULL 은 "기본 카드"를 뜻한다. */
	if (pdev == NULL)
		pdev = vga_default_device();
	/* [한국어] 기본 카드조차 없으면 중재할 것이 없으므로 성공으로 돌아간다. */
	if (pdev == NULL)
		return 0;
	/* [한국어] 여기서부터 아래 unlock 까지가 임계 구역이다. vga_get() 과 달리
	 * 중간에 락을 놓는 지점이 없어, 전체가 원자적으로 처리된다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 락 안에서 중재 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] 중재 대상이 아닌 장치. goto bail 로 락 해제 지점으로 모은다 --
	 * 여러 탈출 경로가 있어도 unlock 을 한 곳에만 두어 빠뜨리지 않게 하는 관용구다. */
	if (vgadev == NULL) {
		rc = -ENODEV;
		goto bail;
	}
	/* [한국어] __vga_tryget() 이 NULL 이 아닌 것을 돌려주면 확보 실패다.
	 * 주의: 여기서는 충돌 카드 포인터와 ERR_PTR 을 구분하지 않는다. 즉
	 * pci_set_vga_state() 가 실패한 경우도 -EBUSY 로 뭉뚱그려진다.
	 * vga_get() 은 IS_ERR() 로 구분하는데 이쪽은 하지 않는다는 비대칭이 있으며,
	 * 그 이유를 밝힌 근거는 이 트리 안에 없다. */
	if (__vga_tryget(vgadev, rsrc))
		rc = -EBUSY;
/* [한국어] 성공/실패 모든 경로가 모이는 지점. 여기서 락을 한 번만 푼다. */
bail:
	/* [한국어] 스핀락 해제와 함께 인터럽트 상태를 잡기 전으로 복원한다. */
	spin_unlock_irqrestore(&vga_lock, flags);
	/* [한국어] 0(성공) / -ENODEV / -EBUSY 중 하나. */
	return rc;
}

/**
 * vga_put - release lock on legacy VGA resources
 * @pdev: PCI device of VGA card or NULL for system default
 * @rsrc: bit mask of resource to release
 *
 * Release resources previously locked by vga_get() or vga_tryget().  The
 * resources aren't disabled right away, so that a subsequent vga_get() on
 * the same card will succeed immediately.  Resources have a counter, so
 * locks are only released if the counter reaches 0.
 */
/* [한국어]
 * vga_put - vga_get()/vga_tryget() 으로 얻은 잠금을 반납한다
 *
 * @pdev: 반납할 카드. NULL 이면 기본 VGA 장치.
 * @rsrc: 반납할 자원 비트마스크. 얻을 때 쓴 값과 짝이 맞아야 한다.
 * @return: 없음. 반납은 실패하지 않는다 -- 장치를 못 찾아도 조용히 빠진다.
 *
 * 왜 필요한가: vga_get() 이 올린 중첩 카운터를 내리는 공개 API 다. 위 상류
 * 커널독이 밝히듯 자원을 즉시 끄지는 않는다(게으른 해제). 카운터가 0 이 되어야
 * 잠금이 풀리고, 그제서야 다른 카드가 그 자원을 빼앗아 갈 수 있게 된다.
 *
 * 여기서는 vga_check_first_use() 를 부르지 않는다는 점이 vga_get()/vga_tryget()
 * 과 다르다. 반납은 "중재의 첫 사용"이 될 수 없기 때문이다 -- 반납할 것이
 * 있다는 말은 이미 얻은 적이 있다는 뜻이다.
 *
 * 실행 컨텍스트: 임계 구역 전체가 스핀락 하나이고 그 안에서 잠들지 않으므로
 * 어떤 문맥에서도 부를 수 있다. 실제로 vga_arb_release() 가 파일이 닫힐 때
 * 반복 호출한다.
 *
 * 에러 경로: 장치를 못 찾으면 goto bail 로 조용히 나간다. 이미 뽑힌 카드에
 * 대한 반납이 늦게 도착하는 경우가 정상적으로 일어나기 때문이다.
 *
 * 호출 체인:
 *   GPU 드라이버 / vga_arb_write()("unlock") / vga_arb_release()
 *     -> [vga_put] -> spin_lock_irqsave(vga_lock) -> vgadev_find() -> __vga_put()
 */
void vga_put(struct pci_dev *pdev, unsigned int rsrc)
{
	/* [한국어] 대상 카드의 중재 항목. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;

	/* The caller should check for this, but let's be sure */
	/* [한국어] NULL 은 "기본 카드"를 뜻한다. */
	if (pdev == NULL)
		pdev = vga_default_device();
	/* [한국어] 기본 카드조차 없으면 반납할 것도 없다. void 함수이므로 그냥 나간다. */
	if (pdev == NULL)
		return;
	/* [한국어] 카운터와 잠금 비트를 고치므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 락 안에서 중재 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] 이미 제거된 장치에 대한 반납이면 할 일이 없다. 오류로 보지 않는
	 * 이유는, 장치 제거(vga_arbiter_del_pci_device)가 잠금 상태와 무관하게
	 * 항목을 없애 버리므로 이런 뒤늦은 반납이 정상적으로 발생하기 때문이다. */
	if (vgadev == NULL)
		goto bail;
	/* [한국어] 실제 반납 처리. 카운터를 내리고 0 이 되면 잠금을 풀며 대기자를 깨운다. */
	__vga_put(vgadev, rsrc);
/* [한국어] 두 경로가 모이는 락 해제 지점. */
bail:
	/* [한국어] 스핀락 해제와 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&vga_lock, flags);
}
/* [한국어] vga_get() 과 짝을 이루어 모듈에 내보낸다. */
EXPORT_SYMBOL(vga_put);

/*
 * [한국어]
 * vga_is_firmware_default - 펌웨어가 부팅 화면을 띄우던 장치인지 판정한다
 *
 * @pdev: 검사할 PCI 장치.
 * @return: true = 이 장치가 펌웨어(BIOS/UEFI) 프레임버퍼의 주인이다.
 *   false = 아니거나, x86 이 아니라 판단할 근거가 없다.
 *
 * 왜 필요한가: "부팅 콘솔을 담당할 카드"를 고를 때 가장 믿을 만한 신호는
 * "펌웨어가 실제로 화면을 띄우고 있던 카드"다. 사용자가 BIOS 설정으로 주
 * 디스플레이를 지정했다면 그 결정이 여기에 반영돼 있다. 그래서
 * vga_is_boot_device() 의 우선순위 목록에서 최상위를 차지한다.
 *
 * 동작: sysfb_primary_display.screen 은 펌웨어가 넘겨준 프레임버퍼 서술자
 * (struct screen_info)이고, screen_info_pci_dev() 가 그 안의 프레임버퍼 주소를
 * 어느 PCI 장치의 자원이 품고 있는지 되짚어 pci_dev 를 찾아 준다. 그 결과와
 * 후보 장치를 포인터로 비교한다.
 *
 * #if 분기의 이유: sysfb 의 primary display 정보는 x86 의 부팅 프로토콜에서
 * 오므로, 다른 아키텍처에서는 이 판정 자체가 성립하지 않아 무조건 false 다.
 * (drivers/pci/Kconfig 의 VGA_ARB 가 `select SCREEN_INFO if X86` 인 것과
 * 짝을 이룬다.)
 *
 * 실행 컨텍스트: vga_is_boot_device() 안에서만 불리므로 vga_lock 을 쥔 상태다.
 * 락을 잡거나 잠들지 않는다.
 *
 * 호출 체인:
 *   vga_arbiter_add_pci_device() -> vga_is_boot_device()
 *     -> [vga_is_firmware_default] -> screen_info_pci_dev()
 */
static bool vga_is_firmware_default(struct pci_dev *pdev)
{
/* [한국어] x86 에서만 펌웨어 프레임버퍼 정보를 신뢰할 수 있다. */
#if defined CONFIG_X86
	/* [한국어] 펌웨어가 쓰던 프레임버퍼의 주인 장치와 같은지 포인터로 비교한다.
	 * screen_info_pci_dev() 는 참조를 잡아 돌려주지 않는 조회 함수이며, 여기서는
	 * 동일성 비교에만 쓰고 저장하지 않는다. */
	return pdev == screen_info_pci_dev(&sysfb_primary_display.screen);
/* [한국어] x86 이 아닌 아키텍처 -- 펌웨어 프레임버퍼를 PCI 장치로 되짚을
 * 방법이 없으므로 "펌웨어 기본 장치 아님"으로 답한다. 그러면 아래 우선순위
 * 규칙(레거시 VGA -> 내장 -> 외장)만으로 기본 장치를 고르게 된다. */
#else
	return false;
#endif
}

/*
 * [한국어]
 * vga_arb_integrated_gpu - 이 장치가 내장(integrated) GPU 인지 ACPI 로 판정한다
 *
 * @dev: 검사할 장치의 struct device 포인터. 호출자가 &pdev->dev 를 넘긴다.
 * @return: true = 내장 GPU 로 보인다. false = 아니거나 ACPI 가 없어 알 수 없다.
 *
 * 왜 필요한가: 레거시 VGA 장치를 못 찾았을 때, 내장 GPU 를 외장 GPU 보다
 * 먼저 기본 장치로 삼기 위해서다. 노트북처럼 내장/외장이 함께 있는 시스템에서
 * 화면이 실제로 연결된 쪽은 대개 내장 GPU 이기 때문이다.
 *
 * 동작: 펌웨어가 ACPI 네임스페이스에서 이 PCI 장치에 붙여 둔 ACPI 장치 노드를
 * ACPI_COMPANION() 으로 얻고, 그 하드웨어 ID(_HID)가 ACPI_VIDEO_HID 와 같은지
 * 본다. ACPI_VIDEO_HID 는 include/acpi/acpi_drivers.h 에서 "LNXVIDEO" 로
 * 정의된 리눅스 내부 식별자다 -- 즉 ACPI 표준의 벤더 ID 가 아니라, ACPI 비디오
 * 확장(_DOS/_DOD 등 백라이트/출력 제어 메서드)을 갖춘 노드에 커널이 붙이는
 * 꼬리표다. 그런 노드가 붙어 있다는 것이 곧 "플랫폼이 이 GPU 를 시스템 내장
 * 디스플레이 장치로 취급한다"는 신호다.
 *
 * #if 분기의 이유: ACPI 가 없는 커널(임베디드, 일부 아키텍처)에서는
 * ACPI_COMPANION 자체가 없으므로 판정할 수 없어 false 다.
 *
 * 실행 컨텍스트: vga_is_boot_device() 안, 즉 vga_lock 을 쥔 상태에서 불린다.
 * strcmp 와 포인터 역참조뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   vga_is_boot_device() -> [vga_arb_integrated_gpu]
 *     -> ACPI_COMPANION() -> acpi_device_hid()
 */
static bool vga_arb_integrated_gpu(struct device *dev)
{
/* [한국어] ACPI 를 쓰는 커널에서만 이 판정이 가능하다. */
#if defined(CONFIG_ACPI)
	/* [한국어] PCI 장치에 대응하는 ACPI 네임스페이스 노드를 얻는다. 펌웨어가
	 * 그 장치를 ACPI 로 서술하지 않았다면 NULL 이다. */
	struct acpi_device *adev = ACPI_COMPANION(dev);

	/* [한국어] adev 가 NULL 이 아닐 때만 _HID 를 읽어 "LNXVIDEO" 와 비교한다.
	 * && 의 단축 평가 덕분에 NULL 역참조가 일어나지 않는다.
	 * strcmp 가 0 이면 같다는 뜻이라 ! 를 붙여 true 로 뒤집는다. */
	return adev && !strcmp(acpi_device_hid(adev), ACPI_VIDEO_HID);
/* [한국어] ACPI 가 없는 빌드 -- 내장/외장을 구분할 근거가 없으므로 false.
 * 그러면 vga_is_boot_device() 는 "처음 찾은 비레거시 장치"를 쓰게 된다. */
#else
	return false;
#endif
}

/*
 * Return true if vgadev is a better default VGA device than the best one
 * we've seen so far.
 */
/*
 * [한국어]
 * vga_is_boot_device - 이 카드가 지금까지의 후보보다 더 나은 기본 VGA 장치인가
 *
 * @vgadev: 방금 등록 중인 카드의 중재 항목. owns 는 이미 COMMAND 레지스터와
 *   브리지 상태를 반영해 채워져 있어야 한다(호출자가 그 순서를 지킨다).
 * @return: true = 이 카드를 새 기본 장치로 삼아야 한다.
 *   false = 기존 기본 장치가 더 낫거나 같다.
 *
 * 왜 필요한가: "기본 VGA 장치"는 부팅 콘솔(vgacon)이 쓰는 카드이고, 유저스페이스가
 * /dev/vga_arbiter 를 열었을 때의 초기 target 이기도 하다. 카드가 여러 장이면
 * 그중 하나를 골라야 하는데, 장치는 열거 순서대로 하나씩 들어오므로 "지금까지
 * 최선"과 "새로 온 후보"를 비교하는 형태로 점진적으로 고른다.
 *
 * 우선순위(위 상류 주석에 적힌 순서 그대로):
 *   1. 펌웨어 프레임버퍼의 주인 -- 가장 확실한 신호.
 *   2. 레거시 VGA 장치 -- IO 와 MEM 을 둘 다 소유(owns 에 LEGACY_MASK 전부).
 *      즉 경로상 브리지의 VGA Enable 이 모두 켜져 레거시 주소가 실제로 이
 *      카드까지 도달하는 상태다.
 *   3. 비레거시 내장 GPU -- ACPI 로 판정.
 *   4. 비레거시 외장 GPU -- 처음 찾은 것.
 *   5. 그 밖 -- 아무것도 아니면 false.
 *
 * 확인된 사실: 위 상류 주석이 네 번 언급하는 vga_arb_select_default_device()
 * 라는 함수는 이 트리 전체에 존재하지 않는다(git grep 으로 전수 확인했고,
 * 이 파일의 그 주석 네 줄이 유일한 등장이다). 과거에 있다가 사라진 함수를
 * 가리키는 낡은 주석으로 보인다. 상류 주석은 규칙대로 손대지 않는다.
 *
 * 실행 컨텍스트: **vga_lock 을 쥔 채** 불린다(vga_arbiter_add_pci_device 의
 * 임계 구역 안). 그 안에서 pci_read_config_word() 로 config space 를 읽으므로,
 * 스핀락을 쥔 채 하드웨어 접근이 일어난다. 잠들지는 않는다.
 *
 * 에러 경로: 없다. pci_read_config_word() 가 실패해도 반환값을 검사하지 않으며,
 * 그 경우 cmd 는 초기화되지 않은 값으로 남는다 -- 코드에 있는 그대로 적어 둔다.
 *
 * 호출 체인:
 *   vga_arbiter_add_pci_device() -> [vga_is_boot_device]
 *     -> vgadev_find() / vga_is_firmware_default() / vga_arb_integrated_gpu()
 *     -> pci_read_config_word(PCI_COMMAND)
 */
static bool vga_is_boot_device(struct vga_device *vgadev)
{
	/* [한국어] 지금까지의 챔피언. 현재 기본 장치의 중재 항목을 찾아 둔다.
	 * 아직 기본 장치가 없으면 vga_default_device() 가 NULL 을 돌려주고,
	 * vgadev_find(NULL) 도 NULL 을 돌려주므로 boot_vga 는 NULL 이 된다 --
	 * "비교 대상이 없다 = 이번 후보가 자동으로 낫다"는 뜻으로 아래에서 쓰인다. */
	struct vga_device *boot_vga = vgadev_find(vga_default_device());
	/* [한국어] 후보 카드의 pci_dev. config space 읽기와 ACPI 조회에 쓴다. */
	struct pci_dev *pdev = vgadev->pdev;
	/* [한국어] cmd      -- 후보 카드의 PCI COMMAND 레지스터(오프셋 0x04) 값.
	 * boot_cmd -- 현 챔피언의 COMMAND 값. 4단계 비교에서만 쓴다.
	 * u16 인 이유는 COMMAND 가 16비트 레지스터이기 때문이다. */
	u16 cmd, boot_cmd;

	/*
	 * We select the default VGA device in this order:
	 *   Firmware framebuffer (see vga_arb_select_default_device())
	 *   Legacy VGA device (owns VGA_RSRC_LEGACY_MASK)
	 *   Non-legacy integrated device (see vga_arb_select_default_device())
	 *   Non-legacy discrete device (see vga_arb_select_default_device())
	 *   Other device (see vga_arb_select_default_device())
	 */

	/*
	 * We always prefer a firmware default device, so if we've already
	 * found one, there's no need to consider vgadev.
	 */
	/* [한국어] 1단계 -- 이미 펌웨어 기본 장치를 찾았다면 그보다 나은 것은 없다.
	 * 더 볼 것 없이 거절한다. */
	if (boot_vga && boot_vga->is_firmware_default)
		return false;

	/* [한국어] 이번 후보가 펌웨어 프레임버퍼의 주인인가. 그렇다면 최상위
	 * 우선순위이므로 다른 조건을 볼 필요가 없다. */
	if (vga_is_firmware_default(pdev)) {
		/* [한국어] 그 사실을 항목에 기록해 둔다. 다음 카드가 심사될 때 위 1단계가
		 * 이 플래그를 보고 곧바로 거절할 수 있게 하는 표시다. */
		vgadev->is_firmware_default = true;
		return true;
	}

	/*
	 * A legacy VGA device has MEM and IO enabled and any bridges
	 * leading to it have PCI_BRIDGE_CTL_VGA enabled so the legacy
	 * resources ([mem 0xa0000-0xbffff], [io 0x3b0-0x3bb], etc) are
	 * routed to it.
	 *
	 * We use the first one we find, so if we've already found one,
	 * vgadev is no better.
	 */
	/* [한국어] 2단계 -- 현 챔피언이 이미 "레거시 VGA 장치"라면 그것을 지킨다.
	 * 판정식이 `& MASK == MASK` 인 이유: IO 와 MEM 을 **둘 다** 소유해야 진짜
	 * 레거시 VGA 다. 하나만 켜져 있으면 레거시 주소 전체에 응답하지 못한다.
	 * 위 상류 주석대로, 이 상태는 경로상 모든 브리지의 PCI_BRIDGE_CTL_VGA 가
	 * 켜져 있어 레거시 사이클이 그 카드까지 도달함을 뜻한다. */
	if (boot_vga &&
	    (boot_vga->owns & VGA_RSRC_LEGACY_MASK) == VGA_RSRC_LEGACY_MASK)
		return false;

	/* [한국어] 이번 후보가 레거시 VGA 장치인가. 챔피언이 없거나 챔피언이
	 * 레거시가 아닐 때 여기 도달하므로, 참이면 곧바로 승격시킨다. */
	if ((vgadev->owns & VGA_RSRC_LEGACY_MASK) == VGA_RSRC_LEGACY_MASK)
		return true;

	/*
	 * If we haven't found a legacy VGA device, accept a non-legacy
	 * device.  It may have either IO or MEM enabled, and bridges may
	 * not have PCI_BRIDGE_CTL_VGA enabled, so it may not be able to
	 * use legacy VGA resources.  Prefer an integrated GPU over others.
	 */
	/* [한국어] 3~4단계로 넘어간다. 후보의 COMMAND 레지스터를 읽어 IO/MEM
	 * 디코딩이 하나라도 켜져 있는지 본다. PCI_COMMAND 는 config space 오프셋 0x04,
	 * bit 0 = I/O Space Enable, bit 1 = Memory Space Enable 이다.
	 * 반환값을 검사하지 않으므로 읽기가 실패하면 cmd 는 미초기화 값이 된다. */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	/* [한국어] 둘 중 하나라도 켜져 있으면 "쓸 수 있는 비레거시 장치"로 본다.
	 * 레거시 VGA 처럼 둘 다일 필요는 없다 -- 위 상류 주석이 밝히듯 브리지의
	 * VGA Enable 이 꺼져 있어 레거시 자원을 못 쓰는 장치도 여기 해당한다. */
	if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {

		/*
		 * An integrated GPU overrides a previous non-legacy
		 * device.  We expect only a single integrated GPU, but if
		 * there are more, we use the *last* because that was the
		 * previous behavior.
		 */
		/* [한국어] 3단계 -- 내장 GPU 는 앞서 뽑힌 비레거시 장치를 무조건 덮어쓴다.
		 * 위 상류 주석이 밝히듯 내장 GPU 가 여러 개면 **마지막** 것이 이기는데,
		 * 조건 없이 true 를 돌려주기 때문이다(과거 동작과의 호환을 위한 선택). */
		if (vga_arb_integrated_gpu(&pdev->dev))
			return true;

		/*
		 * We prefer the first non-legacy discrete device we find.
		 * If we already found one, vgadev is no better.
		 */
		/* [한국어] 4단계 -- 비레거시 외장 장치는 "처음 찾은 것"이 이긴다.
		 * 챔피언이 있다면 그것이 이미 비레거시 후보인지 확인해야 한다. */
		if (boot_vga) {
			/* [한국어] 챔피언의 COMMAND 를 읽는다. 여기까지 왔다는 것은 챔피언이
			 * 레거시 VGA 도, 펌웨어 기본 장치도 아니라는 뜻이므로, 남은 판정 기준은
			 * "IO/MEM 이 켜져 있는가" 하나뿐이다. */
			pci_read_config_word(boot_vga->pdev, PCI_COMMAND,
					     &boot_cmd);
			/* [한국어] 챔피언도 이미 쓸 만한 비레거시 장치라면 이번 후보는 나을 게
			 * 없다. "처음 찾은 것을 쓴다"는 규칙이 여기서 구현된다. */
			if (boot_cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY))
				return false;
		}
		/* [한국어] 챔피언이 없거나, 있어도 IO/MEM 이 모두 꺼진 쓸모없는 장치라면
		 * 이번 후보가 낫다. */
		return true;
	}

	/* [한국어] 5단계 -- IO 도 MEM 도 꺼져 있는 장치. 화면을 낼 수 없으므로
	 * 기본 장치 후보가 되지 못한다. */
	return false;
}

/*
 * Rules for using a bridge to control a VGA descendant decoding: if a bridge
 * has only one VGA descendant then it can be used to control the VGA routing
 * for that device. It should always use the bridge closest to the device to
 * control it. If a bridge has a direct VGA descendant, but also have a sub-
 * bridge VGA descendant then we cannot use that bridge to control the direct
 * VGA descendant. So for every device we register, we need to iterate all
 * its parent bridges so we can invalidate any devices using them properly.
 */
/*
 * [한국어]
 * vga_arbiter_check_bridge_sharing - 브리지 하나로 이 카드를 제어할 수 있는지 판정
 *
 * @vgadev: 방금 만들어진, 아직 vga_list 에 넣지 않은 새 카드의 중재 항목.
 *   호출자가 목록에 넣기 **전에** 부르는 것이 중요하다 -- 그래야 아래 순회에서
 *   자기 자신과 비교하는 일이 없다.
 * @return: 없음. 결과는 vgadev->bridge_has_one_vga 와, 필요하면 기존 카드들의
 *   같은 필드에 기록된다.
 *
 * 왜 필요한가: 브리지의 VGA Enable 비트는 그 브리지 아래 **전체**에 대한
 * 스위치다. 어떤 브리지 아래에 VGA 장치가 하나뿐이면 그 비트만으로 그 장치의
 * 레거시 라우팅을 껐다 켤 수 있어 편하다(장치의 COMMAND 를 건드리지 않아도 되니
 * 그 장치의 정상 BAR 접근이 살아 있다). 그러나 둘 이상이 매달려 있으면 그
 * 브리지를 끄는 순간 애먼 카드까지 함께 꺼지므로, 브리지 대신 각 장치의
 * COMMAND 비트를 써야 한다. 이 함수가 그 구분을 미리 계산해 둔다.
 *
 * 위 상류 주석이 규칙을 요약한다 -- 브리지에 VGA 자손이 하나뿐이면 그 브리지로
 * 그 장치를 제어할 수 있고, 장치에 가장 가까운 브리지를 써야 하며, 직계 VGA
 * 자손과 하위 브리지 VGA 자손을 동시에 가진 브리지는 쓸 수 없다. 그래서 장치를
 * 등록할 때마다 조상 브리지 전체를 훑어 영향을 받는 기존 장치들을 무효화한다.
 *
 * 동작 단계(이중 루프):
 *  바깥 -- 새 장치의 조상 버스를 위로 훑으며 각 단계의 브리지 new_bridge 를 본다.
 *  안쪽 -- 이미 등록된 모든 카드에 대해 두 가지를 검사한다.
 *    (a) 그 카드의 **직계** 브리지가 new_bridge 와 같으면, 그 브리지는 새 장치도
 *        커버하므로 기존 카드 쪽을 무효화한다.
 *    (b) 그 카드의 조상 브리지 사슬 어딘가에 **새 장치의 직계 브리지**가 있으면,
 *        새 장치의 직계 브리지가 기존 카드도 커버한다는 뜻이므로 새 장치 쪽을
 *        무효화한다.
 *
 * 복잡도: (새 장치의 조상 깊이) x (기존 카드 수) x (각 카드의 조상 깊이). VGA
 * 카드가 몇 개뿐이고 PCI 계층이 얕아 문제되지 않는다.
 *
 * 실행 컨텍스트: **vga_lock 을 쥔 채** 불린다(vga_arbiter_add_pci_device 의
 * 임계 구역 안). 목록을 순회하고 다른 카드의 필드를 고치므로 락이 필수다.
 * config space 접근은 없고 이미 만들어진 버스 계층 구조만 따라간다.
 *
 * 에러 경로: 없다. 판정에 실패하는 경우가 없으며, 확신이 없으면 안전한 쪽
 * (false = 브리지 제어 불가)으로 떨어진다.
 *
 * 호출 체인:
 *   vga_arbiter_add_pci_device() -> [vga_arbiter_check_bridge_sharing]
 */
static void vga_arbiter_check_bridge_sharing(struct vga_device *vgadev)
{
	/* [한국어] 이미 등록된 카드들을 훑는 커서. 이름 그대로 "새 장치와 브리지를
	 * 공유하는지 검사할 대상"이다. */
	struct vga_device *same_bridge_vgadev;
	/* [한국어] new_bus -- 새 장치에서 위로 올라가는 바깥 루프의 현재 버스.
	 * bus     -- 기존 카드에서 위로 올라가는 안쪽 루프의 현재 버스. */
	struct pci_bus *new_bus, *bus;
	/* [한국어] new_bridge -- new_bus 를 만들어 낸 상위 브리지 장치(bus->self).
	 * bridge     -- 기존 카드 쪽 버스의 상위 브리지. 루트 버스면 둘 다 NULL 이다. */
	struct pci_dev *new_bridge, *bridge;

	/* [한국어] 낙관적으로 시작한다. 아래 검사에서 반례가 하나라도 나오면
	 * false 로 내린다. 이 초기화가 없으면 kzalloc 이 준 false 가 그대로 남아
	 * 항상 "브리지 제어 불가"로 동작하게 된다. */
	vgadev->bridge_has_one_vga = true;

	/* [한국어] 등록된 카드가 하나도 없으면 브리지를 공유할 상대도 없다.
	 * 곧바로 참으로 확정하고 빠진다. */
	if (list_empty(&vga_list)) {
		/* [한국어] 판정 결과를 dmesg 에 남긴다. 이 메시지는 아래 최종 판정에서
		 * 쓰는 문자열과 같아, 어느 경로로 결정됐든 로그 형식이 일정하다. */
		vgaarb_info(&vgadev->pdev->dev, "bridge control possible\n");
		return;
	}

	/* [한국어] 바깥 루프 준비 -- 새 장치가 꽂힌 버스에서 출발한다. */
	/* Iterate the new device's bridge hierarchy */
	new_bus = vgadev->pdev->bus;
	/* [한국어] 루트 버스에 닿을 때까지(parent 가 NULL 이 될 때까지) 위로 올라간다. */
	while (new_bus) {
		/* [한국어] 이 버스를 만들어 낸 상위 브리지. pci_bus 의 self 는 그 버스의
		 * "위쪽 끝"에 있는 브리지 장치를 가리키며, 루트 버스에서는 NULL 이다. */
		new_bridge = new_bus->self;

		/* Go through list of devices already registered */
		/* [한국어] 이미 등록된 모든 카드와 비교한다. 새 장치는 아직 목록에 없으므로
		 * 자기 자신과 비교될 위험이 없다(호출 순서가 그것을 보장한다). */
		list_for_each_entry(same_bridge_vgadev, &vga_list, list) {
			/* [한국어] 기존 카드가 꽂힌 버스. 아래 안쪽 while 루프의 출발점이기도 하다. */
			bus = same_bridge_vgadev->pdev->bus;
			/* [한국어] 기존 카드의 **직계** 상위 브리지. */
			bridge = bus->self;

			/* See if it shares a bridge with this device */
			/* [한국어] 검사 (a) -- 기존 카드의 직계 브리지가, 새 장치의 조상 브리지
			 * 사슬에 들어 있는 브리지와 같은가. 같다면 그 브리지 아래에 VGA 장치가 둘
			 * 이상(기존 카드 + 새 장치) 있다는 뜻이다.
			 * 주의: 둘 다 NULL 인 경우(양쪽 모두 루트 버스)도 같다고 판정된다. 루트
			 * 버스에는 제어할 브리지가 없으므로, 그때 무효화하는 것이 결과적으로 옳다. */
			if (new_bridge == bridge) {
				/*
				 * If its direct parent bridge is the same
				 * as any bridge of this device then it can't
				 * be used for that device.
				 */
				/* [한국어] 기존 카드 쪽을 무효화한다. 그 카드를 제어하려고 이 브리지를
				 * 끄면 새 장치까지 함께 꺼지기 때문이다. 이후 그 카드는 __vga_tryget() 에서
				 * COMMAND 비트로 제어된다. */
				same_bridge_vgadev->bridge_has_one_vga = false;
			}

			/*
			 * Now iterate the previous device's bridge hierarchy.
			 * If the new device's parent bridge is in the other
			 * device's hierarchy, we can't use it to control this
			 * device.
			 */
			/* [한국어] 검사 (b) -- 기존 카드의 조상 브리지 사슬을 루트까지 훑는다. */
			while (bus) {
				/* [한국어] 이 단계의 브리지. 루트에 닿으면 NULL 이 된다. */
				bridge = bus->self;

				/* [한국어] 기존 카드의 조상 사슬 어딘가에 **새 장치의 직계 브리지**가
				 * 있는가. 있다면 새 장치의 직계 브리지가 기존 카드까지 덮는다는 뜻이므로,
				 * 그 브리지로 새 장치만 골라 끌 수 없다.
				 * bridge 가 NULL 이 아닌지 먼저 보는 이유: 양쪽 다 루트 버스라 NULL == NULL 이
				 * 되는 경우를 여기서는 일치로 치지 않기 위해서다(검사 (a)와 다른 취급). */
				if (bridge && bridge == vgadev->pdev->bus->self)
					/* [한국어] 새 장치 쪽을 무효화한다. */
					vgadev->bridge_has_one_vga = false;

				/* [한국어] 한 단계 위 버스로. NULL 이 되면 루트에 닿은 것이라 루프가 끝난다. */
				bus = bus->parent;
			}
		}
		/* [한국어] 새 장치 쪽도 한 단계 위 버스로 올라간다. 이렇게 조상 전체를
		 * 훑어야, 위 상류 주석이 말하는 "모든 부모 브리지를 순회해 그것을 쓰는
		 * 장치들을 제대로 무효화한다"가 달성된다. */
		new_bus = new_bus->parent;
	}

	/* [한국어] 최종 판정을 dmesg 에 남긴다. 이 한 줄로 시스템이 어떤 방식으로
	 * 중재되는지(브리지 비트냐 COMMAND 비트냐)를 나중에 알 수 있다. */
	if (vgadev->bridge_has_one_vga)
		vgaarb_info(&vgadev->pdev->dev, "bridge control possible\n");
	/* [한국어] 브리지를 공유하는 다른 VGA 장치가 있어, 이 카드는 COMMAND
	 * 레지스터로만 제어할 수 있다. */
	else
		vgaarb_info(&vgadev->pdev->dev, "no bridge control possible\n");
}

/*
 * Currently, we assume that the "initial" setup of the system is not sane,
 * that is, we come up with conflicting devices and let the arbiter's
 * client decide if devices decodes legacy things or not.
 */
/*
 * [한국어]
 * vga_arbiter_add_pci_device - VGA 클래스 PCI 장치를 중재 대상에 등록한다
 *
 * @pdev: 새로 나타난 VGA 클래스 PCI 장치. 호출자가 pci_is_vga() 로 이미 걸렀다.
 * @return: true = 등록 성공. false = 메모리 할당 실패 또는 중복 등록.
 *   호출자 pci_notify() 는 true 일 때만 클라이언트에게 알린다.
 *
 * 왜 필요한가: 중재기는 시스템의 모든 VGA 장치를 알고 있어야 누가 누구와
 * 충돌하는지 계산할 수 있다. 이 함수가 새 장치의 초기 상태를 하드웨어에서
 * 읽어 소프트웨어 그림자(vga_device)로 만들어 목록에 넣는다.
 *
 * 동작 단계:
 *  (1) 항목을 0 초기화해 할당한다(모든 카운터와 플래그가 0/false 로 시작).
 *  (2) 락을 잡고 중복 등록이 아닌지 확인한다.
 *  (3) decodes 를 "전부 해석한다"로 낙관적으로 설정한다. 위 상류 주석이 밝히듯,
 *      부팅 직후 상태는 온전하지 않다고 가정하고 일단 충돌 가능성을 최대로 잡은
 *      뒤, 실제 능력은 클라이언트(GPU 드라이버)가 나중에 알려 주게 한다.
 *  (4) owns 는 낙관적으로 잡지 않고 하드웨어에서 읽는다 -- COMMAND 레지스터의
 *      IO/MEM 활성 비트가 실제 소유 상태다.
 *  (5) 경로상 브리지 중 하나라도 VGA Enable 이 꺼져 있으면 레거시 사이클이
 *      이 카드까지 오지 못하므로 owns 를 0 으로 지운다.
 *  (6) 기본 VGA 장치 후보인지 판정하고, 그렇다면 승격시킨다.
 *  (7) 브리지 공유 관계를 계산한다.
 *  (8) 목록 꼬리에 넣고 카운터를 올린 뒤 결과를 dmesg 에 남긴다.
 *
 * 실행 컨텍스트: PCI 버스 알림자 콜백(프로세스 문맥) 또는 초기화 함수에서
 * 불린다. 할당은 락 **밖**에서 하고(GFP_KERNEL 은 잠들 수 있으므로 스핀락
 * 안에서 쓸 수 없다), 목록 조작만 락 안에서 한다 -- 순서가 뒤바뀌면 스핀락을
 * 쥔 채 잠들어 시스템이 멈춘다.
 *
 * 에러 경로: 할당 실패는 로그만 남기고 조용히 false. 위 상류 주석이 "더 나은
 * 방법을 모르겠다"고 인정한 대로, 그 장치는 중재에서 빠진 채 남는다.
 * 중복 등록은 BUG_ON 으로 잡고 fail 레이블에서 항목을 해제한다.
 *
 * 호출 체인:
 *   pci_notify()(BUS_NOTIFY_ADD_DEVICE) 또는 vga_arb_device_init()
 *     -> [vga_arbiter_add_pci_device]
 *        -> kzalloc_obj() -> vgadev_find() -> pci_read_config_word()
 *        -> vga_is_boot_device() -> vga_set_default_device()
 *        -> vga_arbiter_check_bridge_sharing() -> list_add_tail()
 */
static bool vga_arbiter_add_pci_device(struct pci_dev *pdev)
{
	/* [한국어] 새로 만들 중재 항목. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 브리지 계보를 위로 훑을 때의 현재 버스. */
	struct pci_bus *bus;
	/* [한국어] 그 버스의 상위 브리지 장치. */
	struct pci_dev *bridge;
	/* [한국어] 이 장치의 PCI COMMAND 레지스터(오프셋 0x04) 값. 16비트다. */
	u16 cmd;

	/* Allocate structure */
	/* [한국어] kzalloc_obj(T) 는 sizeof(T) 만큼을 0 으로 채워 할당하는 매크로다
	 * (include/linux/slab.h). 0 초기화가 중요한 이유: locks, owns, 네 개의 카운터,
	 * is_firmware_default, set_decode 가 모두 "아직 아무것도 없음"으로 시작해야
	 * 하기 때문이다. 기본 GFP 는 GFP_KERNEL 이라 잠들 수 있으므로 반드시 아래
	 * spin_lock 보다 **앞에서** 호출해야 한다. */
	vgadev = kzalloc_obj(struct vga_device);
	/* [한국어] 메모리가 부족해 할당에 실패한 경우. */
	if (vgadev == NULL) {
		/* [한국어] 실패를 dmesg 에 남긴다. 이 장치는 중재에서 빠지므로, 나중에
		 * 화면이 깨질 때 원인을 추적할 단서가 된다. */
		vgaarb_err(&pdev->dev, "failed to allocate VGA arbiter data\n");
		/*
		 * What to do on allocation failure? For now, let's just do
		 * nothing, I'm not sure there is anything saner to be done.
		 */
		/* [한국어] false = 등록 실패. 호출자는 클라이언트에게 알리지 않는다. */
		return false;
	}

	/* Take lock & check for duplicates */
	/* [한국어] 여기서부터 목록과 전역 카운터를 만지므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 같은 장치가 이미 등록돼 있는가. 알림자가 중복으로 왔거나
	 * 초기화 스캔과 알림자가 겹치면 여기 걸린다. */
	if (vgadev_find(pdev) != NULL) {
		/* [한국어] BUG_ON(1) 은 무조건 커널 버그로 보고한다(스택 트레이스를 찍고,
		 * 설정에 따라 패닉). 중복 등록은 논리적으로 일어나서는 안 되는 상태이므로
		 * 조용히 넘기지 않고 크게 알린다. BUG_ON 이 패닉하지 않는 설정에서는 계속
		 * 진행하므로 아래 goto 로 정리 경로로 빠진다. */
		BUG_ON(1);
		goto fail;
	}
	/* [한국어] 이 항목이 대표할 PCI 장치를 기록한다. 여기부터 vgadev 가
	 * 하드웨어와 연결된다. */
	vgadev->pdev = pdev;

	/* By default, assume we decode everything */
	/* [한국어] 네 자원을 모두 해석한다고 낙관적으로 가정한다. 실제보다 넓게
	 * 잡는 쪽이 안전하다 -- 해석하지 않는 자원을 해석한다고 보면 불필요한 중재가
	 * 일어날 뿐이지만, 반대로 보면 충돌을 놓쳐 화면이 깨진다.
	 * 이 값은 나중에 GPU 드라이버의 set_decode 콜백이나 vga_set_legacy_decoding()
	 * 이 사실에 맞게 좁혀 준다. */
	vgadev->decodes = VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM |
			  VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;

	/* By default, mark it as decoding */
	/* [한국어] decodes 가 레거시 비트를 포함하므로 "레거시를 해석하는 장치 수"를
	 * 하나 올린다. 이 카운터는 vga_arb_read() 가 유저스페이스에 count: 로 노출한다. */
	vga_decode_count++;

	/*
	 * Mark that we "own" resources based on our enables, we will
	 * clear that below if the bridge isn't forwarding.
	 */
	/* [한국어] owns 는 추측하지 않고 하드웨어에서 읽는다. COMMAND 레지스터는
	 * PCI 스펙의 표준 헤더 오프셋 0x04 에 있는 16비트 레지스터다. */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	/* [한국어] bit 0 = I/O Space Enable. 켜져 있으면 이 카드가 지금 레거시 IO
	 * 주소(0x3b0 대역)에도 응답하고 있다는 뜻이므로 소유로 기록한다. */
	if (cmd & PCI_COMMAND_IO)
		vgadev->owns |= VGA_RSRC_LEGACY_IO;
	/* [한국어] bit 1 = Memory Space Enable. 켜져 있으면 레거시 메모리
	 * (0xa0000-0xbffff)에도 응답 중이므로 소유로 기록한다. */
	if (cmd & PCI_COMMAND_MEMORY)
		vgadev->owns |= VGA_RSRC_LEGACY_MEM;

	/* Check if VGA cycles can get down to us */
	/* [한국어] 이제 "레거시 사이클이 정말 여기까지 내려오는가"를 확인한다.
	 * COMMAND 가 켜져 있어도 중간 브리지가 막고 있으면 소유가 아니다. */
	bus = pdev->bus;
	/* [한국어] 이 장치의 버스에서 루트까지 거슬러 올라간다. */
	while (bus) {
		/* [한국어] 이 버스의 상위 브리지. 루트 버스면 NULL 이라 검사를 건너뛴다. */
		bridge = bus->self;
		/* [한국어] 브리지가 있는 단계에서만 VGA 전달 여부를 확인할 수 있다. */
		if (bridge) {
			/* [한국어] Bridge Control 레지스터 값을 담을 지역 변수. 루프 안에 두어
			 * 범위를 좁힌다. */
			u16 l;

			/* [한국어] PCI_BRIDGE_CONTROL 은 PCI-to-PCI 브리지 헤더(타입 1)의
			 * 오프셋 0x3e 에 있는 16비트 레지스터다. */
			pci_read_config_word(bridge, PCI_BRIDGE_CONTROL, &l);
			/* [한국어] PCI_BRIDGE_CTL_VGA(bit 3) 가 "VGA Enable" -- 이 비트가 꺼져
			 * 있으면 브리지가 레거시 VGA 주소 범위를 아래로 전달하지 않는다. */
			if (!(l & PCI_BRIDGE_CTL_VGA)) {
				/* [한국어] 경로가 한 군데라도 막혀 있으면 이 카드는 레거시 자원을
				 * 전혀 소유하지 못한 것이다. COMMAND 비트로 추정했던 소유를
				 * 통째로 취소한다. */
				vgadev->owns = 0;
				/* [한국어] 더 위를 볼 필요가 없다 -- 이미 막혔다. */
				break;
			}
		}
		/* [한국어] 한 단계 위 버스로. NULL 이 되면 루트에 닿아 루프가 끝난다. */
		bus = bus->parent;
	}

	/* [한국어] 이 카드가 지금까지의 후보보다 나은 기본 VGA 장치인가.
	 * 반드시 owns 를 다 채운 뒤에 물어야 한다 -- 판정이 owns 를 보기 때문이다. */
	if (vga_is_boot_device(vgadev)) {
		/* [한국어] 기본 장치 승격을 dmesg 에 남긴다. 이미 다른 장치가 기본이었다면
		 * " (overriding previous)" 를 덧붙여, 부팅 중 기본 장치가 바뀐 이력을
		 * 로그만으로 따라갈 수 있게 한다. */
		vgaarb_info(&pdev->dev, "setting as boot VGA device%s\n",
			    vga_default_device() ?
			    " (overriding previous)" : "");
		/* [한국어] 참조 카운트를 옮기며 vga_default 를 교체한다. */
		vga_set_default_device(pdev);
	}

	/* [한국어] 브리지 공유 관계를 계산한다. 반드시 list_add_tail() **이전**에
	 * 불러야 한다 -- 목록에 넣은 뒤라면 자기 자신을 "이미 등록된 장치"로 만나
	 * 자기 브리지를 공유한다고 잘못 판정한다. */
	vga_arbiter_check_bridge_sharing(vgadev);

	/* Add to the list */
	/* [한국어] 이제 목록에 넣는다. 꼬리에 붙이므로 등록 순서가 유지되고,
	 * vga_is_boot_device() 의 "처음 찾은 것을 쓴다" 규칙이 열거 순서를 따르게 된다. */
	list_add_tail(&vgadev->list, &vga_list);
	/* [한국어] 중재 대상 장치 수를 하나 올린다. vga_arbiter_notify_clients() 가
	 * 이 값이 1 을 넘는지로 "카드가 여러 장인가"를 판단한다. */
	vga_count++;
	/* [한국어] 최종 상태를 사람이 읽는 형태로 남긴다. 이 한 줄이면 등록 직후의
	 * decodes/owns/locks 를 dmesg 만으로 확인할 수 있다. */
	vgaarb_info(&pdev->dev, "VGA device added: decodes=%s,owns=%s,locks=%s\n",
		vga_iostate_to_str(vgadev->decodes),
		vga_iostate_to_str(vgadev->owns),
		vga_iostate_to_str(vgadev->locks));

	/* [한국어] 성공 경로의 락 해제. */
	spin_unlock_irqrestore(&vga_lock, flags);
	/* [한국어] true = 등록 성공. 호출자는 클라이언트들에게 변화를 알린다. */
	return true;
/* [한국어] 중복 등록 정리 경로. */
fail:
	/* [한국어] 먼저 락을 푼다. */
	spin_unlock_irqrestore(&vga_lock, flags);
	/* [한국어] 그 다음 방금 할당한 항목을 해제한다. kfree 는 잠들 수 있으므로
	 * 반드시 스핀락 **밖**에서 불러야 한다 -- 그래서 unlock 이 먼저다. */
	kfree(vgadev);
	/* [한국어] false = 등록하지 못했다. */
	return false;
}

/*
 * [한국어]
 * vga_arbiter_del_pci_device - 사라진 PCI 장치를 중재 대상에서 제거한다
 *
 * @pdev: 제거되는 PCI 장치.
 * @return: true = 목록에서 빼고 정리했다. false = 애초에 등록돼 있지 않았다.
 *   호출자 pci_notify() 는 true 일 때만 클라이언트에게 알린다.
 *
 * 왜 필요한가: 장치가 뽑히거나 드라이버가 떼어지면 그 pci_dev 가 사라진다.
 * 중재 목록에 허상 포인터를 남겨 두면 다음 중재에서 죽은 메모리를 만지게 된다.
 *
 * 동작 단계: 항목을 찾고, 그것이 기본 장치였다면 기본 장치를 비우고,
 * 레거시 해석 카운터를 조정하고, 목록에서 빼고, 대기자를 모두 깨운다.
 *
 * 잠금 상태를 되돌리지 않는다는 점에 주의: 이 장치에 잠금이 걸려 있었더라도
 * 항목은 그냥 사라진다. 그 대신 대기자를 깨워, 그 자원을 기다리던 쪽이 다시
 * 시도해 이제는 성공하게 만든다. 뒤늦게 도착하는 vga_put() 은 vgadev_find() 가
 * NULL 을 돌려주어 조용히 무시된다.
 *
 * 실행 컨텍스트: PCI 버스 알림자 콜백(프로세스 문맥). 임계 구역 전체가
 * vga_lock 하나이며, kfree 는 락을 푼 뒤에 부른다.
 *
 * 에러 경로: 못 찾으면 bail 로 가서 false 를 돌려준다. 이때 kfree(NULL) 이
 * 불리는데 아무 일도 하지 않으므로 안전하다.
 *
 * 호출 체인:
 *   pci_notify()(BUS_NOTIFY_DEL_DEVICE) -> [vga_arbiter_del_pci_device]
 *     -> vgadev_find() -> vga_set_default_device(NULL) -> list_del()
 *     -> wake_up_all() -> kfree()
 */
static bool vga_arbiter_del_pci_device(struct pci_dev *pdev)
{
	/* [한국어] 제거할 중재 항목. 못 찾으면 NULL 로 남고, 아래 kfree(NULL) 이
	 * 아무 일도 하지 않으므로 그대로 흘러가도 안전하다. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 반환값. 성공을 기본값으로 두고 못 찾은 경로에서만 false 로 바꾼다. */
	bool ret = true;

	/* [한국어] 목록과 전역 카운터를 만지므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 락 안에서 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] 등록된 적 없는 장치다(VGA 클래스가 아니었거나 등록에 실패했던
	 * 장치). 알릴 변화가 없으므로 false 로 돌아선다. */
	if (vgadev == NULL) {
		ret = false;
		goto bail;
	}

	/* [한국어] 사라지는 장치가 기본 VGA 장치였다면 기본 장치 자리를 비운다.
	 * 비우지 않으면 vga_default 가 해제된 pci_dev 를 가리키게 된다.
	 * 참고: 여기서 대체 장치를 다시 고르지는 않는다 -- 이후 vga_default_device() 는
	 * NULL 을 돌려주고, vga_get(NULL, ...) 은 조용히 성공으로 빠진다. */
	if (vga_default == pdev)
		/* [한국어] NULL 을 넘기면 이전 장치에 pci_dev_put 만 걸고 비운다. */
		vga_set_default_device(NULL);

	/* [한국어] 이 장치가 레거시를 해석하고 있었다면 전역 카운터를 내려야 한다.
	 * 해석하지 않는 상태로 바뀐 뒤 제거되는 경우도 있으므로 조건이 필요하다. */
	if (vgadev->decodes & (VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM))
		/* [한국어] "레거시를 해석하는 장치 수"를 하나 줄인다. */
		vga_decode_count--;

	/* Remove entry from list */
	/* [한국어] 목록에서 뗀다. 이 순간부터 vgadev_find() 는 이 장치를 못 찾는다. */
	list_del(&vgadev->list);
	/* [한국어] 중재 대상 장치 수를 하나 줄인다. 이 값이 1 로 내려가면
	 * vga_arbiter_notify_clients() 가 남은 카드에게 "이제 혼자니 디코딩을 켜도
	 * 된다"고 알리게 된다. */
	vga_count--;

	/* Wake up all possible waiters */
	/* [한국어] 이 장치가 잠그고 있던 자원을 기다리던 태스크들을 모두 깨운다.
	 * 잠금 카운터를 정상적으로 되돌리지 않고 항목을 없애 버리므로, 깨워 주지
	 * 않으면 그들은 영원히 잠든 채 남는다. */
	wake_up_all(&vga_wait_queue);
/* [한국어] 성공/실패 경로가 모이는 락 해제 지점. */
bail:
	/* [한국어] 스핀락 해제와 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&vga_lock, flags);
	/* [한국어] 항목 메모리를 반납한다. 반드시 락 밖이어야 한다(kfree 는 잠들 수
	 * 있다). 못 찾은 경로에서는 vgadev 가 NULL 인데, kfree(NULL) 은 아무 일도
	 * 하지 않으므로 별도 분기가 필요 없다. */
	kfree(vgadev);
	/* [한국어] true 면 호출자가 클라이언트들에게 변화를 알린다. */
	return ret;
}

/* Called with the lock */
/*
 * [한국어]
 * vga_update_device_decodes - 카드의 해석(decode) 능력 변화를 중재 상태에 반영한다
 *
 * @vgadev: 대상 카드의 중재 항목.
 * @new_decodes: 새 해석 능력 비트마스크. 호출 경로에 따라 의미가 조금 다르다 --
 *   __vga_set_legacy_decoding() 에서 오면 이미 VGA_RSRC_LEGACY_MASK 로 걸러진
 *   레거시 두 비트뿐이고, vga_arbiter_notify_clients() 에서 오면 드라이버의
 *   set_decode 콜백이 돌려준 값 그대로다.
 * @return: 없음.
 *
 * 왜 필요한가: 카드가 "이제 레거시 자원을 해석하지 않는다"고 선언하면, 그
 * 카드는 중재에서 빠져도 된다. 그런데 해석하지 않게 된 자원에 잠금이 남아
 * 있으면 그 잠금은 영원히 풀리지 않아 다른 카드가 굶는다. 이 함수가 그
 * 정리(해석 능력을 잃은 자원의 잠금 강제 해제)와 전역 카운터 조정을 한 곳에서 한다.
 *
 * 동작 단계: (1) 사라진 해석 능력(decodes_removed)과 그중 잠겨 있던 것
 * (decodes_unlocked)을 먼저 계산한다 -- decodes 를 덮어쓰기 **전에** 계산해야
 * 옛 값을 쓸 수 있다. (2) 새 값을 기록하고 로그를 남긴다. (3) 잠겨 있던 자원의
 * 카운터를 0 으로 만들고 __vga_put() 을 불러 잠금 비트를 지우고 대기자를 깨운다.
 * (4) 레거시 해석 장치 수(vga_decode_count)를 상태 전이에 맞춰 증감한다.
 *
 * 실행 컨텍스트: 함수 위 상류 주석 "Called with the lock" 이 계약을 명시한다 --
 * **vga_lock 을 쥔 채** 불러야 한다. 두 호출자 모두 그 계약을 지킨다.
 * 락 안에서 __vga_put() 을 부르는데, 그 함수도 락을 요구하므로 계약이 맞물린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __vga_set_legacy_decoding() / vga_arbiter_notify_clients()
 *     -> [vga_update_device_decodes] -> __vga_put() -> wake_up_all()
 */
static void vga_update_device_decodes(struct vga_device *vgadev,
				      unsigned int new_decodes)
{
	/* [한국어] 로그 매크로에 넘길 device 포인터. */
	struct device *dev = &vgadev->pdev->dev;
	/* [한국어] 바꾸기 전의 해석 능력. 아래 비교와 로그에 쓰므로 덮어쓰기 전에
	 * 반드시 복사해 두어야 한다. */
	unsigned int old_decodes = vgadev->decodes;
	/* [한국어] 이번에 "사라진" 해석 능력 = 옛날에는 있었지만 새 값에는 없는 비트.
	 * ~new_decodes 로 새 값을 반전시켜 옛 값과 AND 하면 정확히 그것만 남는다. */
	unsigned int decodes_removed = ~new_decodes & old_decodes;
	/* [한국어] 사라진 해석 능력 중 **잠금이 걸려 있던** 것. 이것이 문제의 핵심이다 --
	 * 해석하지도 않는 자원을 잠근 채로 두면 아무도 그것을 가져갈 수 없다.
	 * 아래에서 강제로 풀어 준다. */
	unsigned int decodes_unlocked = vgadev->locks & decodes_removed;

	/* [한국어] 이제 새 값을 기록한다. 위 세 계산이 모두 끝난 뒤여야 한다. */
	vgadev->decodes = new_decodes;

	/* [한국어] 변화를 dmesg 에 남긴다. 옛 값과 새 값, 그리고 소유 상태를 함께
	 * 찍어 "해석 능력이 줄었는데 소유는 그대로"인 상황을 눈으로 확인할 수 있게 한다. */
	vgaarb_info(dev, "VGA decodes changed: olddecodes=%s,decodes=%s:owns=%s\n",
		    vga_iostate_to_str(old_decodes),
		    vga_iostate_to_str(vgadev->decodes),
		    vga_iostate_to_str(vgadev->owns));

	/* If we removed locked decodes, lock count goes to zero, and release */
	/* [한국어] 해석 능력을 잃은 자원에 잠금이 남아 있었다면 정리해야 한다. */
	if (decodes_unlocked) {
		/* [한국어] 레거시 IO 잠금을 통째로 무효화한다. */
		if (decodes_unlocked & VGA_RSRC_LEGACY_IO)
			/* [한국어] 카운터를 하나씩 내리는 게 아니라 **0 으로 강제**한다. 중첩
			 * 카운터가 몇이든 상관없이 잠금을 즉시 풀어야 하기 때문이다. 아래 __vga_put()
			 * 이 "카운터가 0 이면 잠금 비트를 지운다"로 동작하므로, 미리 0 으로 만들어야
			 * 한 번의 호출로 확실히 풀린다. */
			vgadev->io_lock_cnt = 0;
		/* [한국어] 레거시 MEM 쪽 짝. */
		if (decodes_unlocked & VGA_RSRC_LEGACY_MEM)
			/* [한국어] 마찬가지로 0 으로 강제. */
			vgadev->mem_lock_cnt = 0;
		/* [한국어] 잠금 비트를 지우고 대기자를 깨우게 한다. 카운터를 이미 0 으로
		 * 만들어 두었으므로, __vga_put() 안의 감소는 `> 0` 검사에 걸려 아무 일도
		 * 하지 않고, 곧바로 잠금 비트 해제와 wake_up_all() 로 이어진다. */
		__vga_put(vgadev, decodes_unlocked);
	}

	/* Change decodes counter */
	/* [한국어] 전이 1 -- 레거시를 해석하던 카드가 더 이상 해석하지 않게 됐다.
	 * 전역 "레거시 해석 장치 수"를 하나 줄인다. 두 조건을 모두 봐야 하는 이유는
	 * 이 카운터가 상태가 아니라 **전이**에 반응해야 하기 때문이다(같은 상태로
	 * 여러 번 호출돼도 카운터가 어긋나면 안 된다). */
	if (old_decodes & VGA_RSRC_LEGACY_MASK &&
	    !(new_decodes & VGA_RSRC_LEGACY_MASK))
		vga_decode_count--;
	/* [한국어] 전이 2 -- 해석하지 않던 카드가 해석하게 됐다. 하나 늘린다. */
	if (!(old_decodes & VGA_RSRC_LEGACY_MASK) &&
	    new_decodes & VGA_RSRC_LEGACY_MASK)
		vga_decode_count++;
	/* [한국어] 조정 결과를 동적 디버그 로그로 남긴다. 이 값은 vga_arb_read() 가
	 * 유저스페이스에 "count:" 로 그대로 노출하는 값이기도 하다. */
	vgaarb_dbg(dev, "decoding count now is: %d\n", vga_decode_count);
}

/*
 * [한국어]
 * __vga_set_legacy_decoding - 레거시 해석 능력 설정의 공통 구현
 *
 * @pdev: 대상 카드.
 * @decodes: 새 레거시 해석 능력. 레거시 비트만 유효하다.
 * @userspace: true = /dev/vga_arbiter 의 "decodes" 명령에서 온 요청,
 *   false = 커널 드라이버가 vga_set_legacy_decoding() 으로 부른 요청.
 *   이 플래그 하나로 권한을 가른다.
 * @return: 없음. 실패해도 알리지 않는다.
 *
 * 왜 필요한가: 같은 동작에 진입점이 둘(커널 API 와 유저스페이스 명령)인데
 * 권한이 다르다. 공통 로직을 여기 모으고, 차이는 @userspace 플래그로만 둔다.
 *
 * 권한 규칙: 커널 드라이버가 set_decode 콜백을 등록해 둔 카드는 그 드라이버가
 * 해석 능력을 관리하는 중이다. 유저스페이스가 끼어들어 그 값을 덮어쓰면 커널의
 * 상태와 실제 하드웨어가 어긋나므로, 그런 카드에 대한 유저스페이스 요청은
 * 조용히 무시한다(상류 주석 "Don't let userspace futz with kernel driver decodes").
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수가 vga_lock 을 **직접 잡는다** --
 * vga_update_device_decodes() 가 락을 요구하므로 여기서 채워 주는 것이다.
 *
 * 에러 경로: 장치를 못 찾거나 권한이 없으면 goto bail 로 조용히 나간다.
 * void 함수라 호출자에게 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   vga_set_legacy_decoding()(커널) / vga_arb_write()("decodes", 유저스페이스)
 *     -> [__vga_set_legacy_decoding] -> vgadev_find()
 *        -> vga_update_device_decodes()
 */
static void __vga_set_legacy_decoding(struct pci_dev *pdev,
				      unsigned int decodes,
				      bool userspace)
{
	/* [한국어] 대상 카드의 중재 항목. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;

	/* [한국어] 레거시 두 비트만 남긴다. 이 API 는 이름 그대로 "레거시" 해석
	 * 능력만 다루며, NORMAL 비트가 섞여 들어와도 무시한다. 이 마스킹이 없으면
	 * 유저스페이스가 NORMAL 비트를 세워 중재 회계를 어지럽힐 수 있다.
	 * 부작용: 여기서 NORMAL 비트가 지워지므로, 이 경로를 거치면 카드의 decodes 는
	 * 레거시 두 비트만 남게 된다. 코드에 있는 그대로다. */
	decodes &= VGA_RSRC_LEGACY_MASK;

	/* [한국어] 아래 vga_update_device_decodes() 가 락을 요구하므로 여기서 잡는다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 락 안에서 중재 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] 중재 대상이 아닌 장치면 할 일이 없다. */
	if (vgadev == NULL)
		goto bail;

	/* Don't let userspace futz with kernel driver decodes */
	/* [한국어] 유저스페이스 요청인데 이 카드는 커널 드라이버가 관리 중이다.
	 * set_decode 가 NULL 이 아니라는 것이 곧 "드라이버가 vga_client_register() 로
	 * 등록했다"는 뜻이다. 커널이 관리하는 값을 유저스페이스가 덮어쓰지 못하게 막는다.
	 * 커널 경로(userspace == false)는 이 검사를 그냥 통과한다. */
	if (userspace && vgadev->set_decode)
		goto bail;

	/* Update the device decodes + counter */
	/* [한국어] 실제 반영. 사라진 해석 능력의 잠금을 풀고 전역 카운터를 조정한다. */
	vga_update_device_decodes(vgadev, decodes);

	/*
	 * XXX If somebody is going from "doesn't decode" to "decodes"
	 * state here, additional care must be taken as we may have pending
	 * ownership of non-legacy region.
	 */
/* [한국어] 세 경로(성공, 장치 없음, 권한 없음)가 모이는 락 해제 지점. */
bail:
	/* [한국어] 스핀락 해제와 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&vga_lock, flags);
}

/**
 * vga_set_legacy_decoding
 * @pdev: PCI device of the VGA card
 * @decodes: bit mask of what legacy regions the card decodes
 *
 * Indicate to the arbiter if the card decodes legacy VGA IOs, legacy VGA
 * Memory, both, or none. All cards default to both, the card driver (fbdev for
 * example) should tell the arbiter if it has disabled legacy decoding, so the
 * card can be left out of the arbitration process (and can be safe to take
 * interrupts at any time.
 */
/*
 * [한국어]
 * vga_set_legacy_decoding - 커널 드라이버가 자기 카드의 레거시 해석 능력을 알린다
 *
 * @pdev: 알리는 드라이버의 카드.
 * @decodes: 이 카드가 실제로 해석하는 레거시 영역의 비트마스크
 *   (VGA_RSRC_LEGACY_IO / VGA_RSRC_LEGACY_MEM 조합, 없으면 0).
 * @return: 없음.
 *
 * 왜 필요한가: 중재기는 등록 시점에 모든 카드가 모든 자원을 해석한다고 낙관적으로
 * 가정한다(vga_arbiter_add_pci_device 참고). 그러나 현대 GPU 는 레거시 VGA 해석을
 * 꺼 둘 수 있고, 꺼 두었다면 중재 대상에서 빠져도 된다. 위 상류 커널독이 밝히듯,
 * 그렇게 빠진 카드는 언제든 인터럽트를 받아도 안전해진다 -- 중재 때문에 자원이
 * 꺼졌다 켜졌다 하는 일이 없기 때문이다.
 *
 * 이 함수는 __vga_set_legacy_decoding(..., userspace=false) 를 부르는 얇은
 * 껍데기다. false 를 넘기므로 커널 드라이버가 등록해 둔 set_decode 콜백이 있어도
 * 값이 반영된다 -- 즉 "커널은 자기 값을 언제든 고칠 수 있다".
 *
 * 실행 컨텍스트: 잠들지 않으며, 내부에서 vga_lock 을 잡는다. 따라서 호출자는
 * vga_lock 을 쥔 채로 부르면 안 된다(자기 자신을 두 번 잡는 데드락).
 *
 * 에러 경로: 없다. 장치를 못 찾아도 조용히 무시된다.
 *
 * 호출 체인:
 *   GPU/fbdev 드라이버 -> [vga_set_legacy_decoding]
 *     -> __vga_set_legacy_decoding(pdev, decodes, false)
 */
void vga_set_legacy_decoding(struct pci_dev *pdev, unsigned int decodes)
{
	/* [한국어] 세 번째 인자 false = "커널이 부른 요청"이라는 표시.
	 * 유저스페이스 권한 검사를 건너뛰게 한다. */
	__vga_set_legacy_decoding(pdev, decodes, false);
}
/* [한국어] 모듈에서 쓸 수 있도록 내보낸다. */
EXPORT_SYMBOL(vga_set_legacy_decoding);

/**
 * vga_client_register - register or unregister a VGA arbitration client
 * @pdev: PCI device of the VGA client
 * @set_decode: VGA decode change callback
 *
 * Clients have two callback mechanisms they can use.
 *
 * @set_decode callback: If a client can disable its GPU VGA resource, it
 * will get a callback from this to set the encode/decode state.
 *
 * Rationale: we cannot disable VGA decode resources unconditionally
 * because some single GPU laptops seem to require ACPI or BIOS access to
 * the VGA registers to control things like backlights etc. Hopefully newer
 * multi-GPU laptops do something saner, and desktops won't have any
 * special ACPI for this. The driver will get a callback when VGA
 * arbitration is first used by userspace since some older X servers have
 * issues.
 *
 * Does not check whether a client for @pdev has been registered already.
 *
 * To unregister, call vga_client_unregister().
 *
 * Returns: 0 on success, -ENODEV on failure
 */
/*
 * [한국어]
 * vga_client_register - GPU 드라이버의 디코딩 제어 콜백을 등록/해제한다
 *
 * @pdev: 콜백을 등록할 카드.
 * @set_decode: "디코딩을 켜라/꺼라"를 수행하고 실제 적용된 decodes 비트마스크를
 *   돌려주는 드라이버 함수. NULL 을 넘기면 등록 해제이며, vgaarb.h 의 인라인
 *   vga_client_unregister() 가 그렇게 부른다.
 * @return: 0 = 성공, -ENODEV = 그 장치가 중재 목록에 없음(VGA 클래스가 아니거나
 *   아직 등록되지 않음). 드라이버는 보통 실패해도 치명적으로 다루지 않는다.
 *
 * 왜 필요한가: 중재기는 카드의 디코딩을 마음대로 끌 수 없다. 위 상류 커널독이
 * 이유를 밝힌다 -- GPU 가 하나뿐인 일부 노트북은 백라이트 제어 등을 위해
 * ACPI/BIOS 가 VGA 레지스터에 접근해야 해서, 함부로 끄면 화면이 죽는다. 그래서
 * "끌 수 있는지"를 드라이버가 판단하도록 콜백으로 위임한다.
 *
 * 콜백이 불리는 시점: 유저스페이스가 처음으로 VGA 중재를 사용할 때
 * (vga_check_first_use -> vga_arbiter_notify_clients), 그리고 카드가 추가/제거될
 * 때다. 상류 커널독이 "since some older X servers have issues" 라고 적은 것이
 * 그 게으른 알림 정책의 이유다.
 *
 * 같은 카드에 대해 이미 등록된 클라이언트가 있는지 검사하지 않는다는 점도
 * 커널독에 명시돼 있다 -- 나중 등록이 앞의 것을 덮어쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 내부에서 vga_lock 을 직접 잡으므로 호출자는
 * 그 락을 쥔 채로 부르면 안 된다.
 *
 * 에러 경로: 항목을 못 찾으면 -ENODEV. 이때 콜백은 어디에도 저장되지 않는다.
 *
 * 호출 체인:
 *   amdgpu / i915 / nouveau / radeon / qxl / virtio-gpu / lsdc /
 *   drivers/video/aperture.c -> [vga_client_register]
 *     -> vgadev_find() -> vgadev->set_decode 대입
 */
int vga_client_register(struct pci_dev *pdev,
		unsigned int (*set_decode)(struct pci_dev *pdev, bool decode))
{
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 대상 카드의 중재 항목. 락을 푼 뒤에도 NULL 여부만 보므로
	 * 역참조하지 않아 안전하다. */
	struct vga_device *vgadev;

	/* [한국어] set_decode 필드는 vga_lock 이 지키는 상태이므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 락 안에서 중재 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] 찾았을 때만 콜백을 기록한다. NULL 을 넘긴 경우도 그대로 대입되어
	 * 등록 해제가 된다. */
	if (vgadev)
		vgadev->set_decode = set_decode;
	/* [한국어] 대입이 끝났으니 곧바로 락을 푼다. 아래 판정은 포인터의 NULL
	 * 여부만 보므로 락 밖에서 해도 안전하다. */
	spin_unlock_irqrestore(&vga_lock, flags);
	/* [한국어] 항목을 찾지 못했다면 등록에 실패한 것이다. */
	if (!vgadev)
		return -ENODEV;
	/* [한국어] 0 = 등록(또는 해제) 성공. */
	return 0;
}
/* [한국어] 모듈에서 쓸 수 있도록 내보낸다. GPU 드라이버들의 주 진입점이다. */
EXPORT_SYMBOL(vga_client_register);

/*
 * Char driver implementation
 *
 * Semantics is:
 *
 *  open       : Open user instance of the arbiter. By default, it's
 *                attached to the default VGA device of the system.
 *
 *  close      : Close user instance, release locks
 *
 *  read       : Return a string indicating the status of the target.
 *                An IO state string is of the form {io,mem,io+mem,none},
 *                mc and ic are respectively mem and io lock counts (for
 *                debugging/diagnostic only). "decodes" indicate what the
 *                card currently decodes, "owns" indicates what is currently
 *                enabled on it, and "locks" indicates what is locked by this
 *                card. If the card is unplugged, we get "invalid" then for
 *                card_ID and an -ENODEV error is returned for any command
 *                until a new card is targeted
 *
 *   "<card_ID>,decodes=<io_state>,owns=<io_state>,locks=<io_state> (ic,mc)"
 *
 * write       : write a command to the arbiter. List of commands is:
 *
 *   target <card_ID>   : switch target to card <card_ID> (see below)
 *   lock <io_state>    : acquire locks on target ("none" is invalid io_state)
 *   trylock <io_state> : non-blocking acquire locks on target
 *   unlock <io_state>  : release locks on target
 *   unlock all         : release all locks on target held by this user
 *   decodes <io_state> : set the legacy decoding attributes for the card
 *
 * poll         : event if something change on any card (not just the target)
 *
 * card_ID is of the form "PCI:domain:bus:dev.fn". It can be set to "default"
 * to go back to the system default card (TODO: not implemented yet).
 * Currently, only PCI is supported as a prefix, but the userland API may
 * support other bus types in the future, even if the current kernel
 * implementation doesn't.
 *
 * Note about locks:
 *
 * The driver keeps track of which user has what locks on which card. It
 * supports stacking, like the kernel one. This complicates the implementation
 * a bit, but makes the arbiter more tolerant to userspace problems and able
 * to properly cleanup in all cases when a process dies.
 * Currently, a max of 16 cards simultaneously can have locks issued from
 * userspace for a given user (file descriptor instance) of the arbiter.
 *
 * If the device is hot-unplugged, there is a hook inside the module to notify
 * it being added/removed in the system and automatically added/removed in
 * the arbiter.
 */

/* [한국어] 한 유저스페이스 인스턴스(열린 fd 하나)가 동시에 잠금을 걸 수 있는
 * 카드 수의 상한. drivers/pci/Kconfig 의 CONFIG_VGA_ARB_MAX_GPUS 로 정해지며
 * 기본값은 16 이다. 배열 크기를 컴파일 시점에 고정해 동적 할당과 목록 관리를
 * 피하려는 선택이다 -- Kconfig 도움말이 "각 GPU 당 오버헤드는 매우 작다"고 밝힌다.
 * 상한을 넘으면 vga_arb_write() 의 "target" 처리가 -ENOMEM 을 돌려준다. */
#define MAX_USER_CARDS         CONFIG_VGA_ARB_MAX_GPUS
/* [한국어] "이 카드는 이제 유효하지 않다"를 뜻하는 보초(sentinel) 포인터.
 * (struct pci_dev *)-1UL 은 절대 유효한 커널 포인터가 될 수 없는 값이라
 * NULL 과 구분되는 두 번째 특수값으로 쓸 수 있다.
 * 확인된 사실: 이 트리의 vgaarb.c 에서 이 상수는 정의와 vga_arb_read() 의 비교
 * 딱 두 곳에만 등장하며, priv->target 에 이 값을 **대입하는 코드는 없다**
 * (주석을 제거한 토큰열로 전수 확인). 즉 현재 구현에서 그 비교는 성립하지
 * 않는다. 파일 위쪽 상류 주석이 말하는 "카드가 뽑히면 invalid 를 돌려준다"는
 * 설계 의도의 흔적으로 보인다. */
#define PCI_INVALID_CARD       ((struct pci_dev *)-1UL)

/* Each user has an array of these, tracking which cards have locks */
/*
 * [한국어] struct vga_arb_user_card - 한 유저스페이스 인스턴스가 한 카드에
 * 대해 쥐고 있는 잠금 기록
 *
 * vga_arb_private 안에 MAX_USER_CARDS 개의 배열로 들어간다. 위 상류 주석이
 * 밝히듯 "어느 유저가 어느 카드에 무슨 잠금을 걸었는지"를 커널이 추적하기
 * 위한 자료다. 이것이 있어야 프로세스가 죽어 fd 가 닫힐 때
 * vga_arb_release() 가 남은 잠금을 대신 반납해 줄 수 있다 -- 유저스페이스
 * 프로그램이 크래시해도 시스템의 그래픽이 잠긴 채 남지 않는 이유다.
 */
struct vga_arb_user_card {
	/* [한국어] 이 슬롯이 추적하는 카드. NULL 이면 빈 슬롯이다.
	 * 설정자: vga_arb_open() 이 0 번 슬롯을 기본 장치로 채우고, vga_arb_write() 의
	 *   "target" 처리가 빈 슬롯을 찾아 채운다.
	 * 읽는 자: "lock"/"unlock"/"trylock" 처리가 대상 카드의 슬롯을 찾을 때,
	 *   vga_arb_release() 가 정리할 카드를 훑을 때.
	 * 값 범위: NULL(빈 슬롯) 또는 pci_dev 포인터. 한 번 채워진 슬롯은 fd 가 닫힐
	 *   때까지 비워지지 않는다(제거 코드가 없다).
	 * 동기화: 이 배열은 fd 하나에 속한 사적 자료지만, vga_arb_release() 는
	 *   vga_user_lock 을 쥐고 훑는다. 반면 vga_arb_write() 는 어떤 락도 없이
	 *   고친다 -- 같은 fd 를 여러 스레드가 동시에 쓰면 경쟁이 생길 수 있으며,
	 *   그것을 막는 코드는 이 파일에 없다(사실 그대로 적는다). */
	struct pci_dev *pdev;
	/* [한국어] 이 fd 가 이 카드에 건 레거시 MEM 잠금의 횟수.
	 * 설정자: "lock"/"trylock" 성공 시 ++, "unlock" 시 --.
	 * 읽는 자: "unlock" 이 0 인데 풀려고 하면 -EINVAL 로 거절하는 검사,
	 *   그리고 vga_arb_release() 가 남은 횟수만큼 vga_put() 을 반복하는 루프.
	 * 값 범위: 0 이상. 커널 쪽 vga_device.mem_lock_cnt 와 별개의, 이 fd 몫만 센
	 *   장부다. 둘을 나눠 두어야 한 fd 가 죽어도 다른 fd 의 잠금이 함께 풀리지 않는다.
	 * 동기화: 위 pdev 와 같다. */
	unsigned int mem_cnt;
	/* [한국어] 이 fd 가 이 카드에 건 레거시 IO 잠금의 횟수. mem_cnt 의 IO 쪽 짝.
	 * 설정자/읽는 자/값 범위/동기화 모두 mem_cnt 와 같다.
	 * 주의: vga_str_to_iostate() 가 유저스페이스 요청을 언제나 IO|MEM 으로 묶기
	 *   때문에, 실제로는 io_cnt 와 mem_cnt 가 늘 같은 값으로 움직인다. */
	unsigned int io_cnt;
};

/*
 * [한국어] struct vga_arb_private - /dev/vga_arbiter 를 연 fd 하나의 상태
 *
 * vga_arb_open() 이 만들어 file->private_data 에 걸고, vga_arb_release() 가
 * 해제한다. 즉 수명이 열린 파일 서술자와 정확히 같다. 이 구조체가 있어서
 * 여러 프로세스가 각자 독립적으로 중재기를 쓸 수 있고, 프로세스가 죽으면
 * 그 몫의 잠금만 정확히 정리된다.
 */
struct vga_arb_private {
	/* [한국어] 전역 인스턴스 목록 vga_user_list 에 매다는 연결 고리.
	 * 설정자: vga_arb_open() 이 list_add(), vga_arb_release() 가 list_del().
	 * 읽는 자: 이 파일에서 vga_user_list 를 **순회하는 코드는 없다**(토큰 전수
	 *   확인 결과 vga_user_list 는 LIST_HEAD 정의와 list_add 두 곳에만 등장한다).
	 *   즉 현재는 목록에 넣고 빼기만 할 뿐 쓰지 않는다.
	 * 값 범위: 목록에 있는 동안 유효한 리스트 노드.
	 * 동기화: vga_user_lock 스핀락. */
	struct list_head list;
	/* [한국어] 이 fd 의 명령이 향하는 "현재 대상 카드".
	 * 설정자: vga_arb_open() 이 시스템 기본 장치로 초기화하고, "target" 명령이 바꾼다.
	 * 읽는 자: "lock"/"unlock"/"trylock"/"decodes" 명령과 vga_arb_read() 가
	 *   무엇에 대해 동작할지 결정하는 데 쓴다.
	 * 값 범위: NULL(기본 장치가 없었음) 또는 pci_dev 포인터.
	 *   PCI_INVALID_CARD 와 비교하는 코드가 vga_arb_read() 에 있으나, 그 값을
	 *   대입하는 코드는 이 파일에 없다.
	 * 동기화: 별도 보호가 없다. 참조 카운트도 순 증가로 유지되지 않는다 --
	 *   "target" 처리가 pci_dev_get() 으로 잡았다가 같은 함수 끝에서
	 *   pci_dev_put() 으로 놓아 버리므로, 저장된 포인터는 참조를 갖지 않는
	 *   쿠키에 가깝다. 코드에 있는 그대로 적는다. */
	struct pci_dev *target;
	/* [한국어] 이 fd 가 잠금을 건 카드들의 장부. 고정 크기 배열(기본 16)이다.
	 * 설정자: vga_arb_open() 이 0 번을 채우고, "target" 명령이 빈 슬롯을 찾아 채운다.
	 * 읽는 자: 잠금/해제 명령이 선형 탐색으로 해당 카드를 찾고,
	 *   vga_arb_release() 가 전체를 훑어 남은 잠금을 반납한다.
	 * 값 범위: 앞에서부터 채워지며, pdev == NULL 인 첫 슬롯이 빈 자리다.
	 * 동기화: vga_arb_release() 는 vga_user_lock 아래에서 훑지만
	 *   vga_arb_write() 는 락 없이 고친다. */
	struct vga_arb_user_card cards[MAX_USER_CARDS];
	/* [한국어] 이 인스턴스 전용 스핀락으로 의도된 필드.
	 * 설정자: vga_arb_open() 이 spin_lock_init() 으로 초기화한다.
	 * 읽는 자: **없다.** 주석을 제거한 토큰열로 전수 확인한 결과, 이 파일에서
	 *   priv->lock 을 실제로 잡는(spin_lock 계열) 코드는 한 곳도 없다.
	 *   초기화만 되고 쓰이지 않는 필드다 -- 사실 그대로 적는다.
	 * 값 범위: 초기화된 spinlock_t.
	 * 동기화: 해당 없음. */
	spinlock_t lock;
};

/* [한국어] 열려 있는 모든 /dev/vga_arbiter 인스턴스의 목록.
 * 설정자: vga_arb_open() 의 list_add() 와 vga_arb_release() 의 list_del().
 * 읽는 자: 없다(위 list 필드 주석 참조). 목록을 순회하는 코드가 이 파일에 없다.
 * 동기화: 아래 vga_user_lock. */
static LIST_HEAD(vga_user_list);
/* [한국어] vga_user_list 와 각 인스턴스의 cards 배열 정리를 지키는 스핀락.
 * vga_lock 과 별개인 이유: 유저스페이스 인스턴스 관리는 중재 상태와 다른
 * 축의 자료이고, vga_arb_release() 가 이 락을 쥔 채 vga_put() 을 부르는데
 * vga_put() 이 안에서 vga_lock 을 잡기 때문이다. 두 락은 항상
 * vga_user_lock -> vga_lock 순서로만 겹쳐 잡히므로 잠금 순서 역전이 없다. */
static DEFINE_SPINLOCK(vga_user_lock);


/*
 * Take a string in the format: "PCI:domain:bus:dev.fn" and return the
 * respective values. If the string is not in this format, return 0.
 */
/*
 * [한국어]
 * vga_pci_str_to_vars - "PCI:domain:bus:dev.fn" 문자열을 PCI 주소 성분으로 쪼갠다
 *
 * @buf: 파싱할 문자열의 시작 위치. vga_arb_write() 가 "target " 접두사를
 *   건너뛴 뒤의 포인터를 넘긴다. 커널 스택의 kbuf 안이며 널 종료가 보장된다.
 * @count: 남은 바이트 수. **이 함수는 이 값을 쓰지 않는다** -- 파싱을 sscanf 에
 *   맡기고 길이 검사를 하지 않는다. 안전한 이유는 호출자가 널 종료를 보장하기
 *   때문이다.
 * @domain: 출력 -- PCI 도메인(세그먼트) 번호.
 * @bus: 출력 -- 버스 번호.
 * @devfn: 출력 -- PCI_DEVFN(slot, func) 로 합쳐진 8비트 장치/기능 번호.
 * @return: 1 = 네 성분을 모두 읽었다, 0 = 형식이 맞지 않는다. 호출자는 0 이면
 *   -EPROTO 로 write() 를 거절한다.
 *
 * 왜 필요한가: 유저스페이스 ABI 가 카드를 문자열로 지정하기 때문이다
 * (Documentation/gpu/vgaarbiter.rst 의 card_ID 형식). 커널은 그것을
 * pci_get_domain_bus_and_slot() 이 받을 수 있는 숫자로 바꿔야 한다.
 *
 * 형식 주의: %x 이므로 네 성분 모두 **16진수**로 읽는다. lspci 가 보여 주는
 * 표기(예: 0000:01:00.0)와 그대로 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(write 시스템 콜). 락을 잡지 않는다.
 *
 * 에러 경로: sscanf 가 네 개를 못 채우면 0 을 돌려주고 *devfn 은 손대지 않는다.
 *   다만 domain 과 bus 는 sscanf 가 부분적으로 채웠을 수 있다 -- 호출자가
 *   실패 시 그 값을 쓰지 않으므로 문제되지 않는다.
 *
 * 호출 체인:
 *   vga_arb_write()("target") -> [vga_pci_str_to_vars] -> sscanf()
 */
static int vga_pci_str_to_vars(char *buf, int count, unsigned int *domain,
			       unsigned int *bus, unsigned int *devfn)
{
	/* [한국어] sscanf 가 실제로 변환한 항목 수. 4 여야 성공이다. */
	int n;
	/* [한국어] slot(장치 번호)과 func(기능 번호)를 따로 읽어 두었다가 아래에서
	 * 하나의 devfn 으로 합친다. 문자열은 둘을 '.' 로 나눠 표기하지만 커널 API 는
	 * 합쳐진 8비트 값을 쓰기 때문이다. */
	unsigned int slot, func;

	/* [한국어] "PCI:" 리터럴로 시작하는지 확인하면서 네 개의 16진수를 읽는다.
	 * 형식 문자열의 ':' 와 '.' 는 입력에도 그대로 있어야 하며, 하나라도 어긋나면
	 * 변환이 거기서 멈춰 n 이 4 미만이 된다. */
	n = sscanf(buf, "PCI:%x:%x:%x.%x", domain, bus, &slot, &func);
	/* [한국어] 네 개를 다 채우지 못했으면 형식 위반이다. */
	if (n != 4)
		return 0;

	/* [한국어] PCI_DEVFN(slot, func) 은 (slot << 3) | (func & 7) 로 두 값을 하나의
	 * 8비트 devfn 으로 합치는 매크로다(include/linux/pci.h). PCI 스펙에서 한 버스에
	 * 장치 32개(5비트), 장치마다 기능 8개(3비트)가 가능하기 때문에 이런 배치다. */
	*devfn = PCI_DEVFN(slot, func);

	/* [한국어] 1 = 파싱 성공. */
	return 1;
}

/*
 * [한국어]
 * vga_arb_read - /dev/vga_arbiter 에서 현재 대상 카드의 상태 문자열을 읽어 준다
 *
 * @file: 열린 파일. private_data 에 이 fd 의 vga_arb_private 가 들어 있다.
 * @buf: 유저 공간 버퍼. __user 표시는 커널이 직접 역참조하면 안 되는
 *   포인터라는 뜻이며, sparse 정적 검사기가 이 규칙 위반을 잡아낸다.
 * @count: 유저가 요청한 최대 바이트 수.
 * @ppos: 파일 오프셋. **이 함수는 쓰지 않는다** -- 이 장치는 매번 현재 상태를
 *   돌려주는 스트림이지 오프셋이 있는 파일이 아니다.
 * @return: 유저에게 복사한 바이트 수, 또는 -ENOMEM / -EFAULT.
 *
 * 출력 형식(상류 주석과 Documentation/gpu/vgaarbiter.rst 에 명시):
 *   "count:<n>,PCI:<이름>,decodes=<상태>,owns=<상태>,locks=<상태>(<ic>:<mc>)"
 *   여기서 상태는 io / mem / io+mem / none 중 하나이고, ic 와 mc 는 각각
 *   IO 와 MEM 의 잠금 중첩 횟수다(진단용).
 *   대상이 없거나 목록에서 찾지 못하면 "invalid" 한 단어만 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥(read 시스템 콜). 버퍼 할당은 락 밖에서
 * GFP_KERNEL 로 하고(잠들 수 있으므로), 상태를 읽는 동안만 vga_lock 을 잡으며,
 * 유저 공간 복사는 다시 락 밖에서 한다 -- copy_to_user() 는 페이지 폴트로
 * 잠들 수 있어 스핀락 안에서 부르면 안 되기 때문이다. 이 세 단계의 순서가
 * 이 함수 구조의 전부다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM. 복사 실패는 -EFAULT(버퍼는 그 전에 해제한다).
 *
 * 호출 체인:
 *   유저스페이스 read() -> VFS -> [vga_arb_read]
 *     -> kmalloc() -> spin_lock_irqsave(vga_lock) -> vgadev_find()
 *     -> vga_iostate_to_str() -> copy_to_user() -> kfree()
 */
static ssize_t vga_arb_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	/* [한국어] 이 fd 의 사적 상태. vga_arb_open() 이 넣어 둔 것이다. */
	struct vga_arb_private *priv = file->private_data;
	/* [한국어] 대상 카드의 중재 항목. 락 안에서만 유효하다. */
	struct vga_device *vgadev;
	/* [한국어] 대상 카드의 pci_dev. priv->target 의 사본이다. */
	struct pci_dev *pdev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 만들어 낸 문자열의 길이. 아래에서 유저 요청 크기로 잘린다. */
	size_t len;
	/* [한국어] copy_to_user() 의 반환값(복사하지 못한 바이트 수)을 받는다. */
	int rc;
	/* [한국어] 문자열을 조립할 커널 측 임시 버퍼. 스택이 아니라 힙을 쓰는 이유는
	 * 1KB 가 커널 스택(보통 16KB)에 두기엔 부담스럽기 때문이다. */
	char *lbuf;

	/* [한국어] 1KB 를 잡는다. 출력 형식이 고정이라 넘칠 일이 없는 넉넉한 크기다.
	 * GFP_KERNEL 은 잠들 수 있으므로 반드시 아래 spin_lock 보다 앞이어야 한다. */
	lbuf = kmalloc(1024, GFP_KERNEL);
	/* [한국어] 메모리 부족. 아직 락을 잡지 않았으므로 그냥 반환해도 안전하다. */
	if (lbuf == NULL)
		return -ENOMEM;

	/* Protect vga_list */
	/* [한국어] 여기서부터 vga_list 와 vgadev 필드를 읽으므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);

	/* If we are targeting the default, use it */
	/* [한국어] 이 fd 가 겨누고 있는 카드를 꺼낸다. */
	pdev = priv->target;
	/* [한국어] 대상이 없거나(fd 를 열 때 기본 장치가 없었다) 무효 표시면
	 * 상태를 만들 수 없다. PCI_INVALID_CARD 비교는 현재 이 파일에 그 값을
	 * 대입하는 코드가 없어 성립하지 않지만, 방어적으로 남아 있다. */
	if (pdev == NULL || pdev == PCI_INVALID_CARD) {
		/* [한국어] 문자열 조립에는 락이 필요 없으므로 먼저 푼다. */
		spin_unlock_irqrestore(&vga_lock, flags);
		/* [한국어] "invalid" 한 단어만 돌려준다. 상류 주석대로, 유저스페이스는 이
		 * 응답을 보고 대상을 다시 지정해야 한다. */
		len = sprintf(lbuf, "invalid");
		goto done;
	}

	/* Find card vgadev structure */
	/* [한국어] 락 안에서 중재 항목을 찾는다. */
	vgadev = vgadev_find(pdev);
	/* [한국어] target 으로 지정됐던 카드가 그 사이 중재 목록에서 사라진 경우.
	 * 위 상류 주석이 "일어나서는 안 되는 일"이라고 적었지만, 핫플러그로 카드가
	 * 뽑히면 실제로 일어날 수 있다. */
	if (vgadev == NULL) {
		/*
		 * Wow, it's not in the list, that shouldn't happen, let's
		 * fix us up and return invalid card.
		 */
		/* [한국어] 락을 풀고, */
		spin_unlock_irqrestore(&vga_lock, flags);
		/* [한국어] 역시 "invalid" 로 답한다. */
		len = sprintf(lbuf, "invalid");
		goto done;
	}

	/* Fill the buffer with info */
	/* [한국어] 상태 문자열을 조립한다. snprintf 는 1024 바이트를 넘지 않도록
	 * 잘라 주므로 버퍼 오버플로가 없다. 반환값 len 은 (잘리지 않았다면) 실제
	 * 문자열 길이다. */
	len = snprintf(lbuf, 1024,
		       "count:%d,PCI:%s,decodes=%s,owns=%s,locks=%s(%u:%u)\n",
		       /* [한국어] count: 는 전역 vga_decode_count -- 시스템 전체에서 레거시를
		        * 해석하는 장치 수다. 특정 카드의 값이 아니라는 점에 주의.
		        * pci_name() 은 "0000:01:00.0" 형식의 장치 이름 문자열을 돌려준다. */
		       vga_decode_count, pci_name(pdev),
		       /* [한국어] decodes -- 이 카드가 해석할 수 있는 자원. */
		       vga_iostate_to_str(vgadev->decodes),
		       /* [한국어] owns -- 지금 실제로 켜져 있는 자원. */
		       vga_iostate_to_str(vgadev->owns),
		       /* [한국어] locks -- 지금 잠겨 있는 자원. 이어지는 (%u:%u) 에 IO/MEM 잠금의
		        * 중첩 횟수를 그대로 노출해 디버깅을 돕는다. */
		       vga_iostate_to_str(vgadev->locks),
		       vgadev->io_lock_cnt, vgadev->mem_lock_cnt);

	/* [한국어] 조립이 끝났으니 락을 푼다. 아래 copy_to_user() 는 잠들 수 있어
	 * 반드시 락 밖에서 불러야 한다. */
	spin_unlock_irqrestore(&vga_lock, flags);
/* [한국어] 정상 경로와 "invalid" 두 경로가 모이는 지점. 여기부터는 락이
 * 풀린 상태다. */
done:

	/* Copy that to user */
	/* [한국어] 유저가 요청한 것보다 길면 잘라 낸다. 이 검사가 없으면
	 * copy_to_user() 가 유저 버퍼를 넘어 써 메모리를 망가뜨린다. */
	if (len > count)
		len = count;
	/* [한국어] 커널 버퍼의 내용을 유저 공간으로 복사한다. 실패하면 복사하지
	 * 못한 바이트 수를 돌려준다(0 이 성공). 페이지 폴트로 잠들 수 있다. */
	rc = copy_to_user(buf, lbuf, len);
	/* [한국어] 성공하든 실패하든 임시 버퍼는 반드시 해제한다. 아래 오류 반환
	 * **전에** 해제해야 메모리 누수가 없다. */
	kfree(lbuf);
	/* [한국어] 한 바이트라도 복사하지 못했으면 유저 포인터가 잘못된 것이다. */
	if (rc)
		return -EFAULT;
	/* [한국어] 실제로 유저에게 전달한 바이트 수. read() 의 반환값이 된다. */
	return len;
}

/*
 * TODO: To avoid parsing inside kernel and to improve the speed we may
 * consider use ioctl here
 */
/*
 * [한국어]
 * vga_arb_write - /dev/vga_arbiter 에 들어온 문자열 명령을 해석해 실행한다
 *
 * @file: 열린 파일. private_data 에 이 fd 의 vga_arb_private 가 있다.
 * @buf: 유저 공간의 명령 문자열. __user 포인터라 직접 역참조하면 안 된다.
 * @count: 쓰기 요청 바이트 수.
 * @ppos: 파일 오프셋. 이 함수는 쓰지 않는다.
 * @return: 성공이면 count(전부 소비했다는 뜻), 실패면 음수 오류 코드.
 *   -EINVAL(길이 초과, 잠금 회계 불일치), -EFAULT(유저 버퍼 접근 실패),
 *   -EPROTO(모르는 명령이나 자원 이름), -ENODEV(대상 없음), -EBUSY(trylock 실패),
 *   -ENOMEM(카드 슬롯 소진).
 *
 * 지원 명령(위 "Char driver implementation" 상류 주석 및
 * Documentation/gpu/vgaarbiter.rst 와 동일):
 *   "target <card_ID>"  -- 이후 명령이 향할 카드를 바꾼다. card_ID 는
 *                          "PCI:domain:bus:dev.fn" 또는 "default".
 *   "lock <io_state>"   -- 대상 카드를 잠근다. 자원이 날 때까지 블로킹.
 *   "trylock <io_state>"-- 블로킹 없이 시도.
 *   "unlock <io_state>" -- 잠금을 푼다. "unlock all" 도 받는다.
 *   "decodes <io_state>"-- 카드의 레거시 해석 능력을 설정한다.
 *
 * 왜 이런 설계인가: 커널이 잠금의 주인을 fd 단위로 기억하기 때문에, 유저스페이스
 * 프로그램이 잠금을 쥔 채 죽어도 fd 가 닫히면서 vga_arb_release() 가 자동으로
 * 정리한다. 상류 주석이 "makes the arbiter more tolerant to userspace problems"
 * 라고 밝힌 부분이 이것이다. 함수 위 상류 주석은 문자열 파싱 대신 ioctl 을 쓰는
 * 편이 나았을 것이라고 스스로 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 문맥(write 시스템 콜). "lock" 명령이
 * vga_get_uninterruptible() 을 통해 **잠들 수 있으므로** 어떤 락도 쥐지 않은
 * 상태여야 한다. 실제로 이 함수는 vga_lock 도 vga_user_lock 도 잡지 않는다.
 * 그 결과 priv->cards 배열과 priv->target 을 락 없이 읽고 쓴다 -- 같은 fd 를
 * 여러 스레드가 동시에 쓰면 경쟁이 생길 수 있으며, 그것을 막는 코드는 없다.
 *
 * 확인된 사실 두 가지(코드에 있는 그대로):
 *  (1) "trylock" 처리는 `if (vga_tryget(...))` 가 참일 때를 성공으로 다룬다.
 *      그러나 vga_tryget() 은 성공 시 0, 실패 시 음수를 돌려준다. 즉 판정이
 *      뒤집혀 있다.
 *  (2) "target" 처리는 vgadev_find() 를 vga_lock 없이 부른다. 다른 모든
 *      호출자는 락을 쥐고 부른다.
 *  두 가지 모두 이 트리 안에 설명이나 근거가 없다.
 *
 * 호출 체인:
 *   유저스페이스 write() -> VFS -> [vga_arb_write]
 *     -> copy_from_user() -> vga_str_to_iostate() / vga_pci_str_to_vars()
 *     -> vga_get_uninterruptible() / vga_tryget() / vga_put()
 *     -> __vga_set_legacy_decoding()
 */
static ssize_t vga_arb_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	/* [한국어] 이 fd 의 사적 상태(대상 카드와 잠금 장부). */
	struct vga_arb_private *priv = file->private_data;
	/* [한국어] "unlock" 처리에서 찾아낸 카드 슬롯. NULL 로 시작해, 못 찾으면
	 * NULL 인 채로 남아 -EINVAL 판정에 쓰인다. */
	struct vga_arb_user_card *uc = NULL;
	/* [한국어] 명령이 향하는 카드의 pci_dev. 대부분 priv->target 의 사본이고,
	 * "target" 명령에서는 새로 조회한 결과가 들어간다. */
	struct pci_dev *pdev;

	/* [한국어] 파싱된 자원 비트마스크. vga_str_to_iostate() 가 채운다. */
	unsigned int io_state;

	/* [한국어] kbuf     -- 유저 문자열을 복사해 올 커널 스택 버퍼(64바이트).
	 * curr_pos -- 파싱 커서. 명령 접두사를 건너뛰며 앞으로 나아간다.
	 * 스택 버퍼를 쓰는 이유는 명령이 짧고 고정 상한이 있기 때문이다. */
	char kbuf[64], *curr_pos;
	/* [한국어] 커서 뒤에 남은 바이트 수. 접두사를 건너뛸 때마다 함께 줄인다.
	 * vga_str_to_iostate() 와 vga_pci_str_to_vars() 에 넘기지만 두 함수 모두
	 * 실제로는 이 값을 쓰지 않는다(널 종료에 의존한다). */
	size_t remaining = count;

	/* [한국어] vga_get_uninterruptible() 의 반환값을 받는다. */
	int err;
	/* [한국어] 이 write() 의 최종 반환값. 모든 분기가 done 레이블로 모이며
	 * 그 직전에 이 변수를 채운다. */
	int ret_val;
	/* [한국어] priv->cards 배열 순회용 인덱스. "target" 처리에서는 루프가
	 * 끝난 뒤의 값(MAX_USER_CARDS 인지)으로 슬롯 소진 여부를 판정하므로,
	 * 루프 밖에 선언돼 있어야 한다. */
	int i;

	/* [한국어] kbuf 는 64바이트이고 아래에서 kbuf[count] 에 널을 쓰므로,
	 * count 가 63 을 넘으면 배열 밖을 침범한다. >= 로 비교해 count == 64 도
	 * 막는 것이 핵심이다(그때 kbuf[64] 는 이미 범위 밖). */
	if (count >= sizeof(kbuf))
		return -EINVAL;
	/* [한국어] 유저 공간에서 커널 버퍼로 복사한다. 0 이 아니면 복사하지 못한
	 * 바이트가 있다는 뜻이므로 유저 포인터가 잘못된 것이다. */
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	/* [한국어] 파싱 커서를 버퍼 처음에 놓는다. */
	curr_pos = kbuf;
	/* [한국어] 널 종료를 강제한다. 유저가 개행이나 널을 보내지 않았을 수 있는데,
	 * 아래 strncmp/sscanf 가 모두 널 종료를 전제하므로 반드시 필요하다.
	 * 위 길이 검사 덕분에 count 는 최대 63 이라 이 쓰기는 항상 버퍼 안이다. */
	kbuf[count] = '\0';

	/* [한국어] 명령 1 -- "lock <io_state>". 접두사에 공백까지 포함해 비교하므로
	 * "lockfoo" 같은 문자열은 걸리지 않는다. */
	if (strncmp(curr_pos, "lock ", 5) == 0) {
		/* [한국어] "lock " 5바이트를 건너뛴다. */
		curr_pos += 5;
		/* [한국어] 남은 길이도 함께 줄여 커서와 일관되게 유지한다. */
		remaining -= 5;

		/* [한국어] 어느 클라이언트(fd)가 무슨 명령을 보냈는지 동적 디버그 로그로
		 * 남긴다. priv 포인터를 식별자로 쓴다. */
		pr_debug("client 0x%p called 'lock'\n", priv);

		/* [한국어] "io"/"mem"/"io+mem"/"none" 중 하나로 파싱한다. 모르는 문자열이면
		 * 프로토콜 위반이다. */
		if (!vga_str_to_iostate(curr_pos, remaining, &io_state)) {
			ret_val = -EPROTO;
			goto done;
		}
		/* [한국어] "lock none" 은 의미가 없다. 잠글 자원을 지정하지 않은 요청이므로
		 * 거절한다. (unlock/trylock 쪽에는 이 검사가 없고, 상류가 "TODO: Add this?"
		 * 주석으로 그 사실을 남겨 두었다.) */
		if (io_state == VGA_RSRC_NONE) {
			ret_val = -EPROTO;
			goto done;
		}

		/* [한국어] 현재 대상 카드를 꺼낸다. */
		pdev = priv->target;
		/* [한국어] 대상이 지정되지 않았다면 잠글 카드가 없다. pdev 대신
		 * priv->target 을 다시 읽어 비교하지만 같은 값이다. */
		if (priv->target == NULL) {
			ret_val = -ENODEV;
			goto done;
		}

		/* [한국어] 실제 잠금. vgaarb.h 의 인라인이 vga_get(pdev, io_state, 0) 을
		 * 부른다 -- 0 은 "인터럽트 불가 대기"라는 뜻이다. 여기서 잠들 수 있으므로,
		 * 이 함수가 어떤 스핀락도 쥐고 있지 않다는 점이 중요하다.
		 * 참고: 인터럽트 불가 대기를 쓰기 때문에 잠금을 기다리는 동안 이 write() 는
		 * 시그널로 중단되지 않는다. */
		err = vga_get_uninterruptible(pdev, io_state);
		/* [한국어] 잠금 실패(-ENODEV 등). 커널이 돌려준 오류를 그대로 유저에게 전달한다. */
		if (err) {
			ret_val = err;
			goto done;
		}

		/* [한국어] 커널 쪽 잠금은 성공했으니, 이 fd 의 장부에도 기록해야 한다.
		 * 이 기록이 있어야 fd 가 닫힐 때 vga_arb_release() 가 정확한 횟수만큼
		 * 반납해 줄 수 있다. */
		/* Update the client's locks lists */
		/* [한국어] 슬롯 배열을 선형 탐색한다(최대 16개라 충분히 빠르다). */
		for (i = 0; i < MAX_USER_CARDS; i++) {
			/* [한국어] 이 카드의 슬롯을 찾았다. 슬롯은 "target" 명령 때 이미 만들어져
			 * 있다 -- 없으면 이 루프는 아무것도 하지 않고 끝나며, 커널 잠금과 fd 장부가
			 * 어긋나게 된다(코드에 있는 그대로다). */
			if (priv->cards[i].pdev == pdev) {
				/* [한국어] IO 잠금 횟수를 하나 올린다. */
				if (io_state & VGA_RSRC_LEGACY_IO)
					priv->cards[i].io_cnt++;
				/* [한국어] MEM 잠금 횟수를 하나 올린다. vga_str_to_iostate() 가 언제나
				 * 두 비트를 함께 세우므로 실제로는 둘이 나란히 증가한다. */
				if (io_state & VGA_RSRC_LEGACY_MEM)
					priv->cards[i].mem_cnt++;
				/* [한국어] 슬롯은 카드당 하나뿐이므로 더 볼 필요가 없다. */
				break;
			}
		}

		/* [한국어] 성공. write() 는 요청한 바이트를 전부 소비했다고 알린다. */
		ret_val = count;
		goto done;
	/* [한국어] 명령 2 -- "unlock <io_state>" 또는 "unlock all". */
	} else if (strncmp(curr_pos, "unlock ", 7) == 0) {
		/* [한국어] "unlock " 7바이트를 건너뛴다. */
		curr_pos += 7;
		/* [한국어] 남은 길이도 맞춘다. */
		remaining -= 7;

		/* [한국어] 명령 수신을 동적 디버그 로그로 남긴다. */
		pr_debug("client 0x%p called 'unlock'\n", priv);

		/* [한국어] "all" 은 특별 취급 -- vga_str_to_iostate() 를 거치지 않고 곧바로
		 * IO|MEM 으로 해석한다. 문서상 "이 유저가 이 대상에 건 모든 잠금 해제"지만,
		 * 실제 구현은 아래에서 IO 와 MEM 을 **한 번씩만** 내리므로 중첩 잠금을 전부
		 * 풀지는 않는다. Documentation/gpu/vgaarbiter.rst 도 이 명령을
		 * "(not implemented yet)" 으로 적어 두었다. */
		if (strncmp(curr_pos, "all", 3) == 0)
			io_state = VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
		/* [한국어] "all" 이 아니면 일반 자원 이름으로 파싱한다. */
		else {
			/* [한국어] 함수 이름과 인자 목록이 두 줄로 나뉘어 있을 뿐, 앞의
			 * "lock" 처리와 같은 파싱이다. */
			if (!vga_str_to_iostate
			    (curr_pos, remaining, &io_state)) {
				ret_val = -EPROTO;
				goto done;
			}
			/* TODO: Add this?
			   if (io_state == VGA_RSRC_NONE) {
			   ret_val = -EPROTO;
			   goto done;
			   }
			  */
		}

		/* [한국어] 현재 대상 카드를 꺼낸다. */
		pdev = priv->target;
		/* [한국어] 대상이 없으면 풀 잠금도 없다. */
		if (priv->target == NULL) {
			ret_val = -ENODEV;
			goto done;
		}
		/* [한국어] 이 카드의 슬롯을 찾는다. 앞의 "lock" 루프와 달리 break 가 없어
		 * 배열 끝까지 훑는다 -- 같은 카드가 여러 슬롯에 들어갈 수 없으므로 결과는
		 * 같지만, 마지막으로 일치한 슬롯이 남는다는 차이가 있다. */
		for (i = 0; i < MAX_USER_CARDS; i++) {
			/* [한국어] 일치하는 슬롯의 주소를 uc 에 담아 아래에서 카운터를 조작한다. */
			if (priv->cards[i].pdev == pdev)
				uc = &priv->cards[i];
		}

		/* [한국어] 이 fd 가 그 카드에 대해 슬롯을 만든 적이 없다. 즉 잠근 적이
		 * 없으므로 풀 것도 없다. */
		if (!uc) {
			ret_val = -EINVAL;
			goto done;
		}

		/* [한국어] IO 를 풀라고 했는데 이 fd 의 IO 잠금 횟수가 0 이다.
		 * 이 검사가 없으면 아래에서 0 을 하나 더 내려 unsigned 언더플로가 나고,
		 * 무엇보다 다른 fd 가 건 잠금을 남의 fd 가 풀어 버리게 된다. */
		if (io_state & VGA_RSRC_LEGACY_IO && uc->io_cnt == 0) {
			ret_val = -EINVAL;
			goto done;
		}

		/* [한국어] MEM 쪽의 같은 검사. */
		if (io_state & VGA_RSRC_LEGACY_MEM && uc->mem_cnt == 0) {
			ret_val = -EINVAL;
			goto done;
		}

		/* [한국어] 커널 쪽 잠금을 실제로 반납한다. 카운터가 0 이 되면 잠금 비트가
		 * 풀리고 대기자가 깨어난다. */
		vga_put(pdev, io_state);

		/* [한국어] 커널 반납이 끝났으니 이 fd 의 장부도 맞춘다. 순서가 중요하다 --
		 * 먼저 내려 두면 위 검사와 실제 반납 사이에 상태가 어긋난다. */
		if (io_state & VGA_RSRC_LEGACY_IO)
			uc->io_cnt--;
		/* [한국어] MEM 쪽 짝. */
		if (io_state & VGA_RSRC_LEGACY_MEM)
			uc->mem_cnt--;

		/* [한국어] 성공. */
		ret_val = count;
		goto done;
	/* [한국어] 명령 3 -- "trylock <io_state>". 블로킹 없이 시도한다. */
	} else if (strncmp(curr_pos, "trylock ", 8) == 0) {
		/* [한국어] "trylock " 8바이트를 건너뛴다. */
		curr_pos += 8;
		/* [한국어] 남은 길이도 맞춘다. */
		remaining -= 8;

		/* [한국어] 명령 수신을 동적 디버그 로그로 남긴다. */
		pr_debug("client 0x%p called 'trylock'\n", priv);

		/* [한국어] 자원 이름을 파싱한다. 여기에는 "lock" 과 달리 VGA_RSRC_NONE
		 * 거절 검사가 없다 -- 바로 아래 상류 "TODO: Add this?" 주석이 그 누락을
		 * 인정하고 있다. */
		if (!vga_str_to_iostate(curr_pos, remaining, &io_state)) {
			ret_val = -EPROTO;
			goto done;
		}
		/* TODO: Add this?
		   if (io_state == VGA_RSRC_NONE) {
		   ret_val = -EPROTO;
		   goto done;
		   }
		 */

		/* [한국어] 현재 대상 카드를 꺼낸다. */
		pdev = priv->target;
		/* [한국어] 대상이 없으면 잠글 카드가 없다. */
		if (priv->target == NULL) {
			ret_val = -ENODEV;
			goto done;
		}

		/* [한국어] 비블로킹 시도. **주의**: vga_tryget() 은 성공 시 0, 실패 시 음수
		 * (-EBUSY / -ENODEV)를 돌려주므로, 이 조건이 참인 쪽은 사실 실패 경로다.
		 * 즉 판정이 뒤집혀 있다. 이 트리 안에 이 형태를 설명하는 주석이나 근거는
		 * 없으며, 코드에 있는 그대로만 적어 둔다. */
		if (vga_tryget(pdev, io_state)) {
			/* Update the client's locks lists... */
			/* [한국어] 이 fd 의 장부에도 잠금을 기록한다. "lock" 쪽과 같은 루프다. */
			for (i = 0; i < MAX_USER_CARDS; i++) {
				/* [한국어] 이 카드의 슬롯을 찾는다. */
				if (priv->cards[i].pdev == pdev) {
					/* [한국어] IO 잠금 횟수를 올린다. */
					if (io_state & VGA_RSRC_LEGACY_IO)
						priv->cards[i].io_cnt++;
					/* [한국어] MEM 잠금 횟수를 올린다. */
					if (io_state & VGA_RSRC_LEGACY_MEM)
						priv->cards[i].mem_cnt++;
					/* [한국어] 슬롯은 하나뿐이므로 종료. */
					break;
				}
			}
			/* [한국어] 성공으로 보고한다. */
			ret_val = count;
			goto done;
		/* [한국어] 위 조건이 거짓인 쪽. */
		} else {
			/* [한국어] -EBUSY 로 보고한다. */
			ret_val = -EBUSY;
			goto done;
		}

	/* [한국어] 명령 4 -- "target <card_ID>". 이후 명령이 향할 카드를 바꾼다.
	 * 다른 명령과 달리 카드를 새로 조회하므로 참조 카운트를 다룬다. */
	} else if (strncmp(curr_pos, "target ", 7) == 0) {
		/* [한국어] "PCI:domain:bus:dev.fn" 을 쪼갠 결과를 받을 지역 변수들.
		 * 이 분기 안에서만 쓰이므로 블록 안에 선언한다. */
		unsigned int domain, bus, devfn;
		/* [한국어] 조회한 카드가 중재 대상인지 확인할 때 쓰는 지역 변수.
		 * 바깥 함수의 이름과 겹치지 않는 이 분기 전용 변수다. */
		struct vga_device *vgadev;

		/* [한국어] "target " 7바이트를 건너뛴다. */
		curr_pos += 7;
		/* [한국어] 남은 길이도 맞춘다. */
		remaining -= 7;
		/* [한국어] 명령 수신을 동적 디버그 로그로 남긴다. */
		pr_debug("client 0x%p called 'target'\n", priv);
		/* If target is default */
		/* [한국어] "default" 는 시스템 기본 VGA 장치를 뜻하는 예약어다.
		 * Documentation/gpu/vgaarbiter.rst 는 이것을 "(TODO: not implemented yet)"
		 * 으로 적어 두었지만, 이 구현에는 실제로 처리 코드가 있다. */
		if (!strncmp(curr_pos, "default", 7))
			/* [한국어] 기본 장치를 얻고 곧바로 참조를 하나 올린다. vga_default_device()
			 * 는 참조를 주지 않는 조회 함수라, 여기서 잡아 두지 않으면 아래에서 쓰는
			 * 사이에 장치가 사라질 수 있다. 기본 장치가 없으면 pci_dev_get(NULL) 이
			 * NULL 을 돌려주어 pdev 가 NULL 이 된다. */
			pdev = pci_dev_get(vga_default_device());
		/* [한국어] "default" 가 아니면 PCI 주소 문자열로 해석한다. */
		else {
			/* [한국어] "PCI:0000:01:00.0" 형태를 domain/bus/devfn 으로 쪼갠다.
			 * 형식이 어긋나면 프로토콜 위반이다. */
			if (!vga_pci_str_to_vars(curr_pos, remaining,
						 &domain, &bus, &devfn)) {
				ret_val = -EPROTO;
				goto done;
			}
			/* [한국어] 주소로 실제 PCI 장치를 찾는다. 이 함수는 **참조를 하나 올려서**
			 * 돌려주므로, 이후 모든 탈출 경로에서 pci_dev_put() 으로 짝을 맞춰야 한다. */
			pdev = pci_get_domain_bus_and_slot(domain, bus, devfn);
			/* [한국어] 그 주소에 장치가 없다. 참조를 잡지 못했으므로 put 도 필요 없다. */
			if (!pdev) {
				/* [한국어] 어떤 주소가 잘못됐는지 로그에 남긴다. PCI_SLOT/PCI_FUNC 은
				 * devfn 을 다시 장치 번호(상위 5비트)와 기능 번호(하위 3비트)로 쪼개는
				 * 매크로로, 유저가 입력한 형태 그대로 되돌려 보여 주기 위한 것이다. */
				pr_debug("invalid PCI address %04x:%02x:%02x.%x\n",
					 domain, bus, PCI_SLOT(devfn),
					 PCI_FUNC(devfn));
				/* [한국어] -ENODEV = 그 주소에 장치가 없다. 참조를 잡지 못했으므로
				 * 이 경로에서는 pci_dev_put() 이 필요 없다. */
				ret_val = -ENODEV;
				goto done;
			}

			/* [한국어] 조회 성공을 로그에 남긴다. 입력 문자열과 해석 결과, 그리고
			 * 얻은 포인터를 함께 찍어 파싱이 의도대로 됐는지 확인할 수 있게 한다. */
			pr_debug("%s ==> %04x:%02x:%02x.%x pdev %p\n", curr_pos,
				domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn),
				pdev);
		}

		/* [한국어] 조회한 장치가 중재 대상인지 확인한다.
		 * 확인된 사실: 이 호출은 **vga_lock 을 쥐지 않은 상태**에서 이뤄진다.
		 * 이 파일의 다른 모든 vgadev_find() 호출자는 락을 쥐고 부른다. 이 트리 안에
		 * 그 차이를 설명하는 주석이나 근거는 없으며, 코드에 있는 그대로만 적어 둔다. */
		vgadev = vgadev_find(pdev);
		/* [한국어] 찾은 항목(또는 NULL)을 로그에 남긴다. */
		pr_debug("vgadev %p\n", vgadev);
		/* [한국어] VGA 클래스가 아니거나 중재에 등록되지 않은 장치다. */
		if (vgadev == NULL) {
			/* [한국어] pdev 가 NULL 일 수도 있다 -- "target default" 인데 기본 장치가
			 * 없었던 경우다. 그때는 잡은 참조도 없으므로 put 하면 안 된다. */
			if (pdev) {
				/* [한국어] 어떤 장치가 거절됐는지 로그에 남긴다. */
				vgaarb_dbg(&pdev->dev, "not a VGA device\n");
				/* [한국어] 위에서 올린 참조를 반납한다. 이 줄이 없으면 참조가 새어
				 * 그 pci_dev 가 영원히 해제되지 않는다. */
				pci_dev_put(pdev);
			}

			/* [한국어] -ENODEV = 중재 대상이 아니다. */
			ret_val = -ENODEV;
			goto done;
		}

		/* [한국어] 새 대상으로 기록한다.
		 * 주의: 아래에서 이 함수를 벗어나기 전에 pci_dev_put() 으로 참조를 놓아 버리므로,
		 * priv->target 에 남는 포인터는 참조를 갖지 않는 쿠키가 된다. 그 장치가
		 * 나중에 사라지면 이 포인터는 허상이 된다 -- 코드에 있는 그대로 적어 둔다. */
		priv->target = pdev;
		/* [한국어] 이 카드의 슬롯을 찾거나, 없으면 빈 슬롯을 하나 만든다.
		 * 루프를 벗어난 뒤의 i 값으로 "슬롯이 다 찼는가"를 판정하므로, break 없이
		 * 끝까지 돈 경우와 구분된다. */
		for (i = 0; i < MAX_USER_CARDS; i++) {
			/* [한국어] 이미 이 카드의 슬롯이 있다면 그대로 쓰면 된다. */
			if (priv->cards[i].pdev == pdev)
				break;
			/* [한국어] 앞에서부터 처음 만나는 빈 슬롯(pdev == NULL)을 차지한다.
			 * 슬롯은 한 번 채워지면 fd 가 닫힐 때까지 비워지지 않는다. */
			if (priv->cards[i].pdev == NULL) {
				/* [한국어] 슬롯에 카드를 등록한다. */
				priv->cards[i].pdev = pdev;
				/* [한국어] 잠금 장부를 0 에서 시작한다. */
				priv->cards[i].io_cnt = 0;
				/* [한국어] MEM 쪽도 0 으로. */
				priv->cards[i].mem_cnt = 0;
				/* [한국어] 슬롯을 잡았으니 루프 종료. */
				break;
			}
		}
		/* [한국어] 루프가 break 없이 끝났다 -- 빈 슬롯도, 기존 슬롯도 없었다는 뜻,
		 * 즉 이 fd 가 이미 MAX_USER_CARDS 개의 카드를 잡고 있다. */
		if (i == MAX_USER_CARDS) {
			/* [한국어] 상한에 걸렸음을 로그에 남긴다. 상한은 CONFIG_VGA_ARB_MAX_GPUS 다. */
			vgaarb_dbg(&pdev->dev, "maximum user cards (%d) number reached, ignoring this one!\n",
				MAX_USER_CARDS);
			/* [한국어] 잡았던 참조를 반납한다. 슬롯을 못 만들었으므로 대상 지정은
			 * 실패로 처리된다. 다만 위에서 priv->target 에는 이미 대입한 뒤라,
			 * 실패하고도 target 은 바뀐 채 남는다 -- 코드에 있는 그대로다. */
			pci_dev_put(pdev);
			/* XXX: Which value to return? */
			/* [한국어] 상류가 바로 위 "XXX: Which value to return?" 주석으로 스스로
			 * 의문을 남긴 자리다. 자원(슬롯) 부족이라는 뜻으로 -ENOMEM 을 쓴다. */
			ret_val =  -ENOMEM;
			goto done;
		}

		/* [한국어] 성공. */
		ret_val = count;
		/* [한국어] 성공 경로에서도 참조를 놓는다. 위 priv->target 주석에서 말한
		 * "참조 없는 쿠키" 상태가 여기서 만들어진다. */
		pci_dev_put(pdev);
		goto done;


	/* [한국어] 명령 5 -- "decodes <io_state>". 카드의 레거시 해석 능력을 설정한다.
	 * 잠금이 아니라 "이 카드가 무엇을 해석하는가"를 바꾸는 명령이다. */
	} else if (strncmp(curr_pos, "decodes ", 8) == 0) {
		/* [한국어] "decodes " 8바이트를 건너뛴다. */
		curr_pos += 8;
		/* [한국어] 남은 길이도 맞춘다. */
		remaining -= 8;
		/* [한국어] 명령 수신을 동적 디버그 로그로 남긴다. */
		pr_debug("client 0x%p called 'decodes'\n", priv);

		/* [한국어] 자원 이름을 파싱한다. */
		if (!vga_str_to_iostate(curr_pos, remaining, &io_state)) {
			ret_val = -EPROTO;
			goto done;
		}
		/* [한국어] 현재 대상 카드를 꺼낸다. */
		pdev = priv->target;
		/* [한국어] 대상이 없으면 설정할 카드가 없다. */
		if (priv->target == NULL) {
			ret_val = -ENODEV;
			goto done;
		}

		/* [한국어] 세 번째 인자 true = "유저스페이스가 부른 요청". 그래서 커널
		 * 드라이버가 set_decode 콜백을 등록해 둔 카드라면 이 요청은 조용히 무시된다.
		 * void 함수라 무시됐는지 여부는 유저스페이스에 전달되지 않는다. */
		__vga_set_legacy_decoding(pdev, io_state, true);
		/* [한국어] 무시됐더라도 성공으로 보고한다. */
		ret_val = count;
		goto done;
	}
	/* [한국어] 다섯 접두사 중 어느 것에도 맞지 않았다. done 을 거치지 않고
	 * 곧바로 반환하는 유일한 경로다(ret_val 이 초기화되지 않았을 수 있으므로
	 * 여기서 직접 값을 돌려주는 것이 맞다). */
	/* If we got here, the message written is not part of the protocol! */
	return -EPROTO;

/* [한국어] 모든 명령 처리가 모이는 반환 지점. */
done:
	/* [한국어] 각 분기가 채워 둔 결과를 그대로 돌려준다. 양수면 소비한 바이트 수,
	 * 음수면 오류다. */
	return ret_val;
}

/*
 * [한국어]
 * vga_arb_fpoll - /dev/vga_arbiter 의 poll/select 구현
 *
 * @file: 감시 대상 파일.
 * @wait: poll_table. 커널이 이 fd 를 어떤 대기열에 걸어야 하는지 알려 주는 통로다.
 * @return: 준비 상태 비트마스크. 여기서는 항상 EPOLLIN(읽을 것이 있음).
 *
 * 왜 필요한가: 유저스페이스가 카드 상태 변화를 폴링 없이 기다릴 수 있게 한다.
 * 상류 주석대로 "대상 카드뿐 아니라 **어떤** 카드에 변화가 생겨도" 깨어난다 --
 * 전역 대기열 하나를 공유하기 때문이다.
 *
 * 동작: poll_wait() 는 실제로 잠들지 않는다. 이 fd 를 vga_wait_queue 에 등록만
 * 하고 곧바로 돌아온다. 잠드는 일은 VFS 의 poll 코어가 모든 fd 를 등록한 뒤에
 * 한다. 그래서 이 함수 안에는 대기 루프가 없다.
 *
 * 항상 EPOLLIN 을 돌려주는 이유: read() 가 언제든 현재 상태 문자열을 만들어
 * 돌려주므로, 이 장치는 "읽을 것이 없는" 상태가 존재하지 않는다. 그 결과
 * poll() 은 즉시 반환하고, 유저스페이스는 변화 감지를 위해 read() 결과를
 * 직접 비교해야 한다 -- 코드에서 확인되는 동작 그대로다.
 *
 * 실행 컨텍스트: 프로세스 문맥(poll/select 시스템 콜). 락을 잡지 않는다.
 *
 * 호출 체인:
 *   유저스페이스 poll()/select() -> VFS -> [vga_arb_fpoll] -> poll_wait()
 */
static __poll_t vga_arb_fpoll(struct file *file, poll_table *wait)
{
	/* [한국어] 호출 사실만 동적 디버그 로그로 남긴다. */
	pr_debug("%s\n", __func__);

	/* [한국어] 이 fd 를 전역 대기열에 등록한다. __vga_put() 이나
	 * vga_arbiter_del_pci_device() 가 wake_up_all(&vga_wait_queue) 를 부르면
	 * poll() 로 기다리던 프로세스도 함께 깨어난다. */
	poll_wait(file, &vga_wait_queue, wait);
	/* [한국어] 항상 "읽기 가능"으로 답한다. 위 함수 주석 참고. */
	return EPOLLIN;
}

/*
 * [한국어]
 * vga_arb_open - /dev/vga_arbiter 를 여는 open() 구현
 *
 * @inode: 장치 노드의 inode. misc 장치라 여기서 쓰지 않는다.
 * @file: 새로 열린 파일. private_data 에 이 인스턴스의 상태를 매단다.
 * @return: 0 = 성공, -ENOMEM = 인스턴스 상태 할당 실패.
 *
 * 왜 필요한가: 중재기는 잠금의 주인을 fd 단위로 추적한다. 그러려면 fd 마다
 * 독립된 상태(대상 카드와 잠금 장부)가 있어야 하고, 그것을 여기서 만든다.
 * 상류 주석대로 "기본적으로 시스템 기본 VGA 장치에 붙은 채로" 시작한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(open 시스템 콜). 할당은 락 밖에서 GFP_KERNEL 로
 * 하고, 전역 목록에 넣는 동안만 vga_user_lock 을 짧게 잡는다.
 *
 * 에러 경로: 할당 실패면 -ENOMEM. 그 밖에 실패할 수 있는 연산이 없다.
 *
 * 호출 체인:
 *   유저스페이스 open("/dev/vga_arbiter") -> VFS -> misc 코어 -> [vga_arb_open]
 *     -> kzalloc_obj() -> list_add() -> vga_default_device()
 */
static int vga_arb_open(struct inode *inode, struct file *file)
{
	/* [한국어] 이 fd 의 사적 상태. */
	struct vga_arb_private *priv;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;

	/* [한국어] open 이 불렸음을 동적 디버그 로그로 남긴다. */
	pr_debug("%s\n", __func__);

	/* [한국어] kzalloc_obj(*priv) 는 typeof(*priv), 즉 struct vga_arb_private
	 * 크기만큼을 0 으로 채워 할당한다. 0 초기화가 중요한 이유: cards 배열 전체가
	 * pdev == NULL(빈 슬롯)로 시작해야 하기 때문이다. GFP_KERNEL 이라 잠들 수
	 * 있으므로 반드시 아래 spin_lock 보다 앞이어야 한다. */
	priv = kzalloc_obj(*priv);
	/* [한국어] 메모리 부족. 아직 아무것도 등록하지 않았으므로 그냥 반환한다. */
	if (priv == NULL)
		return -ENOMEM;
	/* [한국어] 인스턴스 전용 스핀락을 초기화한다.
	 * 확인된 사실: 이 락을 실제로 잡는(spin_lock 계열) 코드는 이 파일에 없다.
	 * 초기화만 하고 쓰이지 않는다. */
	spin_lock_init(&priv->lock);
	/* [한국어] VFS 가 이후 read/write/release 콜백에 넘겨줄 자리에 매단다.
	 * 이 대입 이후로 file->private_data 가 이 인스턴스의 유일한 소유자다. */
	file->private_data = priv;

	/* [한국어] 전역 인스턴스 목록을 만지므로 락이 필요하다. vga_lock 이 아니라
	 * vga_user_lock 인 점에 주의 -- 두 락은 서로 다른 자료를 지킨다. */
	spin_lock_irqsave(&vga_user_lock, flags);
	/* [한국어] 목록 머리에 넣는다. 순서가 중요하지 않아 add_tail 이 아니다.
	 * (이 목록을 순회하는 코드는 현재 이 파일에 없다.) */
	list_add(&priv->list, &vga_user_list);
	/* [한국어] 목록 조작이 끝났으니 곧바로 푼다. 아래 초기화는 이 fd 만의
	 * 사적 자료라 락이 필요 없다. */
	spin_unlock_irqrestore(&vga_user_lock, flags);

	/* Set the client's lists of locks */
	/* [한국어] 처음 대상은 시스템 기본 VGA 장치다. 옆의 상류 주석이 밝히듯
	 * 아직 NULL 일 수 있다 -- 그러면 이후 명령들이 -ENODEV 로 거절된다.
	 * 참조 카운트를 올리지 않고 그대로 저장한다는 점에 주의(쿠키로만 쓴다). */
	priv->target = vga_default_device(); /* Maybe this is still null! */
	/* [한국어] 0번 슬롯을 그 카드로 미리 채워 둔다. 이렇게 해 두면 유저스페이스가
	 * "target" 명령 없이 곧바로 "lock" 을 보내도 장부에 기록할 슬롯이 있다. */
	priv->cards[0].pdev = priv->target;
	/* [한국어] 아직 아무 잠금도 없으므로 0. kzalloc 이 이미 0 으로 채웠지만
	 * 의도를 드러내려 명시적으로 쓴다. */
	priv->cards[0].io_cnt = 0;
	/* [한국어] MEM 쪽도 마찬가지로 0. */
	priv->cards[0].mem_cnt = 0;

	/* [한국어] 0 = open 성공. */
	return 0;
}

/*
 * [한국어]
 * vga_arb_release - fd 가 닫힐 때 이 인스턴스가 남긴 잠금을 모두 반납한다
 *
 * @inode: 장치 노드의 inode. 쓰지 않는다.
 * @file: 닫히는 파일. private_data 에서 인스턴스 상태를 꺼낸다.
 * @return: 항상 0. 정리는 실패하지 않는다.
 *
 * 왜 필요한가: 이 파일에서 가장 중요한 안전장치다. X 서버 같은 프로그램이
 * VGA 자원을 잠근 채 크래시하면, 그 잠금이 영원히 남아 다른 프로그램이
 * 화면을 쓸 수 없게 된다. 프로세스가 죽으면 커널이 반드시 fd 를 닫아 주므로,
 * 여기서 정리하면 어떤 경우에도 잠금이 새지 않는다. 상류 주석이
 * "able to properly cleanup in all cases when a process dies" 라고 밝힌 것이 이것이다.
 *
 * 동작 단계: 목록에서 자신을 빼고, 슬롯 배열을 훑으며 각 카드에 대해 io_cnt 와
 * mem_cnt 만큼 vga_put() 을 반복해 커널 쪽 중첩 카운터를 정확히 0 으로 되돌린다.
 *
 * 잠금 순서: vga_user_lock 을 쥔 채 vga_put() 을 부르고, vga_put() 은 안에서
 * vga_lock 을 잡는다. 즉 vga_user_lock -> vga_lock 순서로 중첩된다. 이 파일의
 * 다른 어떤 곳도 반대 순서로 잡지 않으므로 잠금 순서 역전(데드락)은 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(마지막 close 또는 프로세스 종료). 스핀락 안에서
 * vga_put() 을 부르지만 그 함수는 잠들지 않으므로 문제없다. kfree 는 락 밖에서 한다.
 *
 * 호출 체인:
 *   유저스페이스 close() / 프로세스 종료 -> VFS -> [vga_arb_release]
 *     -> list_del() -> vga_put() -> __vga_put() -> wake_up_all() -> kfree()
 */
static int vga_arb_release(struct inode *inode, struct file *file)
{
	/* [한국어] 정리할 인스턴스 상태. */
	struct vga_arb_private *priv = file->private_data;
	/* [한국어] 슬롯 순회용 커서. */
	struct vga_arb_user_card *uc;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 슬롯 배열 인덱스. */
	int i;

	/* [한국어] release 가 불렸음을 동적 디버그 로그로 남긴다. */
	pr_debug("%s\n", __func__);

	/* [한국어] 전역 인스턴스 목록과 이 인스턴스의 장부를 다루므로 락을 잡는다. */
	spin_lock_irqsave(&vga_user_lock, flags);
	/* [한국어] 목록에서 자신을 뺀다. 아래에서 priv 를 해제하므로, 그 전에
	 * 반드시 빼야 목록이 해제된 메모리를 가리키지 않는다. */
	list_del(&priv->list);
	/* [한국어] 이 fd 가 손댄 모든 카드 슬롯을 훑는다. */
	for (i = 0; i < MAX_USER_CARDS; i++) {
		/* [한국어] 현재 슬롯의 주소를 잡아 둔다. */
		uc = &priv->cards[i];
		/* [한국어] 빈 슬롯은 건너뛴다. 슬롯은 앞에서부터 채워지지만, 0번이
		 * NULL 인 경우(open 시 기본 장치가 없었던 경우)도 있어 continue 로 넘긴다. */
		if (uc->pdev == NULL)
			continue;
		/* [한국어] 정리 직전의 잔여 잠금 횟수를 로그에 남긴다. 유저스페이스가
		 * 짝을 맞추지 않고 죽었을 때 얼마나 남았는지 확인할 수 있는 단서다. */
		vgaarb_dbg(&uc->pdev->dev, "uc->io_cnt == %d, uc->mem_cnt == %d\n",
			uc->io_cnt, uc->mem_cnt);
		/* [한국어] 남은 IO 잠금 횟수만큼 반복해서 반납한다.
		 * 주의: 후위 감소라 조건이 거짓이 되는 마지막 판정에서도 한 번 더 감소한다.
		 * 따라서 루프가 끝난 뒤 uc->io_cnt 는 0 이 아니라 UINT_MAX 다. 곧바로
		 * priv 를 해제하므로 실제 영향은 없다 -- 코드에 있는 그대로 적어 둔다. */
		while (uc->io_cnt--)
			/* [한국어] 한 번의 vga_put 이 커널 쪽 중첩 카운터를 하나 내린다. 여러 번
			 * 잠갔으면 여러 번 반납해야 잠금이 실제로 풀린다. */
			vga_put(uc->pdev, VGA_RSRC_LEGACY_IO);
		/* [한국어] MEM 쪽의 같은 반복. 역시 끝나면 uc->mem_cnt 는 UINT_MAX 가 된다. */
		while (uc->mem_cnt--)
			vga_put(uc->pdev, VGA_RSRC_LEGACY_MEM);
	}
	/* [한국어] 정리가 끝났으니 락을 푼다. kfree 는 잠들 수 있어 반드시 락 밖이어야 한다. */
	spin_unlock_irqrestore(&vga_user_lock, flags);

	/* [한국어] 인스턴스 상태를 해제한다. 이 시점 이후 file->private_data 는
	 * 허상이 되지만, VFS 가 이 파일에 더는 콜백을 걸지 않으므로 안전하다. */
	kfree(priv);

	/* [한국어] 0 = 정상 종료. release 콜백의 반환값은 close() 결과에 반영된다. */
	return 0;
}

/*
 * Callback any registered clients to let them know we have a change in VGA
 * cards.
 */
/*
 * [한국어]
 * vga_arbiter_notify_clients - 등록된 모든 GPU 클라이언트에게 디코딩 상태를 지시한다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 카드가 한 장뿐이면 중재할 것이 없으므로 그 카드는 레거시
 * 디코딩을 켜 두어도 된다. 두 장 이상이면 충돌 가능성이 있으므로 꺼야 한다.
 * 이 함수가 그 판단을 내려 각 드라이버의 set_decode 콜백으로 전달하고,
 * 드라이버가 실제로 적용한 결과를 되받아 중재 상태에 반영한다.
 *
 * 불리는 시점 세 가지: (1) 중재가 처음 실제로 사용될 때
 * (vga_check_first_use), (2) VGA 장치가 추가될 때, (3) 제거될 때.
 * (2)와 (3)은 pci_notify() 가 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **이 함수가 vga_lock 을 직접 잡는다.** 따라서
 * 호출자는 그 락을 쥐고 있으면 안 된다 -- vga_check_first_use() 와 pci_notify()
 * 모두 락 밖에서 부른다.
 *
 * 중요한 제약: set_decode 콜백이 **스핀락을 쥔 채** 호출된다. 그러므로 GPU
 * 드라이버의 구현은 잠들 수 없고, 오래 걸리는 작업을 해서도 안 된다.
 *
 * 에러 경로: 없다. 콜백이 무엇을 돌려주든 그대로 반영한다.
 *
 * 호출 체인:
 *   vga_check_first_use() / pci_notify() -> [vga_arbiter_notify_clients]
 *     -> vgadev->set_decode() -> vga_update_device_decodes()
 */
static void vga_arbiter_notify_clients(void)
{
	/* [한국어] 장치 목록 순회 커서. */
	struct vga_device *vgadev;
	/* [한국어] spin_lock_irqsave 가 보존할 인터럽트 플래그. */
	unsigned long flags;
	/* [한국어] 드라이버 콜백이 돌려준, 실제로 적용된 해석 능력 비트마스크. */
	unsigned int new_decodes;
	/* [한국어] 콜백에 넘길 지시값. true = "디코딩을 켜도 된다",
	 * false = "꺼라". 모든 카드에 같은 값을 넘긴다. */
	bool new_state;

	/* [한국어] 아직 아무도 중재를 요구하지 않았다면 아무것도 하지 않는다.
	 * 파일 위쪽에 설명한 게으른 정책 -- 중재를 쓰지도 않는데 미리 디코딩을 끄면
	 * 옛 X 서버들이 오동작하기 때문이다. vga_check_first_use() 가 플래그를
	 * 먼저 올린 뒤에 이 함수를 부르는 순서가 그래서 중요하다. */
	if (!vga_arbiter_used)
		return;

	/* [한국어] 카드가 두 장 이상이면 false(디코딩을 꺼라), 한 장 이하면
	 * true(켜도 된다). 판단 근거가 vga_count 하나뿐이라는 점이 이 정책의 전부다. */
	new_state = (vga_count > 1) ? false : true;

	/* [한국어] 목록을 순회하고 각 항목의 decodes 를 고치므로 락이 필요하다. */
	spin_lock_irqsave(&vga_lock, flags);
	/* [한국어] 등록된 모든 VGA 장치를 훑는다. */
	list_for_each_entry(vgadev, &vga_list, list) {
		/* [한국어] 콜백을 등록한 클라이언트가 있는 카드만 대상이다. 등록하지 않은
		 * 카드는 스스로 디코딩을 조절할 수단이 없으므로 건드리지 않는다. */
		if (vgadev->set_decode) {
			/* [한국어] 드라이버에게 지시하고 실제 적용 결과를 받는다.
			 * 스핀락을 쥔 채로 부르는 콜백이므로 드라이버는 잠들면 안 된다. */
			new_decodes = vgadev->set_decode(vgadev->pdev,
							 new_state);
			/* [한국어] 드라이버가 알려 준 새 해석 능력을 중재 상태에 반영한다.
			 * 이 함수는 락을 쥔 채 불러야 하는데, 지금이 정확히 그 상태다. */
			vga_update_device_decodes(vgadev, new_decodes);
		}
	}
	/* [한국어] 순회가 끝났으니 락을 푼다. */
	spin_unlock_irqrestore(&vga_lock, flags);
}

/*
 * [한국어]
 * pci_notify - PCI 버스 알림자 콜백. VGA 장치의 추가/제거를 중재기에 반영한다
 *
 * @nb: 등록에 쓴 notifier_block. 여기서는 쓰지 않는다(전역 pci_notifier 하나뿐).
 * @action: 무슨 일이 일어났는지. BUS_NOTIFY_ADD_DEVICE / BUS_NOTIFY_DEL_DEVICE
 *   등 PCI 코어가 정의한 값이 온다.
 * @data: 대상 장치의 struct device 포인터.
 * @return: 항상 0. 알림자 체인에서 0 은 "계속 진행하라"는 뜻이며, 이 콜백은
 *   다른 구독자를 막지 않는다.
 *
 * 왜 필요한가: 중재기는 부팅 시점의 스캔만으로는 부족하다. 그 뒤에 열거되는
 * 장치나 핫플러그로 꽂히고 뽑히는 카드까지 따라가야 목록이 정확해진다. 버스
 * 알림자는 PCI 코어가 그 사건을 알려 주는 표준 통로다.
 *
 * 실행 컨텍스트: PCI 코어가 장치를 등록/해제하는 프로세스 문맥에서 불린다.
 * 락을 잡지 않으며, 락은 아래 add/del 함수들이 각자 잡는다.
 *
 * 에러 경로: add 가 실패하면(메모리 부족, 중복) notify 가 false 로 남아
 * 클라이언트 알림을 건너뛴다. 그 외에 오류를 위로 전달하는 경로는 없다.
 *
 * 호출 체인:
 *   PCI 코어(device_add / device_del) -> 버스 알림자 체인 -> [pci_notify]
 *     -> vga_arbiter_add_pci_device() / vga_arbiter_del_pci_device()
 *     -> vga_arbiter_notify_clients()
 */
static int pci_notify(struct notifier_block *nb, unsigned long action,
		      void *data)
{
	/* [한국어] 알림자는 struct device 를 넘기므로 void * 를 되돌린다. */
	struct device *dev = data;
	/* [한국어] struct device 를 감싸고 있는 struct pci_dev 로 복원한다.
	 * to_pci_dev 는 container_of 기반 매크로다. pci_bus_type 에 등록한 알림자라
	 * 여기 오는 장치는 항상 PCI 장치임이 보장된다. */
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] 클라이언트에게 변화를 알려야 하는지 여부. 목록이 실제로 바뀐
	 * 경우에만 true 가 된다 -- 헛된 콜백을 막는다. */
	bool notify = false;

	/* [한국어] 어떤 장치에 대해 알림이 왔는지 동적 디버그 로그로 남긴다. */
	vgaarb_dbg(dev, "%s\n", __func__);

	/* Only deal with VGA class devices */
	/* [한국어] VGA 클래스가 아닌 장치는 이 파일과 무관하다. pci_is_vga() 는
	 * 클래스 코드가 PCI_CLASS_DISPLAY_VGA(03 00) 이거나
	 * PCI_CLASS_NOT_DEFINED_VGA(00 01, 클래스 코드 도입 이전의 VGA)인지 본다
	 * -- 근거는 include/linux/pci.h 의 pci_is_vga() 커널독이다.
	 * 0 을 돌려주는 것은 오류가 아니라 "이 알림은 내 관심사가 아니다"라는 뜻이다. */
	if (!pci_is_vga(pdev))
		return 0;

	/*
	 * For now, we're only interested in devices added and removed.
	 * I didn't test this thing here, so someone needs to double check
	 * for the cases of hot-pluggable VGA cards.
	 */
	/* [한국어] 장치가 추가됐다. 중재 목록에 넣는다. */
	if (action == BUS_NOTIFY_ADD_DEVICE)
		notify = vga_arbiter_add_pci_device(pdev);
	/* [한국어] 장치가 제거된다. 중재 목록에서 뺀다. 위 상류 주석은 핫플러그
	 * 경로가 충분히 시험되지 않았다고 스스로 인정하고 있다. */
	else if (action == BUS_NOTIFY_DEL_DEVICE)
		notify = vga_arbiter_del_pci_device(pdev);

	/* [한국어] 목록이 실제로 바뀐 경우에만 클라이언트에게 알린다. 카드 수가
	 * 바뀌면 vga_arbiter_notify_clients() 의 new_state 판정이 달라지므로,
	 * 각 드라이버의 디코딩 상태를 다시 맞춰 주어야 한다. */
	if (notify)
		vga_arbiter_notify_clients();
	/* [한국어] 0 = NOTIFY_DONE 과 같은 값으로, 알림자 체인을 정상 진행시킨다. */
	return 0;
}

/* [한국어] PCI 버스 알림자 등록용 서술자. 전역 하나뿐이며
 * vga_arb_device_init() 이 bus_register_notifier() 로 등록한다. 해제하는
 * 코드는 없다 -- 이 파일은 모듈 언로드를 지원하지 않기 때문이다
 * (module_exit 가 없다). */
static struct notifier_block pci_notifier = {
	/* [한국어] 사건이 생길 때마다 불릴 콜백. 지정 초기화자(.notifier_call)라
	 * 구조체의 다른 필드(priority, next)는 0 으로 남는다. */
	.notifier_call = pci_notify,
};

/* [한국어] /dev/vga_arbiter 의 파일 연산 표. VFS 가 유저스페이스의
 * 시스템 콜을 여기 실린 함수들로 연결한다. const 인 이유는 등록 후 바뀌지
 * 않기 때문이며, 읽기 전용 섹션에 놓여 변조를 막는다. */
static const struct file_operations vga_arb_device_fops = {
	/* [한국어] read() -- 현재 대상 카드의 상태 문자열을 돌려준다. */
	.read = vga_arb_read,
	/* [한국어] write() -- "target"/"lock"/"trylock"/"unlock"/"decodes" 명령을 받는다. */
	.write = vga_arb_write,
	/* [한국어] poll()/select() -- 카드 상태 변화 감시. 항상 EPOLLIN 을 돌려준다. */
	.poll = vga_arb_fpoll,
	/* [한국어] open() -- fd 마다 독립된 인스턴스 상태를 만든다. */
	.open = vga_arb_open,
	/* [한국어] close() -- 남은 잠금을 자동 반납한다. 이 파일의 핵심 안전장치다. */
	.release = vga_arb_release,
	/* [한국어] lseek() -- noop_llseek 은 오프셋을 바꾸지 않고 성공만 돌려주는
	 * 표준 헬퍼다(include/linux/fs.h). 이 장치는 오프셋 개념이 없는 스트림이라,
	 * 아무것도 지정하지 않아 -ESPIPE 가 나가게 두는 대신 무해한 성공을 준다. */
	.llseek = noop_llseek,
};

/* [한국어] misc 문자 장치 등록 서술자. misc 프레임워크는 메이저 번호 10 을
 * 공유하며, 전용 메이저를 받을 만큼 크지 않은 장치들이 쓴다. */
static struct miscdevice vga_arb_device = {
	/* [한국어] 지정 초기화자가 아닌 **위치 초기화자**다. struct miscdevice 의
	 * 필드 순서(include/linux/miscdevice.h)가 { int minor; const char *name;
	 * const struct file_operations *fops; ... } 이므로 각각
	 *   MISC_DYNAMIC_MINOR -- 마이너 번호를 커널이 알아서 고르라는 표시,
	 *   "vga_arbiter"      -- /dev 아래에 만들어질 노드 이름,
	 *   &vga_arb_device_fops -- 위에서 만든 파일 연산 표
	 * 로 대응된다. 나머지 필드는 0/NULL 로 남는다. */
	MISC_DYNAMIC_MINOR, "vga_arbiter", &vga_arb_device_fops
};

/*
 * [한국어]
 * vga_arb_device_init - VGA 중재기의 초기화 진입점
 *
 * @return: misc_register() 의 결과. 0 = 성공, 음수 = /dev 노드 등록 실패.
 *   initcall 의 반환값은 부팅을 멈추지 않으며, 실패해도 아래 작업은 이미
 *   수행된 뒤다(코드에 있는 그대로).
 *
 * 왜 필요한가: 중재기가 일을 하려면 (1) 유저스페이스 창구를 열고,
 * (2) 앞으로 나타날 장치를 감시하고, (3) 이미 열거된 장치를 모두 수집해야 한다.
 * 이 함수가 그 셋을 순서대로 한다.
 *
 * 왜 subsys_initcall_sync 인가: PCI 버스 열거는 subsys_initcall 단계에서
 * 일어난다. _sync 는 그 단계의 비동기 작업까지 끝난 뒤에 실행하라는 뜻이므로,
 * 이 시점에는 부팅 시 존재하던 PCI 장치가 모두 열거를 마친 상태다. 그래서
 * 아래 3단계 전수 스캔이 빠짐없이 동작한다. 동시에 대부분의 장치 드라이버
 * probe(device_initcall 단계)보다는 앞이라, GPU 드라이버가
 * vga_client_register() 를 부를 때 중재기는 이미 준비돼 있다.
 *
 * 실행 컨텍스트: 부팅 초기, 프로세스 문맥(init 커널 스레드). 이 함수 자체는
 * 락을 잡지 않고, vga_arbiter_add_pci_device() 가 각자 잡는다.
 *
 * 등록만 있고 해제가 없다: 이 파일에는 module_exit 나 __exit 함수가 없다.
 * CONFIG_VGA_ARB 가 bool(모듈 불가)이라 언로드 경로가 필요 없기 때문이다.
 *
 * 호출 체인:
 *   커널 initcall 실행기 -> [vga_arb_device_init]
 *     -> misc_register() -> bus_register_notifier() -> pci_get_subsys()
 *     -> pci_is_vga() -> vga_arbiter_add_pci_device()
 */
static int __init vga_arb_device_init(void)
{
	/* [한국어] misc_register() 의 결과이자 이 initcall 의 반환값. */
	int rc;
	/* [한국어] 아래 전수 스캔의 순회 커서. */
	struct pci_dev *pdev;

	/* [한국어] 1단계 -- /dev/vga_arbiter 를 만든다. 이 순간부터 유저스페이스가
	 * 열 수 있다. */
	rc = misc_register(&vga_arb_device);
	/* [한국어] 등록 실패는 로그만 남기고 계속 진행한다. 유저스페이스 창구가
	 * 없어도 커널 내부 중재(vga_get/vga_put)는 정상 동작하기 때문이다. */
	if (rc < 0)
		pr_err("error %d registering device\n", rc);

	/* [한국어] 2단계 -- PCI 버스에 알림자를 건다. 이 시점 이후에 추가되거나
	 * 제거되는 장치는 pci_notify() 를 통해 자동으로 반영된다.
	 * 반환값을 검사하지 않는다는 점은 코드에 있는 그대로다. */
	bus_register_notifier(&pci_bus_type, &pci_notifier);

	/* Add all VGA class PCI devices by default */
	/* [한국어] 3단계 준비 -- pci_get_subsys() 는 이전 위치를 인자로 받아 다음
	 * 장치를 돌려주는 순회 API 이므로, 처음에는 NULL 로 시작한다. */
	pdev = NULL;
	/* [한국어] 시스템의 모든 PCI 장치를 하나씩 훑는다. PCI_ANY_ID 를 네 번
	 * 넘겨 vendor/device/subvendor/subdevice 를 모두 "무엇이든"으로 두었으므로
	 * 전수 순회가 된다. 이 함수는 돌려주는 장치의 참조를 올리고, 다음 호출 때
	 * 이전 장치의 참조를 대신 내려 준다 -- 그래서 루프 안에 pci_dev_put() 이
	 * 없어도 참조가 새지 않고, 루프가 NULL 로 끝나면 마지막 참조도 정리된다. */
	while ((pdev =
		pci_get_subsys(PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
			       PCI_ANY_ID, pdev)) != NULL) {
		/* [한국어] VGA 클래스인 장치만 중재 대상이다. */
		if (pci_is_vga(pdev))
			/* [한국어] 부팅 시점에 이미 존재하던 카드들을 목록에 넣는다. 알림자는
			 * 이 시점 **이후**의 사건만 알려 주므로, 이 스캔이 없으면 부팅 때부터
			 * 꽂혀 있던 카드가 중재에서 빠진다. */
			vga_arbiter_add_pci_device(pdev);
	}

	/* [한국어] 초기화 완료를 dmesg 에 남긴다. pr_fmt 덕분에 "vgaarb: loaded" 로 찍힌다. */
	pr_info("loaded\n");
	/* [한국어] misc_register() 의 결과를 그대로 돌려준다. 실패했더라도 알림자
	 * 등록과 전수 스캔은 이미 끝난 뒤다. */
	return rc;
}
/* [한국어] 이 함수를 커널 초기화 순서의 subsys 단계 끝(sync)에 실행하도록
 * 등록한다. 위 함수 주석에서 설명한 타이밍 요구를 만족시키는 장치다. */
subsys_initcall_sync(vga_arb_device_init);
