// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Precision Time Measurement
 * Copyright (c) 2016, Intel Corporation.
 */

/*
 * [한국어 설명] 호스트와 장치의 시계를 나노초 단위로 맞추는 기능 (ptm.c)
 *
 * === 파일의 역할 ===
 * PTM(Precision Time Measurement)은 PCIe 계층의 장치들이 같은 시각을
 * 공유하게 해 주는 기능이다. 링크를 오가는 왕복 지연을 하드웨어가 직접
 * 측정하고 그것을 보정해, 호스트와 장치의 시계를 나노초 수준으로 맞춘다.
 *
 * 역할이 셋으로 나뉜다.
 *   Root      - 기준 시계를 갖는다. 보통 Root Complex 다.
 *   Responder - 요청을 받아 자기 시각을 알려 주는 중간 노드. 스위치다.
 *   Requester - 시각을 물어 자기 시계를 맞추는 엔드포인트.
 * 요청이 Requester 에서 Root 까지 올라가려면 그 경로의 모든 중간 노드가
 * Responder 여야 한다. 하나라도 PTM 을 모르면 그 경로는 성립하지 않는다.
 * pci_ptm_init() 이 그 연쇄를 따라가며 판정한다.
 *
 * granularity(정밀도)도 경로 전체가 함께 결정한다. 각 노드가 "나는
 * 몇 나노초까지 보장한다" 고 밝히고, 경로에서 가장 나쁜 값이 전체의
 * 정밀도가 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: probe.c 의 pci_init_capabilities()
 *         -> [이 파일] pci_ptm_init()
 *            -> capability 를 읽고, 상위로 거슬러 올라가며 경로가
 *               성립하는지와 정밀도를 계산해 dev->ptm_* 에 캐시한다
 *
 * 사용: PTM 이 필요한 드라이버가
 *         -> [이 파일] pci_enable_ptm(pdev, &granularity)
 *            -> 경로상의 모든 노드에서 PTM Enable 을 켠다
 *
 * 복원: 전원 복귀 후 pci_restore_state()
 *         -> [이 파일] pci_restore_ptm_state()
 *
 * 디버그: debugfs 에 각 노드의 PTM 상태를 노출한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거와 드라이버 초기화 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거), pci.c(전원 복원), 그리고 PTM 을 쓰는 드라이버들.
 * 아래쪽: access.c 의 config 접근, debugfs.
 * 공유 상태: struct pci_dev 의 ptm_cap(capability 오프셋),
 *   ptm_root / ptm_enabled / ptm_granularity, 그리고 상위 노드를 가리키는
 *   연결. 경로 전체가 함께 켜져야 하므로 이 연결이 필요하다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 PTM 을 전혀 쓰지 않는다. drivers/nvme/ 전체에서
 * "ptm" 이라는 문자열의 등장이 0건이다(주석 제거 후 검색).
 *
 * NVMe 스펙에도 시각 관련 기능이 있지만 다른 방식이다 —
 * Set Features 의 Timestamp(Feature Identifier 0x0E)로 호스트가
 * 밀리초 단위 시각을 컨트롤러에 알려 준다. 정밀도가 PTM 과 비교할 수
 * 없이 낮고, 목적도 다르다(로그의 시간 기록용이지 동기화용이 아니다).
 * drivers/nvme/ 에 NVME_FEAT_TIMESTAMP 가 쓰이는 것이 그것이다.
 *
 * PTM 이 실제로 쓰이는 것은 정밀한 시각이 필요한 장치들이다 —
 * 오디오 인터페이스, 산업용 이더넷(TSN), 계측 장비 같은 것들이다.
 *
 * (기존 주석은 "NVMe 컨트롤러가 PTM Requester 역할을 할 수 있으며,
 *  NVMe 타임스탬프와 telemetry 로그의 시간 정렬에 활용될 수 있다",
 *  "NVMe 드라이버가 PTM 기반 기능을 쓰려면 pci_enable_ptm 을 호출한다"
 *  고 적었으나, drivers/nvme/ 에 그런 호출도 PTM 이라는 언급도 없다.
 *  하드웨어가 지원할 가능성과 드라이버가 실제로 쓰는 것은 다른 문제이므로,
 *  검증된 사실로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_ptm_init()          : capability 를 찾고, 상위 경로가 PTM 을 지원하는지
 *                           확인해 이 장치가 Requester 가 될 수 있는지 판정한다.
 * pci_disable_ptm()       : PTM Enable 을 끈다.
 * pci_save_ptm_state() / pci_restore_ptm_state() : 전원 복귀 대비 저장/복원.
 * pci_enable_ptm()        : 경로상의 모든 노드에서 PTM 을 켜고, 최종
 *                           정밀도를 호출자에게 알려 준다.
 * pci_ptm_debugfs_init()  : debugfs 에 상태를 노출한다. 경로가 왜 성립하지
 *                           않는지 진단할 때 쓴다.
 * struct pci_ptm_debugfs  : debugfs 항목 하나의 상태.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include "../pci.h"

/*
 * pci_upstream_ptm:
 *   NVMe 장치에서 PTM Root 방향으로 한 단계 올라가면서 PTM을 지원하는
 *   상위 장치를 찾는다. Switch Downstream Port는 PTM capability를 갖지
 *   않으므로 그 위의 Upstream Port까지 추가로 찾는다.
 */
/*
 * If the next upstream device supports PTM, return it; otherwise return
 * NULL.  PTM Messages are local, so both link partners must support it.
 */
static struct pci_dev *pci_upstream_ptm(struct pci_dev *dev)
{
	struct pci_dev *ups = pci_upstream_bridge(dev);

	/*
	 * Switch Downstream Ports are not permitted to have a PTM
	 * capability; their PTM behavior is controlled by the Upstream
	 * Port (PCIe r5.0, sec 7.9.16), so if the upstream bridge is a
	 * Switch Downstream Port, look up one more level.
	 */
	if (ups && pci_pcie_type(ups) == PCI_EXP_TYPE_DOWNSTREAM)
		ups = pci_upstream_bridge(ups);

	if (ups && ups->ptm_cap)
		return ups;

	return NULL;
}

/*
 * pci_ptm_init:
 *   PCI 장치를 probe할 때 호출되며, NVMe 장치라면 PTM capability를 찾아
 *   dev->ptm_cap, ptm_granularity, ptm_root/requester/responder 플래그를
 *   설정한다. 이 정보는 이후 NVMe 드라이버가 pci_enable_ptm()을 결정할 때
 *   사용된다.
 */
/*
 * Find the PTM Capability (if present) and extract the information we need
 * to use it.
 */
void pci_ptm_init(struct pci_dev *dev)
{
	u16 ptm;
	u32 cap;
	struct pci_dev *ups;

	if (!pci_is_pcie(dev))
		return;

	ptm = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	if (!ptm)
		return;

	dev->ptm_cap = ptm;
	atomic_set(&dev->ptm_enable_cnt, 0);
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_PTM, sizeof(u32));

	pci_read_config_dword(dev, ptm + PCI_PTM_CAP, &cap);
	dev->ptm_granularity = FIELD_GET(PCI_PTM_GRANULARITY_MASK, cap);

	/*
	 * Per the spec recommendation (PCIe r6.0, sec 7.9.15.3), select the
	 * furthest upstream Time Source as the PTM Root.  For Endpoints,
	 * "the Effective Granularity is the maximum Local Clock Granularity
	 * reported by the PTM Root and all intervening PTM Time Sources."
	 */
	ups = pci_upstream_ptm(dev);
	if (ups) {
		if (ups->ptm_granularity == 0)
			dev->ptm_granularity = 0;
		else if (ups->ptm_granularity > dev->ptm_granularity)
			dev->ptm_granularity = ups->ptm_granularity;
	} else if (cap & PCI_PTM_CAP_ROOT) {
		dev->ptm_root = 1;
	} else if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {

		/*
		 * Per sec 7.9.15.3, this should be the Local Clock
		 * Granularity of the associated Time Source.  But it
		 * doesn't say how to find that Time Source.
		 */
		dev->ptm_granularity = 0;
	}

	if (cap & PCI_PTM_CAP_RES)
		dev->ptm_responder = 1;
	if (cap & PCI_PTM_CAP_REQ)
		dev->ptm_requester = 1;
}

/*
 * pci_save_ptm_state:
 *   시스템 suspend 전에 NVMe 장치의 PTM Control 레지스터 값을 저장한다.
 *   resume 후 pci_restore_ptm_state()로 복원할 때 사용된다.
 */
void pci_save_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!ptm)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM);
	if (!save_state)
		return;

	cap = (u32 *)&save_state->cap.data[0];
	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, cap);
}

/*
 * pci_restore_ptm_state:
 *   시스템 resume 시 저장했던 PTM Control 레지스터 값을 NVMe 장치에
 *   복원하여 PTM 동작을 suspend 이전 상태로 되돌린다.
 */
void pci_restore_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!ptm)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM);
	if (!save_state)
		return;

	cap = (u32 *)&save_state->cap.data[0];
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, *cap);
}

/*
 * __pci_enable_ptm:
 *   NVMe 장치의 PTM capability와 device type에 따라 PTM Control 레지스터에
 *   Enable/Root/Granularity 비트를 기록한다. 실제 enable 참조 카운트 관리는
 *   pci_enable_ptm()에서 담당한다.
 */
/* Enable PTM in the Control register if possible */
static int __pci_enable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	u32 ctrl;

	if (!ptm)
		return -EINVAL;

	switch (pci_pcie_type(dev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
		if (!dev->ptm_root)
			return -EINVAL;
		break;
	case PCI_EXP_TYPE_UPSTREAM:
		if (!dev->ptm_responder)
			return -EINVAL;
		break;
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_LEG_END:
		if (!dev->ptm_requester)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl);

	ctrl |= PCI_PTM_CTRL_ENABLE;
	ctrl &= ~PCI_PTM_GRANULARITY_MASK;
	ctrl |= FIELD_PREP(PCI_PTM_GRANULARITY_MASK, dev->ptm_granularity);
	if (dev->ptm_root)
		ctrl |= PCI_PTM_CTRL_ROOT;

	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl);
	return 0;
}

/*
 * pci_enable_ptm:
 *   NVMe 드라이버(또는 PCI 핵심 코드)가 NVMe 장치에서 PTM을 활성화할 때
 *   호출한다. 상위 장치부터 재귀적으로 enable을 전개하고, 이미 enable되어
 *   있으면 참조 카운트만 증가시킨다. 성공 시 dmesg에 granularity를 출력한다.
 */
/**
 * pci_enable_ptm() - Enable Precision Time Measurement
 * @dev: PCI device
 *
 * Enable Precision Time Measurement for @dev.
 *
 * Return: zero if successful, or -EINVAL if @dev lacks a PTM Capability or
 * is not a PTM Root and lacks an upstream path of PTM-enabled devices.
 */
int pci_enable_ptm(struct pci_dev *dev)
{
	int rc;
	char clock_desc[8];

	/*
	 * A device uses local PTM Messages to request time information
	 * from a PTM Root that's farther upstream. Every device along
	 * the path must support PTM and have it enabled so it can
	 * handle the messages. Therefore, if this device is not a PTM
	 * Root, the upstream link partner must have PTM enabled before
	 * we can enable PTM.
	 */
	if (!dev->ptm_root) {
		struct pci_dev *parent;

		parent = pci_upstream_ptm(dev);
		if (!parent)
			return -EINVAL;
		/* Enable PTM for the parent */
		rc = pci_enable_ptm(parent);
		if (rc)
			return rc;
	}

	/* Already enabled? */
	if (atomic_inc_return(&dev->ptm_enable_cnt) > 1)
		return 0;

	rc = __pci_enable_ptm(dev);
	if (rc) {
		atomic_dec(&dev->ptm_enable_cnt);
		return rc;
	}

	switch (dev->ptm_granularity) {
	case 0:
		snprintf(clock_desc, sizeof(clock_desc), "unknown");
		break;
	case 255:
		snprintf(clock_desc, sizeof(clock_desc), ">254ns");
		break;
	default:
		snprintf(clock_desc, sizeof(clock_desc), "%uns",
			 dev->ptm_granularity);
		break;
	}
	pci_info(dev, "PTM enabled%s, %s granularity\n",
		 dev->ptm_root ? " (root)" : "", clock_desc);

	return 0;
}
EXPORT_SYMBOL(pci_enable_ptm);

/*
 * __pci_disable_ptm:
 *   PTM Control 레지스터의 Enable/Root 비트를 클리어하여 NVMe 장치의 PTM을
 *   비활성화한다. 참조 카운트는 호출자가 관리한다.
 */
static void __pci_disable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	u32 ctrl;

	if (!ptm)
		return;

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl);
	ctrl &= ~(PCI_PTM_CTRL_ENABLE | PCI_PTM_CTRL_ROOT);
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl);
}

/*
 * pci_disable_ptm:
 *   NVMe 드라이버가 PTM 사용을 중단할 때 호출한다. 자신의 enable 카운트를
 *   감소시키고, 마지막 참조 해제 시 실제로 레지스터를 비활성화한다.
 *   이후 상위 장치에 대해서도 재귀적으로 disable을 전파한다.
 */
/**
 * pci_disable_ptm() - Disable Precision Time Measurement
 * @dev: PCI device
 *
 * Disable Precision Time Measurement for @dev.
 */
void pci_disable_ptm(struct pci_dev *dev)
{
	struct pci_dev *parent;

	if (atomic_dec_and_test(&dev->ptm_enable_cnt))
		__pci_disable_ptm(dev);

	parent = pci_upstream_ptm(dev);
	if (parent)
		pci_disable_ptm(parent);
}
EXPORT_SYMBOL(pci_disable_ptm);

/*
 * pci_suspend_ptm:
 *   시스템 suspend 경로에서 NVMe 장치의 PTM을 일시적으로 끈다. 이때
 *   enable 카운트는 보존되어 resume 시 재활성화 여부를 판단한다.
 */
/*
 * Disable PTM, but preserve dev->ptm_enable_cnt so we silently re-enable it on
 * resume if necessary.
 */
void pci_suspend_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt))
		__pci_disable_ptm(dev);
}

/*
 * pci_resume_ptm:
 *   시스템 resume 경로에서 NVMe 장치의 PTM을 다시 활성화한다.
 */
/* If PTM was enabled before suspend, re-enable it when resuming */
void pci_resume_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt))
		__pci_enable_ptm(dev);
}

/*
 * pcie_ptm_enabled:
 *   NVMe 장치(또는 상위 장치)에서 PTM이 현재 enable되어 있는지 확인한다.
 */
bool pcie_ptm_enabled(struct pci_dev *dev)
{
	if (!dev)
		return false;

	return atomic_read(&dev->ptm_enable_cnt);
}
EXPORT_SYMBOL(pcie_ptm_enabled);

#if IS_ENABLED(CONFIG_DEBUG_FS)

/*
 * context_update_write:
 *   debugfs를 통해 NVMe 장치(또는 Root Complex/Switch)의 PTM context update
 *   모드를 "auto" 또는 "manual"로 설정한다. NVMe 입장에서는 PTM 동작에 대한
 *   디버깅/테스트 인터페이스로 볼 수 있다.
 */
static ssize_t context_update_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data;
	char buf[7];
	int ret;
	u8 mode;

	if (!ptm_debugfs->ops->context_update_write)
		return -EOPNOTSUPP;

	if (count < 1 || count >= sizeof(buf))
		return -EINVAL;

	ret = copy_from_user(buf, ubuf, count);
	if (ret)
		return -EFAULT;

	buf[count] = '\0';

	if (sysfs_streq(buf, "auto"))
		mode = PCIE_PTM_CONTEXT_UPDATE_AUTO;
	else if (sysfs_streq(buf, "manual"))
		mode = PCIE_PTM_CONTEXT_UPDATE_MANUAL;
	else
		return -EINVAL;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_update_write(ptm_debugfs->pdata, mode);
	mutex_unlock(&ptm_debugfs->lock);
	if (ret)
		return ret;

	return count;
}

/*
 * context_update_read:
 *   debugfs에서 현재 PTM context update 모드를 읽어 "auto\n" 또는 "manual\n"으로
 *   사용자 공간에 반환한다.
 */
static ssize_t context_update_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data;
	char buf[8]; /* Extra space for NULL termination at the end */
	ssize_t pos;
	u8 mode;

	if (!ptm_debugfs->ops->context_update_read)
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ptm_debugfs->ops->context_update_read(ptm_debugfs->pdata, &mode);
	mutex_unlock(&ptm_debugfs->lock);

	if (mode == PCIE_PTM_CONTEXT_UPDATE_AUTO)
		pos = scnprintf(buf, sizeof(buf), "auto\n");
	else
		pos = scnprintf(buf, sizeof(buf), "manual\n");

	return simple_read_from_buffer(ubuf, count, ppos, buf, pos);
}

static const struct file_operations context_update_fops = {
	.open = simple_open,
	.read = context_update_read,
	.write = context_update_write,
};

/*
 * context_valid_get:
 *   debugfs에서 현재 PTM context가 유효한지 여부를 0/1로 읽는다.
 *   NVMe 입장에서는 PTM 동작 상태를 모니터링할 수 있는 디버깅 수단이다.
 */
static int context_valid_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	bool valid;
	int ret;

	if (!ptm_debugfs->ops->context_valid_read)
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_valid_read(ptm_debugfs->pdata, &valid);
	mutex_unlock(&ptm_debugfs->lock);
	if (ret)
		return ret;

	*val = valid;

	return 0;
}

/*
 * context_valid_set:
 *   debugfs를 통해 PTM context valid 상태를 0 또는 1로 설정한다.
 */
static int context_valid_set(void *data, u64 val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	int ret;

	if (!ptm_debugfs->ops->context_valid_write)
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_valid_write(ptm_debugfs->pdata, !!val);
	mutex_unlock(&ptm_debugfs->lock);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(context_valid_fops, context_valid_get,
			 context_valid_set, "%llu\n");

/*
 * local_clock_get:
 *   debugfs에서 PTM Local Clock 값을 읽어온다. NVMe 장치의 로컬 타임스탬프
 *   디버깅에 활용될 수 있다.
 */
static int local_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->local_clock_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->local_clock_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(local_clock_fops, local_clock_get, NULL, "%llu\n");

/*
 * master_clock_get:
 *   debugfs에서 PTM Master Clock 값을 읽어온다.
 */
static int master_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->master_clock_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->master_clock_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(master_clock_fops, master_clock_get, NULL, "%llu\n");

/*
 * t1_get:
 *   debugfs에서 PTM t1 timestamp를 읽어온다. t1은 PTM 메시지 교환 과정에서
 *   측정되는 타임스탬프 중 하나다.
 */
static int t1_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->t1_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t1_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t1_fops, t1_get, NULL, "%llu\n");

/*
 * t2_get:
 *   debugfs에서 PTM t2 timestamp를 읽어온다.
 */
static int t2_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->t2_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t2_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t2_fops, t2_get, NULL, "%llu\n");

/*
 * t3_get:
 *   debugfs에서 PTM t3 timestamp를 읽어온다.
 */
static int t3_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->t3_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t3_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t3_fops, t3_get, NULL, "%llu\n");

/*
 * t4_get:
 *   debugfs에서 PTM t4 timestamp를 읽어온다.
 */
static int t4_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	int ret;

	if (!ptm_debugfs->ops->t4_read)
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t4_read(ptm_debugfs->pdata, &clock);
	if (ret)
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t4_fops, t4_get, NULL, "%llu\n");

/*
 * pcie_ptm_create_debugfs_file:
 *   드라이버가 해당 attribute를 지원할 때만 debugfs 파일을 생성하는 매크로.
 *   NVMe/Root Complex 컨트롤러 드라이버는 이 매크로를 통해 필요한 PTM
 *   debugfs 항목만 노출한다.
 */
#define pcie_ptm_create_debugfs_file(pdata, mode, attr)			\
	do {									\
	\
		if (ops->attr##_visible && ops->attr##_visible(pdata))		\
		\
			debugfs_create_file(#attr, mode, ptm_debugfs->debugfs,	\
					    ptm_debugfs, &attr##_fops);	\
	} while (0)

/*
 * pcie_ptm_create_debugfs:
 *   PTM을 지원하는 컴포넌트(NVMe 컨트롤러, Root Complex 등)를 위해 debugfs
 *   디렉터리와 attribute 파일들을 생성한다. NVMe 컨트롤러 드라이버가
 *   pcie_ptm_ops를 등록하면 /sys/kernel/debug/pcie_ptm_... 아래에서 PTM
 *   context를 확인할 수 있다.
 */
/*
 * pcie_ptm_create_debugfs() - Create debugfs entries for the PTM context
 * @dev: PTM capable component device
 * @pdata: Private data of the PTM capable component device
 * @ops: PTM callback structure
 *
 * Create debugfs entries for exposing the PTM context of the PTM capable
 * components such as Root Complex and Endpoint controllers.
 *
 * Return: Pointer to 'struct pci_ptm_debugfs' if success, NULL otherwise.
 */
struct pci_ptm_debugfs *pcie_ptm_create_debugfs(struct device *dev, void *pdata,
			  const struct pcie_ptm_ops *ops)
{
	struct pci_ptm_debugfs *ptm_debugfs;
	char *dirname;
	int ret;

	/* Caller must provide check_capability() callback */
	if (!ops->check_capability)
		return NULL;

	/* Check for PTM capability before creating debugfs attributes */
	ret = ops->check_capability(pdata);
	if (!ret) {
		dev_dbg(dev, "PTM capability not present\n");
		return NULL;
	}

	ptm_debugfs = kzalloc_obj(*ptm_debugfs);
	if (!ptm_debugfs)
		return NULL;

	dirname = devm_kasprintf(dev, GFP_KERNEL, "pcie_ptm_%s", dev_name(dev));
	if (!dirname) {
		kfree(ptm_debugfs);
		return NULL;
	}

	ptm_debugfs->debugfs = debugfs_create_dir(dirname, NULL);
	ptm_debugfs->pdata = pdata;
	ptm_debugfs->ops = ops;
	mutex_init(&ptm_debugfs->lock);

	pcie_ptm_create_debugfs_file(pdata, 0644, context_update);
	pcie_ptm_create_debugfs_file(pdata, 0644, context_valid);
	pcie_ptm_create_debugfs_file(pdata, 0444, local_clock);
	pcie_ptm_create_debugfs_file(pdata, 0444, master_clock);
	pcie_ptm_create_debugfs_file(pdata, 0444, t1);
	pcie_ptm_create_debugfs_file(pdata, 0444, t2);
	pcie_ptm_create_debugfs_file(pdata, 0444, t3);
	pcie_ptm_create_debugfs_file(pdata, 0444, t4);

	return ptm_debugfs;
}
EXPORT_SYMBOL_GPL(pcie_ptm_create_debugfs);

/*
 * pcie_ptm_destroy_debugfs:
 *   pcie_ptm_create_debugfs()로 생성한 debugfs 디렉터리와 구조체를
 *   정리한다. NVMe 드라이버가 probe 해제 시 호출할 수 있다.
 */
/*
 * pcie_ptm_destroy_debugfs() - Destroy debugfs entries for the PTM context
 * @ptm_debugfs: Pointer to the PTM debugfs struct
 */
void pcie_ptm_destroy_debugfs(struct pci_ptm_debugfs *ptm_debugfs)
{
	if (!ptm_debugfs)
		return;

	mutex_destroy(&ptm_debugfs->lock);
	debugfs_remove_recursive(ptm_debugfs->debugfs);
	kfree(ptm_debugfs);
}
EXPORT_SYMBOL_GPL(pcie_ptm_destroy_debugfs);
#endif
