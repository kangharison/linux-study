// SPDX-License-Identifier: GPL-2.0

/*
 * 파일 상단 요약 (NVMe 관점)
 *
 * bsg.c는 블록 계층에서 SCSI generic(sg) v4 인터페이스를 구현하여
 * 사용자 공간이 /dev/bsg/* 장치를 통해 원시 명령을 전달할 수 있게 한다.
 * NVMe SSD 입장에서는 request_queue에 등록된 bsg 콜백
 * (예: nvme_bsg_sg_io, nvme_bsg_uring_cmd)을 경유하여
 * 사용자 공간의 Admin/IO 명령이 컨트롤러의 Submission Queue(SQ)에
 * 도달하는 관문(gateway) 역할을 수행한다.
 * 흐름: write/ioctl(/dev/bsg/nvme*) -> bsg -> sg_io_fn/uring_cmd_fn
 *        -> nvme_queue_rq -> nvme_submit_cmd -> doorbell.
 * 선행 파일: block/blk-core.c, block/blk-mq.c, drivers/nvme/host/core.c 등.
 */

/*
 * bsg.c - block layer implementation of the sg v4 interface
 */
#include <linux/module.h>		/* 모듈 초기화; bsg 모듈 등록 시 사용 */
#include <linux/init.h>
#include <linux/file.h>			/* struct file/open/release 등 파일 연산 지원 */
#include <linux/blkdev.h>		/* request_queue, queue_max_bytes, blk_get_queue 등 blk-mq/blk-core 인프라 */
#include <linux/cdev.h>			/* 문자 장치(cdev) 등록; /dev/bsg/* 노드 생성 */
#include <linux/jiffies.h>		/* timeout을 jiffies로 변환; NVMe 명령 만료 시점 계산 */
#include <linux/percpu.h>
#include <linux/idr.h>			/* IDA minor 번호 할당; /dev/bsg/nvme* minor 관리 */
#include <linux/bsg.h>
#include <linux/slab.h>
#include <linux/io_uring/cmd.h>	/* io_uring_cmd; NVMe uring passthrough 진입점 */

#include <scsi/scsi.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>			/* sg_io_v4; 사용자 공간 원시 명령 구조체 */

/* NVMe 드라이버는 drivers/nvme/host/core.c 등에서 bsg_register_queue()를
 * 호출하여 /dev/bsg/* 장치를 request_queue에 연결한다.
 */

#define BSG_DESCRIPTION	"Block layer SCSI generic (bsg) driver"
#define BSG_VERSION	"0.4"

/*
 * NVMe 관점: bsg_device는 /dev/bsg/* 에 대응되며, 커널의 request_queue를
 * 사용자 공간의 SG_IO/io_uring passthrough 명령과 연결한다.
 * 실제 NVMe 명령 변환/제출은 아래 sg_io_fn/uring_cmd_fn 콜백이 담당한다.
 */
struct bsg_device {
	struct request_queue *queue;	/* NVMe 컨트롤러/네임스페이스의 request_queue; blk-mq가 SQ/CQ 매핑에 사용 */
	struct device device;		/* /dev/bsg/ 아래의 sysfs/cdev용 char 장치 */
	struct cdev cdev;		/* 문자 장치 등록; 사용자 공간 open/ioctl 진입점 */
	int max_queue;			/* 최대 동시 passthrough 명령 수; NVMe SQ 깊이보다 작게 제한되지는 않음 (추정) */
	unsigned int timeout;		/* 명령 타임아웃; NVMe 명령이 완료(CQE)되지 않을 때 사용 */
	unsigned int reserved_size;	/* 데이터 버퍼 상한; PRP/SGL 생성 시 사용 가능한 최대 바이트 수 */
	bsg_sg_io_fn *sg_io_fn;		/* ioctl SG_IO 처리 콜백; NVMe 드라이버가 Admin/IO 명령을 변환/제출 */
	bsg_uring_cmd_fn *uring_cmd_fn;	/* io_uring passthrough 콜백; NVMe uring cmd 경로 진입점 (추정) */
};

static inline struct bsg_device *to_bsg_device(struct inode *inode)
{
	/* cdev를 포함하는 bsg_device의 시작 주소를 구한다 */
	return container_of(inode->i_cdev, struct bsg_device, cdev);
}

#define BSG_DEFAULT_CMDS	64	/* NVMe Admin SQ/IO SQ 깊이와는 독립적인 bsg 계층의 기본 동시 명령 한도 */
#define BSG_MAX_DEVS		(1 << MINORBITS)	/* minor 번호 공간; 등록 가능한 /dev/bsg/* 최대 개수 */

static DEFINE_IDA(bsg_minor_ida);	/* minor 번호 할당기; NVMe 컨트롤러/네임스페이스별 bsg 장치 식별 */
static const struct class bsg_class;
static int bsg_major;			/* /dev/bsg/*에 할당된 major 번호; open 시 장치 식별에 사용 */

/*
 * 목적: 사용자가 요청한 timeout 값을 jiffies로 변환하되, 블록 계층의
 *       최소/기본값을 보장한다.
 * 호출 경로: bsg_sg_io -> bsg_timeout
 * NVMe 연결: 변환된 timeout은 NVMe 명령이 CQE를 받지 못했을 때
 *            blk-mq/드라이버의 타임아웃 처리(예: nvme_timeout)로 전달된다 (추정).
 */
static unsigned int bsg_timeout(struct bsg_device *bd, struct sg_io_v4 *hdr)
{
	unsigned int timeout = BLK_DEFAULT_SG_TIMEOUT;

	if (hdr->timeout)
		timeout = msecs_to_jiffies(hdr->timeout);	/* 사용자 지정 timeout(ms)를 jiffies로 변환; NVMe 명령 만료 시각 산출 */
	else if (bd->timeout)
		timeout = bd->timeout;				/* bsg 장치에 설정된 timeout 사용; NVMe controller reset/ERST 대비 (추정) */

	return max_t(unsigned int, timeout, BLK_MIN_SG_TIMEOUT);	/* NVMe 명령 최소 timeout 보장 */
}

/*
 * 목적: SG_IO 요청을 사용자 공간에서 받아 bsg 콜백으로 전달한다.
 * 호출 경로: ioctl(SG_IO) -> bsg_ioctl -> bsg_sg_io
 *              -> bd->sg_io_fn (예: nvme_bsg_sg_io)
 *              -> nvme_queue_rq -> nvme_submit_cmd -> doorbell
 * NVMe 연결: 여기서 전달되는 sg_io_v4가 NVMe Admin/IO 명령으로 변환되며,
 *            결과 CQE가 다시 사용자 공간으로 복사된다.
 */
static int bsg_sg_io(struct bsg_device *bd, bool open_for_write,
		     void __user *uarg)
{
	struct sg_io_v4 hdr;
	int ret;

	if (copy_from_user(&hdr, uarg, sizeof(hdr)))	/* 사용자 공간 SG_IO 헤더를 커널로 복사; NVMe SQE payload 원본 */
		return -EFAULT;
	if (hdr.guard != 'Q')				/* sg v4 프로토콜 가드; 'Q'가 아니면 잘못된 요청 */
		return -EINVAL;
	ret = bd->sg_io_fn(bd->queue, &hdr, open_for_write,	/* NVMe 콜백 진입: Admin/IO 명령 처리 */
			   bsg_timeout(bd, &hdr));
	if (!ret && copy_to_user(uarg, &hdr, sizeof(hdr)))	/* CQE/응답 헤더를 사용자 공간으로 복사 */
		return -EFAULT;
	return ret;						/* 0이면 NVMe 명령이 SQ에 기록된 후 완료 대기까지 처리됨 (추정) */
}

/*
 * 목적: /dev/bsg/* open 시 해당 request_queue를 참조 획득.
 * 호출 경로: open(/dev/bsg/nvme*) -> bsg_open
 * NVMe 연결: blk_get_queue()가 성공해야 NVMe 컨트롤러/네임스페이스가
 *            아직 살아있음을 의미하며, 이후 명령 제출이 가능하다.
 */
static int bsg_open(struct inode *inode, struct file *file)
{
	if (!blk_get_queue(to_bsg_device(inode)->queue))	/* NVMe request_queue 참조 획득 시도; 실패 시 QUEUE_FLAG_DYING 등으로 인해 제출 불가 */
		return -ENXIO;
	return 0;
}

/*
 * 목적: /dev/bsg/* release 시 request_queue 참조 해제.
 * 호출 경로: close(/dev/bsg/nvme*) -> bsg_release
 * NVMe 연결: blk_put_queue()로 request_queue 사용 카운트를 감소시켜
 *            NVMe 장치 제거 시 안전한 해제를 돕는다.
 */
static int bsg_release(struct inode *inode, struct file *file)
{
	blk_put_queue(to_bsg_device(inode)->queue);	/* NVMe request_queue 참조 해제; 카운트 0 시 해제 진행 */
	return 0;
}

/*
 * 목적: 현재 bsg 장치의 최대 동시 명령 개수(max_queue)를 사용자 공간에 반환.
 * 호출 경로: ioctl(SG_GET_COMMAND_Q) -> bsg_ioctl -> bsg_get_command_q
 * NVMe 연결: 이 값은 bsg 계층의 큐 길이 제한일 뿐이며, 실제 NVMe SQ/CQ
 *            깊이는 blk-mq/hardware queue가 관리한다 (추정).
 */
static int bsg_get_command_q(struct bsg_device *bd, int __user *uarg)
{
	return put_user(READ_ONCE(bd->max_queue), uarg);	/* max_queue 원자적 읽기; NVMe 동시 명령 수 상한을 userspace에 보고 */
}

/*
 * 목적: bsg 장치의 최대 동시 명령 개수(max_queue)를 사용자 공간에서 설정.
 * 호출 경로: ioctl(SG_SET_COMMAND_Q) -> bsg_ioctl -> bsg_set_command_q
 * NVMe 연결: 이 값은 NVMe SQ 깊이보다 클 수 있으며, 실제 제출 가능한
 *            명령 수는 nvme_queue_rq 등에서 태그/큐 깊이로 조절된다 (추정).
 */
static int bsg_set_command_q(struct bsg_device *bd, int __user *uarg)
{
	int max_queue;

	if (get_user(max_queue, uarg))		/* 사용자 공간에서 새 max_queue 값 수신 */
		return -EFAULT;
	if (max_queue < 1)				/* 최소 1개 이상의 동시 명령 필요; NVMe SQ entry 소모 보장 */
		return -EINVAL;
	WRITE_ONCE(bd->max_queue, max_queue);	/* bsg 계층 동시 명령 수 갱신; 실제 NVMe 태그 풀 깊이와는 별개 (추정) */
	return 0;
}

/*
 * 목적: /dev/bsg/*에 대한 다양한 ioctl을 처리.
 * 호출 경로: unlocked_ioctl(/dev/bsg/nvme*) -> bsg_ioctl
 *              -> [SG_IO -> bsg_sg_io -> nvme_bsg_sg_io]
 *              -> [SG_SET_TIMEOUT/SG_GET_TIMEOUT 등]
 * NVMe 연결: SG_IO ioctl이 NVMe passthrough의 주요 진입점이며,
 *            나머지 ioctl은 bsg/SCSI 호환성을 위한 메타데이터 조작이다.
 */
static long bsg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct bsg_device *bd = to_bsg_device(file_inode(file));
	struct request_queue *q = bd->queue;			/* NVMe request_queue 포인터; queue_max_bytes 등 조회에 사용 */
	void __user *uarg = (void __user *) arg;
	int __user *intp = uarg;
	int val;

	switch (cmd) {
	/*
	 * Our own ioctls
	 */
	case SG_GET_COMMAND_Q:
		return bsg_get_command_q(bd, uarg);
	case SG_SET_COMMAND_Q:
		return bsg_set_command_q(bd, uarg);

	/*
	 * SCSI/sg ioctls
	 */
	case SG_GET_VERSION_NUM:
		return put_user(30527, intp);				/* sg 드라이버 버전 번호 반환 */
	case SCSI_IOCTL_GET_IDLUN:
		return put_user(0, intp);				/* NVMe에는 SCSI LUN 개념이 없으므로 0 반환 */
	case SCSI_IOCTL_GET_BUS_NUMBER:
		return put_user(0, intp);				/* NVMe에는 SCSI bus 번호 개념이 없으므로 0 반환 */
	case SG_SET_TIMEOUT:
		if (get_user(val, intp))				/* 사용자가 지정한 타임아웃 값 수신 */
			return -EFAULT;
		bd->timeout = clock_t_to_jiffies(val);		/* bsg 장치 단위 timeout 갱신; NVMe 명령 타임아웃/abort 한도로 이어짐 (추정) */
		return 0;
	case SG_GET_TIMEOUT:
		return jiffies_to_clock_t(bd->timeout);		/* 현재 timeout을 clock_t로 반환 */
	case SG_GET_RESERVED_SIZE:
		return put_user(min(bd->reserved_size, queue_max_bytes(q)),
				intp);				/* 허용된 최대 데이터 버퍼 크기 반환; NVMe PRP/SGL/SGE 한도보다 작거나 같음 (추정) */
	case SG_SET_RESERVED_SIZE:
		if (get_user(val, intp))				/* 사용자가 지정한 버퍼 상한 수신 */
			return -EFAULT;
		if (val < 0)
			return -EINVAL;
		bd->reserved_size =
			min_t(unsigned int, val, queue_max_bytes(q));	/* queue_max_bytes를 넘지 않도록 제한; NVMe DMA/SGL 준비 시 buffer 크기 상한 */
		return 0;
	case SG_EMULATED_HOST:
		return put_user(1, intp);				/* sg 호스트 에뮬레이션 상태를 1로 보고 */
	case SG_IO:
		return bsg_sg_io(bd, file->f_mode & FMODE_WRITE, uarg);	/* NVMe passthrough 본 처리; 쓰기 모드 여부도 전달 */
	case SCSI_IOCTL_SEND_COMMAND:
		pr_warn_ratelimited("%s: calling unsupported SCSI_IOCTL_SEND_COMMAND\n",
				current->comm);				/* NVMe 경로에서는 지원하지 않는 SCSI ioctl */
		return -EINVAL;
	default:
		return -ENOTTY;						/* NVMe/bsg 모두 알 수 없는 ioctl */
	}
}

/*
 * 목적: io_uring passthrough에 필요한 128바이트 SQE/32바이트 CQE를
 *       지원하는지 확인한다.
 * 호출 경로: io_uring_cmd -> bsg_uring_cmd -> bsg_check_uring_features
 * NVMe 연결: NVMe 명령 구조(64바이트 SQE + 추가 메타데이터)를 전달하려면
 *            큰 SQE/CQE가 필요하다 (추정).
 */
static int bsg_check_uring_features(unsigned int issue_flags)
{
	/* BSG passthrough requires big SQE/CQE support */
	if ((issue_flags & (IO_URING_F_SQE128|IO_URING_F_CQE32)) !=
	    (IO_URING_F_SQE128|IO_URING_F_CQE32))		/* SQE 128 + CQE 32 비트 모두 설정되어야 NVMe passthrough 가능 */
		return -EOPNOTSUPP;				/* NVMe passthrough에 필요한 uring 기능이 없음 */
	return 0;
}

/*
 * 목적: io_uring 명령을 NVMe uring_cmd_fn 콜백으로 전달.
 * 호출 경로: io_uring_cmd -> bsg_uring_cmd
 *              -> bd->uring_cmd_fn (예: nvme_bsg_uring_cmd) (추정)
 *              -> nvme_queue_rq -> nvme_submit_cmd -> doorbell
 * NVMe 연결: uring 경로로 들어온 NVMe Admin/IO 명령을 SQE에서 꺼내
 *            컨트롤러의 SQ에 기록한다.
 */
static int bsg_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct bsg_device *bd = to_bsg_device(file_inode(ioucmd->file));
	bool open_for_write = ioucmd->file->f_mode & FMODE_WRITE;	/* 쓰기 모드 플래그; NVMe opcode 방향과 연관 (추정) */
	struct request_queue *q = bd->queue;				/* NVMe request_queue; io_uring passthrough 명령의 대상 */
	int ret;

	ret = bsg_check_uring_features(issue_flags);
	if (ret)
		return ret;

	if (!bd->uring_cmd_fn)							/* NVMe 드라이버가 uring 콜백을 등록하지 않은 경우 */
		return -EOPNOTSUPP;

	return bd->uring_cmd_fn(q, ioucmd, issue_flags, open_for_write);	/* NVMe uring passthrough 처리; uring SQE -> NVMe SQE 변환 (추정) */
}

/*
 * NVMe 관점: /dev/bsg/*에 대한 파일 연산자.
 * SG_IO와 io_uring_cmd 진입점이 NVMe passthrough의 시작점이다.
 */
static const struct file_operations bsg_fops = {
	.open		=	bsg_open,
	.release	=	bsg_release,
	.unlocked_ioctl	=	bsg_ioctl,
	.compat_ioctl	=	compat_ptr_ioctl,			/* 32비트 userspace ioctl 호환; NVMe passthrough도 동일 경로 */
	.uring_cmd	=	bsg_uring_cmd,
	.owner		=	THIS_MODULE,
	.llseek		=	default_llseek,
};

/*
 * 목적: struct device 참조 카운트가 0이 되면 bsg 장치 메모리와 minor 번호를 해제.
 * 호출 경로: put_device -> bsg_device_release
 * NVMe 연결: NVMe 컨트롤러 제거 시 request_queue가 해제된 뒤에 bsg 측
 *            자원을 정리하여 dangling 참조를 방지한다.
 */
static void bsg_device_release(struct device *dev)
{
	struct bsg_device *bd = container_of(dev, struct bsg_device, device);

	ida_free(&bsg_minor_ida, MINOR(bd->device.devt));	/* /dev/bsg/* minor 번호 반납 */
	kfree(bd);							/* bsg_device 구조체 해제; NVMe passthrough 진입점 완전 소멸 */
}

/*
 * 목적: /dev/bsg/* 장치를 제거하고 sysfs/cdev 등록을 해제.
 * 호출 경로: nvme_stop_ctrl/nvme_remove -> bsg_unregister_queue
 * NVMe 연결: NVMe 컨트롤러나 네임스페이스가 사라질 때 bsg 진입점을
 *            먼저 닫아 사용자 공간의 추가 명령 제출을 차단한다.
 */
void bsg_unregister_queue(struct bsg_device *bd)
{
	struct gendisk *disk = bd->queue->disk;				/* NVMe 네임스페이스 gendisk; queue_kobj 경유 링크 제거용 */

	if (disk && disk->queue_kobj.sd)
		sysfs_remove_link(&disk->queue_kobj, "bsg");		/* queue sysfs에서 bsg 심볼릭 링크 제거 */
	cdev_device_del(&bd->cdev, &bd->device);			/* /dev/bsg/* 장치 제거; 이후 open 실패 */
	put_device(&bd->device);					/* bsg_device_release 트리거; NVMe 제거 시 dangling 참조 방지 */
}
EXPORT_SYMBOL_GPL(bsg_unregister_queue);

/*
 * 목적: request_queue에 대한 /dev/bsg/* 장치를 생성하고 콜백을 등록.
 * 호출 경로: nvme_probe/nvme_init_ns -> bsg_register_queue
 *              (drivers/nvme/host/core.c 등)
 * NVMe 연결: 이 함수가 성공해야 사용자 공간이 /dev/bsg/nvme*를 통해
 *            NVMe Admin/IO 명령을 전달할 수 있다.
 */
struct bsg_device *bsg_register_queue(struct request_queue *q,
		struct device *parent, const char *name, bsg_sg_io_fn *sg_io_fn,
		bsg_uring_cmd_fn *uring_cmd_fn)
{
	struct bsg_device *bd;
	int ret;

	bd = kzalloc_obj(*bd);							/* bsg_device 할당 */
	if (!bd)
		return ERR_PTR(-ENOMEM);
	bd->max_queue = BSG_DEFAULT_CMDS;					/* 기본 동시 명령 수 64 설정; NVMe SQ 깊이와 별개 */
	bd->reserved_size = INT_MAX;						/* 데이터 버퍼 상한 초기화; 이후 queue_max_bytes로 클리핑 */
	bd->queue = q;								/* NVMe request_queue 연결; blk-mq -> hardware queue -> SQ/CQ 매핑 시작 */
	bd->sg_io_fn = sg_io_fn;						/* ioctl SG_IO -> NVMe 콜백 연결; nvme_bsg_sg_io 진입 */
	bd->uring_cmd_fn = uring_cmd_fn;					/* io_uring -> NVMe 콜백 연결; nvme_bsg_uring_cmd 진입 (추정) */

	ret = ida_alloc_max(&bsg_minor_ida, BSG_MAX_DEVS - 1, GFP_KERNEL);
	if (ret < 0) {
		if (ret == -ENOSPC)
			dev_err(parent, "bsg: too many bsg devices\n");	/* minor 번호 고갈; 추가 NVMe 컨트롤러/네임스페이스 등록 불가 */
		kfree(bd);
		return ERR_PTR(ret);
	}
	bd->device.devt = MKDEV(bsg_major, ret);				/* /dev/bsg/* major/minor 할당; userspace open의 장치 식별 */
	bd->device.class = &bsg_class;
	bd->device.parent = parent;						/* NVMe 장치를 부모로; sysfs 트리 상속 */
	bd->device.release = bsg_device_release;
	dev_set_name(&bd->device, "%s", name);
	device_initialize(&bd->device);

	cdev_init(&bd->cdev, &bsg_fops);					/* bsg_fops(SG_IO, uring_cmd) 등록 */
	bd->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&bd->cdev, &bd->device);
	if (ret)
		goto out_put_device;

	if (q->disk && q->disk->queue_kobj.sd) {
		ret = sysfs_create_link(&q->disk->queue_kobj, &bd->device.kobj,
					"bsg");					/* queue sysfs에 bsg 링크 생성; NVMe ns/ctrl sysfs와 연결 */
		if (ret)
			goto out_device_del;
	}

	return bd;

out_device_del:
	cdev_device_del(&bd->cdev, &bd->device);
out_put_device:
	put_device(&bd->device);
	return ERR_PTR(ret);							/* 등록 실패; NVMe passthrough 진입점 생성 불가 */
}
EXPORT_SYMBOL_GPL(bsg_register_queue);

/*
 * 목적: /dev/bsg/<name> 형태의 devnode 경로를 생성.
 * 호출 경로: device_add -> bsg_devnode
 * NVMe 연결: NVMe 컨트롤러/네임스페이스 이름이 경로에 들어가
 *            /dev/bsg/nvme* 형태로 노출된다.
 */
static char *bsg_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "bsg/%s", dev_name(dev));		/* /dev/bsg/nvme0n1 등 NVMe 장치 노드 이름 생성 */
}

/*
 * bsg 클래스 정의. /sys/class/bsg/ 아래에 NVMe bsg 장치들이 생성된다.
 */
static const struct class bsg_class = {
	.name		= "bsg",
	.devnode	= bsg_devnode,
};

/*
 * 목적: bsg 클래스와 문자 장치 영역을 등록하여 /dev/bsg/* 인프라를 초기화.
 * 호출 경로: device_initcall -> bsg_init
 * NVMe 연결: 이 초기화가 완료된 후 NVMe 드라이버가 bsg_register_queue()를
 *            호출할 수 있다.
 */
static int __init bsg_init(void)
{
	dev_t devid;
	int ret;

	ret = class_register(&bsg_class);
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&devid, 0, BSG_MAX_DEVS, "bsg");
	if (ret)
		goto destroy_bsg_class;
	bsg_major = MAJOR(devid);						/* /dev/bsg major 번호 저장; 이후 MKDEV에 사용 */

	printk(KERN_INFO BSG_DESCRIPTION " version " BSG_VERSION
	       " loaded (major %d)\n", bsg_major);
	return 0;

destroy_bsg_class:
	class_unregister(&bsg_class);
	return ret;
}

MODULE_AUTHOR("Jens Axboe");
MODULE_DESCRIPTION(BSG_DESCRIPTION);
MODULE_LICENSE("GPL");

device_initcall(bsg_init);

/* NVMe 관점 핵심 요약
 *
 * - bsg는 /dev/bsg/*를 통해 사용자 공간의 원시 NVMe 명령(Admin/IO)이
 *   blk-mq request_queue로 진입하는 char-device 어댑터다.
 * - NVMe 드라이버가 등록한 sg_io_fn/uring_cmd_fn이 실제로 CID 할당,
 *   PRP/SGL 구성, doorbell 갱신으로 이어진다.
 * - max_queue는 bsg 계층의 동시 명령 개수 제한이며, 실제 NVMe SQ/CQ
 *   깊이는 blk-mq/드라이버가 별도로 관리한다 (추정).
 * - uring_cmd 경로는 큰 SQE/CQE(128/32바이트)를 필요로 하며,
 *   NVMe 명령 구조 전달에 적합하다 (추정).
 * - unregister_queue는 모듈 제거/reset 시 /dev/bsg/ 장치를 정리하고
 *   request_queue 참조를 해제한다.
 */
