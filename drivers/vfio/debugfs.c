// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, HiSilicon Ltd.
 */

/*
 * [한국어 설명] VFIO 디바이스 디버그 파일시스템 노출 (drivers/vfio/debugfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 VFIO 코어가 등록한 각 vfio_device 의 런타임 상태를
 * `debugfs` 가상 파일시스템(/sys/kernel/debug/vfio/<dev>/...) 으로 사용자
 * 공간에 노출하는 read-only 진단 인터페이스를 제공한다. VFIO 의 핵심
 * 기능이 아니라 "디버그/관찰용" 부속 모듈이므로, debugfs 가 비활성화
 * (CONFIG_DEBUG_FS=n) 인 환경에서는 이 파일이 통째로 빌드되지 않을 수
 * 있다. 노출하는 항목은 두 가지로 한정된다 — (1) 마이그레이션 상태머신의
 * 현재 상태(STOP/RUNNING/STOP_COPY/RESUMING/PRE_COPY 등 8개),
 * (2) 이 디바이스가 지원하는 마이그레이션 capability 비트맵
 * (stop-copy, p2p, pre-copy, dirty-tracking). QEMU/libvirt 같은
 * 사용자 공간이 마이그레이션 트레이스를 확인하거나, 개발자가
 * VFIO_DEVICE_FEATURE_MIGRATION ioctl 동작을 검증할 때 이 디렉토리를
 * 들여다본다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VFIO 는 커널의 디바이스 패스스루 프레임워크로, 다음 계층 구조를 갖는다:
 *
 *   QEMU/DPDK (사용자 공간)
 *      │ ioctl(VFIO_DEVICE_FEATURE, MIGRATION_GET_STATE/...)
 *      ▼
 *   /dev/vfio/<group>  또는  /dev/vfio/devices/vfio<N>  (cdev)
 *      │ vfio_device_fops_unl_ioctl (vfio_main.c)
 *      ▼
 *   vfio_device (코어 객체)  ◄── 이 파일이 device 별 debugfs dentry 부착
 *      │ vdev->mig_ops->migration_get_state()
 *      ▼
 *   버스 드라이버 (vfio-pci-core / mdev / cdx ...)
 *      │
 *      ▼
 *   하드웨어 (PF/VF/SR-IOV)
 *
 * 본 파일은 위 계층 중 "코어" 단계에서 측면(out-of-band) 진단 채널을
 * 제공한다. ioctl 핫 패스와 무관하게 read 전용으로 동작하며, 실제 상태
 * 조회는 vdev->mig_ops 콜백을 호출하여 버스 드라이버에 위임한다.
 * 따라서 마이그레이션을 지원하지 않는 디바이스(mig_ops==NULL) 에는
 * "migration/" 서브디렉토리 자체가 만들어지지 않는다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/vfio_main.c
 *      : 이 파일의 4개 외부 진입점(create_root/remove_root/init/exit) 을
 *        호출. 모듈 init 단계에서 create_root, exit 단계에서 remove_root,
 *        디바이스 등록·해제 시 init/exit 를 짝으로 호출한다.
 *  - drivers/vfio/vfio.h
 *      : 본 파일이 구현하는 4개 함수의 prototype 선언. 외부 노출은 이
 *        헤더로 한정.
 *  - include/linux/vfio.h
 *      : struct vfio_device, enum vfio_device_mig_state,
 *        VFIO_MIGRATION_* 플래그, vfio_device_ops/mig_ops/log_ops 등
 *        본 파일이 reflect 하는 모든 타입의 정의처.
 *  - fs/debugfs/*
 *      : debugfs_create_dir / debugfs_create_devm_seqfile /
 *        debugfs_remove_recursive 의 구현처. debugfs 가 disable 일 때는
 *        스텁이 inline 으로 NULL 반환하며 이 파일도 컴파일 가드를 따른다.
 *  - 버스 드라이버 (lib/vfio_pci_core, mlx5/main.c 등)
 *      : 실제 vdev->mig_ops->migration_get_state 콜백 구현체.
 *        본 파일은 그 결과를 문자열로 변환만 한다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_debugfs_root (정적 변수)
 *      : 모든 vfio_device 가 매다는 루트 dentry. 단일 전역.
 *  - vfio_debugfs_create_root() / remove_root()
 *      : 모듈 init/exit 시 한 번씩 호출되는 루트 디렉토리 생성/제거.
 *  - vfio_device_debugfs_init(vdev) / debugfs_exit(vdev)
 *      : 디바이스별 dentry 생성/제거. mig_ops 가 있을 때만
 *        "migration/state", "migration/features" 두 파일을 추가.
 *  - vfio_device_state_read() / features_read()
 *      : seq_file read 콜백. enum 값을 문자열로, 비트마스크를
 *        라인 단위 토큰으로 출력.
 */

#include <linux/device.h>		/* [한국어] struct device, dev_name(): vdev->device 의 sysfs 이름을 dentry 라벨로 사용. */
#include <linux/debugfs.h>		/* [한국어] debugfs_create_dir/_devm_seqfile/_remove_recursive: 본 파일의 핵심 의존. CONFIG_DEBUG_FS=n 시 inline 스텁. */
#include <linux/seq_file.h>		/* [한국어] struct seq_file, seq_puts: seq_file_op→read 콜백에서 텍스트 누적용 버퍼 API. */
#include <linux/vfio.h>		/* [한국어] struct vfio_device, enum vfio_device_mig_state, VFIO_MIGRATION_* 플래그 정의. */
#include "vfio.h"			/* [한국어] 본 파일이 export 하는 4개 함수의 prototype + vdev->debug_root 필드 가시성. */

/* [한국어] vfio 모듈 전역 debugfs 루트 dentry.
 * 설정자: vfio_debugfs_create_root() — 모듈 init 단계에서 한 번 호출.
 * 읽는 자: vfio_device_debugfs_init() 가 디바이스별 서브 dentry 의 부모로 사용.
 * 값 범위: NULL(모듈 unload 후 또는 debugfs disable) 또는 유효한 dentry.
 * 동기화: 단일 모듈 init/exit 경로에서만 변경되므로 별도 락 불필요. */
static struct dentry *vfio_debugfs_root;

/*
 * [한국어]
 * vfio_device_state_read - 디바이스의 마이그레이션 상태머신 현재 상태를
 *                          /sys/kernel/debug/vfio/<dev>/migration/state 로 출력
 *
 * @seq:  seq_file 컨텍스트. seq->private 에는 debugfs_create_devm_seqfile()
 *        가 등록 시 전달한 device 포인터(vdev->device)가 들어 있다.
 * @data: 사용하지 않음 (debugfs seq_file 시그니처 호환용).
 * @return: 0 = 성공, -EINVAL = 버스 드라이버 콜백이 실패한 경우.
 *
 * 호출 컨텍스트: 사용자 공간이 해당 파일을 read(2) 할 때 VFS → debugfs →
 *   seq_file → 본 함수의 1회 호출 흐름으로 진입. 프로세스 컨텍스트이므로
 *   슬립 가능하나, 마이그레이션 상태 조회는 일반적으로 vendor cmdq 를
 *   거치지 않고 메모리 내 변수만 반환하므로 빠르다.
 *
 * 동작 단계:
 *  1. seq->private 에서 struct device 포인터 추출.
 *  2. container_of() 로 device 를 감싸는 vfio_device 객체로 환산.
 *     (vfio_device.device 는 내장(non-pointer) 필드이므로 이 변환이 안전)
 *  3. BUILD_BUG_ON 으로 enum 값 개수 일관성 컴파일타임 검증.
 *     PRE_COPY_P2P(=7) + 1 = 8 = VFIO_DEVICE_STATE_NR 가 깨지면(즉 새
 *     상태가 추가되었는데 switch 가 갱신되지 않았다면) 빌드 실패시켜
 *     누락 케이스를 강제로 드러낸다.
 *  4. vdev->mig_ops->migration_get_state(vdev, &state) 호출.
 *     이 콜백은 버스 드라이버(vfio-pci-core / mlx5 등) 가 구현하며,
 *     하드웨어가 가진 현재 마이그레이션 phase 를 돌려준다.
 *  5. enum 값을 사람이 읽을 수 있는 토큰으로 매핑하여 seq_puts.
 *     ERROR/STOP/RUNNING/STOP_COPY/RESUMING/RUNNING_P2P/PRE_COPY/PRE_COPY_P2P
 *     의 8개 + default("Invalid") 보호.
 *
 * 호출 체인:
 *   userspace read(/sys/kernel/debug/vfio/<dev>/migration/state)
 *     → debugfs seq_file infrastructure
 *     → [vfio_device_state_read]
 *     → vdev->mig_ops->migration_get_state
 *     → vendor 드라이버의 실제 상태 추출 (예: vfio-pci-core 의
 *       vfio_pci_core_mig_get_state)
 */
static int vfio_device_state_read(struct seq_file *seq, void *data)
{
	struct device *vf_dev = seq->private;					/* [한국어] debugfs_create_devm_seqfile() 등록 시 전달된 dev 인자가 그대로 들어옴 — 본 파일의 init 함수가 &vdev->device 로 등록. */
	struct vfio_device *vdev = container_of(vf_dev,				/* [한국어] device 가 구조체 내장 필드이므로 container_of 매크로로 부모 vfio_device 주소를 안전하게 역산. */
						struct vfio_device, device);
	enum vfio_device_mig_state state;					/* [한국어] 콜백이 채워줄 출력 변수. uninit 으로 두고 success 시에만 사용. */
	int ret;								/* [한국어] 콜백 반환값. 0 이외는 EINVAL 로 사용자 공간에 전달. */

	BUILD_BUG_ON(VFIO_DEVICE_STATE_NR !=					/* [한국어] enum 끝값과 가산식이 어긋나면 컴파일 실패 — 새 상태 추가 시 본 switch 도 같이 갱신하라는 강제 alarm. */
		     VFIO_DEVICE_STATE_PRE_COPY_P2P + 1);

	ret = vdev->mig_ops->migration_get_state(vdev, &state);			/* [한국어] 버스 드라이버가 보유한 현재 phase 조회. mig_ops 는 init 시 NULL 검증을 했기에 여기서는 deref 안전. */
	if (ret)								/* [한국어] 콜백 실패(예: 디바이스 hot-removed) 시 사용자에게 EINVAL 로 단일화하여 응답 — debugfs 는 detail 을 숨겨도 무방. */
		return -EINVAL;

	switch (state) {							/* [한국어] enum → 문자열 매핑. case 빠짐 없이 8개 + default 로 미래 확장 안전망. */
	case VFIO_DEVICE_STATE_ERROR:						/* [한국어] 하드웨어/벤더가 복구 불가 에러로 진입. 호스트 정책상 디바이스 reset 필요. */
		seq_puts(seq, "ERROR\n");
		break;
	case VFIO_DEVICE_STATE_STOP:						/* [한국어] DMA/인터럽트 모두 정지된 안정 상태. 마이그레이션 시작 직후·복구 후 통과 지점. */
		seq_puts(seq, "STOP\n");
		break;
	case VFIO_DEVICE_STATE_RUNNING:						/* [한국어] 게스트가 정상 동작. 디폴트 phase. */
		seq_puts(seq, "RUNNING\n");
		break;
	case VFIO_DEVICE_STATE_STOP_COPY:					/* [한국어] STOP 이후 디바이스 상태를 stream 으로 추출 중 — destination 으로 전송할 라이브 페이지 직렬화 단계. */
		seq_puts(seq, "STOP_COPY\n");
		break;
	case VFIO_DEVICE_STATE_RESUMING:					/* [한국어] destination 측에서 stream 을 받아 재구성 중 — 아직 디바이스 미가동. */
		seq_puts(seq, "RESUMING\n");
		break;
	case VFIO_DEVICE_STATE_RUNNING_P2P:					/* [한국어] RUNNING 과 거의 동일하지만 P2P DMA 만 격리(quiesce). 다른 디바이스로의 직접 DMA 차단. */
		seq_puts(seq, "RUNNING_P2P\n");
		break;
	case VFIO_DEVICE_STATE_PRE_COPY:					/* [한국어] live 마이그레이션 중 dirty page 만 반복 전송하는 워밍업 phase. 게스트는 계속 동작. */
		seq_puts(seq, "PRE_COPY\n");
		break;
	case VFIO_DEVICE_STATE_PRE_COPY_P2P:					/* [한국어] PRE_COPY + P2P quiesce. peer device 와의 race 차단. */
		seq_puts(seq, "PRE_COPY_P2P\n");
		break;
	default:								/* [한국어] enum 외 값 — 메모리 corruption 또는 ABI 미스매치. 안전하게 "Invalid" 보고. */
		seq_puts(seq, "Invalid\n");
	}

	return 0;								/* [한국어] seq_file 관례: 단일 read 가 완전히 채웠으므로 0. */
}

/*
 * [한국어]
 * vfio_device_features_read - 디바이스가 알리는 마이그레이션 capability
 *                             비트맵을 라인별 토큰으로 덤프
 *
 * @seq:  seq_file 컨텍스트. seq->private = &vdev->device.
 * @data: 사용하지 않음.
 * @return: 항상 0. 비활성 capability 는 단순히 출력하지 않는다.
 *
 * 본 함수는 콜백 호출 없이 vdev 가 등록 시점에 고정해 둔 플래그
 * (vdev->migration_flags) 와 log_ops 포인터 유무만 읽으므로 락 불필요.
 * 출력 형식은 "라인당 토큰 1개" 로 사용자 공간 파서가 readline+set 로
 * 검사하기 쉽도록 설계됐다.
 *
 *  - "stop-copy"      : VFIO_MIGRATION_STOP_COPY (필수, 모든 마이그레이션 디바이스가 보유)
 *  - "p2p"            : VFIO_MIGRATION_P2P (P2P DMA quiesce 가능)
 *  - "pre-copy"       : VFIO_MIGRATION_PRE_COPY (live migration warmup 지원)
 *  - "dirty-tracking" : log_ops 가 등록된 경우 (페이지 쓰기 트래킹 가능)
 *
 * 호출 체인: read(features) → seq_file → [본 함수].
 */
static int vfio_device_features_read(struct seq_file *seq, void *data)
{
	struct device *vf_dev = seq->private;					/* [한국어] state 와 동일 패턴 — 등록 시 device 포인터를 그대로 회수. */
	struct vfio_device *vdev = container_of(vf_dev, struct vfio_device, device); /* [한국어] device → vfio_device 역산. */

	if (vdev->migration_flags & VFIO_MIGRATION_STOP_COPY)			/* [한국어] 가장 기본 capability. 등록 시 mig_ops 가 있으면 거의 항상 set. */
		seq_puts(seq, "stop-copy\n");
	if (vdev->migration_flags & VFIO_MIGRATION_P2P)			/* [한국어] P2P DMA quiesce 지원 — RUNNING_P2P / PRE_COPY_P2P 진입 가능 여부. */
		seq_puts(seq, "p2p\n");
	if (vdev->migration_flags & VFIO_MIGRATION_PRE_COPY)			/* [한국어] PRE_COPY phase 지원 — 다운타임 단축의 핵심 기능. */
		seq_puts(seq, "pre-copy\n");
	if (vdev->log_ops)							/* [한국어] 별도 vtable 인 log_ops 가 등록됐으면 dirty page tracking 가능. flag 가 아닌 포인터 유무로 판정. */
		seq_puts(seq, "dirty-tracking\n");

	return 0;								/* [한국어] 모든 비트가 off 여도 빈 파일을 반환 — 사용자 공간은 EOF 로 "no caps" 추론. */
}

/*
 * [한국어]
 * vfio_device_debugfs_init - 디바이스별 debugfs 디렉토리/파일 생성
 *
 * @vdev: 코어가 등록 처리 중인 vfio_device 객체. 호출 시점에 device->kobj
 *        는 sysfs 에 add 된 후이므로 dev_name() 결과가 유효하다.
 *
 * 호출 시점: vfio_device_register / __vfio_register_dev (vfio_main.c) 가
 *   디바이스 sysfs 등록을 끝낸 직후. mig_ops 가 NULL 이면 migration
 *   디렉토리를 만들지 않으므로 비-마이그레이션 디바이스에서도 안전.
 *
 * 동작:
 *  1. /sys/kernel/debug/vfio/<dev_name>/ 디렉토리 생성 (root 의 자식).
 *     dev_name() 은 sysfs 의 마지막 컴포넌트(예: "0000:01:00.0") 를 돌려준다.
 *     반환된 dentry 는 vdev->debug_root 에 보관 — exit 에서 통째로 제거.
 *  2. mig_ops 가 있을 때만 "migration/" 서브디렉토리 + 그 아래
 *     "state", "features" 두 seq_file 등록.
 *
 * 에러 처리: debugfs_create_* API 들은 실패 시 PTR_ERR 를 반환하지만
 * 본 함수는 그것을 검사하지 않는다. 이는 의도적이다 — debugfs 가
 * disable 또는 실패해도 VFIO 본 기능은 무관하므로 graceful degradation.
 * 후속 remove_recursive 가 ERR_PTR 도 안전하게 처리한다.
 *
 * 호출 체인:
 *   __vfio_register_dev() (vfio_main.c)
 *     → [vfio_device_debugfs_init]
 *       → debugfs_create_dir, debugfs_create_devm_seqfile (fs/debugfs)
 */
void vfio_device_debugfs_init(struct vfio_device *vdev)
{
	struct device *dev = &vdev->device;					/* [한국어] devm_seqfile 등록에 device 포인터가 필요 — 디바이스 lifetime 에 자동 cleanup 묶기 위함. */

	vdev->debug_root = debugfs_create_dir(dev_name(vdev->dev),		/* [한국어] vdev->dev 는 백킹 PCI/플랫폼 device. dev_name 으로 unique key("0000:bb:dd.f" 같은) 부여. */
					      vfio_debugfs_root);

	if (vdev->mig_ops) {							/* [한국어] 마이그레이션 미지원 디바이스는 여기서 종료 — 디렉토리 자체만 빈 채로 유지. */
		struct dentry *vfio_dev_migration = NULL;			/* [한국어] 임시 dentry — recursive remove 에서 부모(debug_root) 가 함께 지우므로 별도 보관 불필요. */

		vfio_dev_migration = debugfs_create_dir("migration",		/* [한국어] VFIO migration uAPI 의 정확한 mirror 가 되도록 이름을 "migration" 으로 고정. */
							vdev->debug_root);
		debugfs_create_devm_seqfile(dev, "state", vfio_dev_migration,	/* [한국어] devm_* 변형: device 가 unregister 될 때 자동 free. exit 누락 시에도 안전. */
					    vfio_device_state_read);
		debugfs_create_devm_seqfile(dev, "features", vfio_dev_migration, /* [한국어] features 는 정적 — read 콜백이 vdev 필드만 읽고 끝. */
					    vfio_device_features_read);
	}
}

/*
 * [한국어]
 * vfio_device_debugfs_exit - 디바이스 unregister 시 debugfs 트리 제거
 *
 * @vdev: 제거 대상 vfio_device.
 *
 * debugfs_remove_recursive 는 (a) NULL, (b) ERR_PTR, (c) 정상 dentry
 * 모두 안전하게 처리하므로 init 에서 실패한 경우에도 추가 검사 없이
 * 호출할 수 있다. 호출 시점은 vfio_device_unregister() 가 sysfs/cdev
 * 모두 떼낸 직후이며, 이 시점에는 read 콜백이 진입할 수 없도록
 * VFS dentry inode 가 풀려 있어야 한다(remove_recursive 가 동기화 보장).
 *
 * 호출 체인: vfio_unregister_device() (vfio_main.c) → [본 함수].
 */
void vfio_device_debugfs_exit(struct vfio_device *vdev)
{
	debugfs_remove_recursive(vdev->debug_root);				/* [한국어] migration/ 와 그 안의 state·features 를 한 번에 제거 — devm 등록 콜백도 dentry 제거 시 안전 종료. */
}

/*
 * [한국어]
 * vfio_debugfs_create_root - 모듈 init 시 단 한 번 호출되는 루트 생성
 *
 * /sys/kernel/debug/vfio 디렉토리를 만든다. 부모를 NULL 로 주면 debugfs
 * mount point 의 최상위 자식이 된다. 반환된 dentry 는 모듈-스코프 정적
 * 변수에 저장되어 이후 모든 device 가 부모로 사용한다.
 *
 * 호출 체인: vfio_init() (vfio_main.c, module_init) → [본 함수].
 */
void vfio_debugfs_create_root(void)
{
	vfio_debugfs_root = debugfs_create_dir("vfio", NULL);			/* [한국어] CONFIG_DEBUG_FS=n 또는 mount 안 된 환경이면 NULL 또는 ERR_PTR — 후속 자식 생성도 자동으로 noop 됨. */
}

/*
 * [한국어]
 * vfio_debugfs_remove_root - 모듈 exit 시 단 한 번 호출되는 루트 제거
 *
 * 모든 device 가 unregister 된 이후 호출되어야 일관성이 유지된다.
 * (vfio_main.c 의 module_exit 가 디바이스 정리를 먼저 수행하는 순서를
 * 책임진다.) recursive 제거이지만 이 시점에는 자식이 없는 상태가 정상.
 * 변수에 NULL 을 다시 채워 unload→reload 가 idempotent 하도록 보장.
 */
void vfio_debugfs_remove_root(void)
{
	debugfs_remove_recursive(vfio_debugfs_root);				/* [한국어] 만에 하나 자식이 남아 있어도 강제 제거 — VFS lock 으로 race 차단. */
	vfio_debugfs_root = NULL;						/* [한국어] reload 대비 — 다시 init 호출되면 새 dentry 로 깨끗하게 시작. */
}
