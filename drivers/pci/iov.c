// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express I/O Virtualization (IOV) support
 *   Single Root IOV 1.0
 *   Address Translation Service 1.0
 *
 * Copyright (C) 2009 Intel Corporation, Yu Zhao <yu.zhao@intel.com>
 */

/*
 * [한국어 설명] PCI Express SR-IOV(Single Root I/O Virtualization) 구현 (drivers/pci/iov.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 PCIe 장치 하나(PF, Physical Function)가 자기 자신을 여러 개의
 * 경량 함수(VF, Virtual Function)로 복제해 보이게 하는 SR-IOV 기능을 구현한다.
 * 구체적으로는 (1) 열거 단계에서 PCI_EXT_CAP_ID_SRIOV(0x10) 확장 capability 를
 * 찾아 struct pci_sriov 를 만들고(sriov_init), (2) 사용자가 sysfs 의
 * sriov_numvfs 에 값을 쓰면 SR-IOV Control 레지스터의 VF Enable 비트를 세워
 * 하드웨어에 VF 를 만들게 하고(sriov_enable), (3) 그렇게 생긴 VF 각각을
 * struct pci_dev 로 만들어 PCI 코어에 등록하며(pci_iov_add_virtfn),
 * (4) 반대로 되돌리는 경로(sriov_disable)와 전원/오류 복구 후 SR-IOV
 * 레지스터를 되살리는 경로(pci_restore_iov_state)를 제공한다.
 * VF 는 자기만의 config space 를 갖지만 대부분의 레지스터가 PF 에 하드와이어
 * 되어 있어, 링크 설정·전원 제어 같은 "장치 전체" 자원은 PF 만 만질 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 코어의 열거(probe.c) 단계 끝에서 pci_iov_init() 이 불려 이 파일이
 * 시작된다. 즉 위치는 "장치를 발견해 pci_dev 를 만든 직후, 드라이버가
 * 바인딩되기 전"이다. 그 뒤 실제 VF 생성은 드라이버가 붙은 다음에
 * 사용자 공간의 요청으로만 일어난다:
 *   사용자: echo N > /sys/bus/pci/devices/<PF>/sriov_numvfs
 *     -> sriov_numvfs_store()          (이 파일)
 *     -> pdev->driver->sriov_configure()  (드라이버가 등록한 함수 포인터)
 *     -> pci_sriov_configure_simple()  (이 파일, 특별한 준비가 필요 없는 드라이버용)
 *     -> sriov_enable()                (이 파일)
 *     -> pci_iov_add_virtfn() -> pci_setup_device()/pci_device_add() (probe.c)
 * 실행 컨텍스트는 전부 프로세스 컨텍스트(sysfs write 또는 드라이버 probe)이며,
 * msleep()/ssleep() 로 잠들 수 있으므로 인터럽트 문맥에서는 호출할 수 없다.
 * pci_enable_sriov()/pci_disable_sriov() 는 그 사실을 might_sleep() 으로 못박는다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/pci/probe.c: pci_device_add() 안에서 pci_iov_init() 을 불러 이 파일을
 *   시작시키고, 반대로 pci_release_dev() 가 pci_iov_release() 를 부른다.
 *   버스 번호를 얼마나 예약할지 정하는 pci_scan_child_bus_extend() 는
 *   이 파일의 pci_iov_bus_range() 를 부른다.
 * - drivers/pci/pci-driver.c: 드라이버가 언바인드될 때 pci_iov_remove() 를 부른다.
 * - drivers/pci/pci.c: pci_restore_state() 경로에서 pci_restore_iov_state() 를 불러
 *   D3->D0 복귀나 리셋 뒤 SR-IOV 레지스터를 되살린다.
 * - drivers/pci/setup-res.c: pci_update_resource() 가 VF BAR(resource 인덱스
 *   PCI_IOV_RESOURCES 이상)이면 이 파일의 pci_iov_update_resource() 로 넘긴다.
 * - drivers/pci/rebar.c: VF Resizable BAR 를 다룰 때 이 파일의
 *   pci_iov_resource_set_size() 와 pci_iov_is_memory_decoding_enabled() 를 부른다.
 * - drivers/pci/pci-sysfs.c: sriov_pf_dev_attr_group 과 sriov_vf_dev_attr_group 을
 *   장치 attribute group 목록에 넣어 이 파일이 만든 sysfs 파일을 노출한다.
 * - drivers/pci/ide.c, drivers/pci/tsm.c: VF 의 Requester ID 범위를 알아내려고
 *   pci_iov_virtfn_bus()/pci_iov_virtfn_devfn() 을 부른다.
 * - drivers/vfio/pci 아래의 VF 드라이버들: VF 를 게스트에 넘기는 쪽. pci_iov_vf_id() 로 몇 번째
 *   VF 인지 알아내고 pci_iov_get_pf_drvdata() 로 PF 드라이버의 사설 데이터를 얻는다.
 * 공유하는 핵심 자료구조는 struct pci_sriov(drivers/pci/pci.h)다. PF 의
 * pci_dev->sriov 가 이를 가리키며, capability 오프셋(pos), VF 개수(total_VFs,
 * num_VFs), RID 계산에 쓰는 offset/stride, VF BAR 크기 배열(barsz[]) 이
 * 여기에 모여 있다. VF 쪽 pci_dev 는 is_virtfn=1 과 physfn 포인터로 PF 를 가리킨다.
 *
 * === 주요 함수/구조체 요약 ===
 * - sriov_init(): 열거 시각에 SR-IOV capability 를 읽어 struct pci_sriov 를 만들고,
 *   VF BAR 6개의 크기를 재서 "VF BAR 크기 x TotalVFs" 만큼의 MMIO 를 PF 자원으로 예약한다.
 * - sriov_enable()/sriov_disable(): NumVFs 를 쓰고 VF Enable/MSE 비트를 세우거나
 *   지워 하드웨어의 VF 를 켜고 끈다. 켠 뒤 VF 마다 pci_dev 를 만든다.
 * - pci_iov_virtfn_bus()/pci_iov_virtfn_devfn(): VF 의 RID(버스/devfn)를
 *   PF 의 RID + First VF Offset + VF Stride * vf_id 로 계산한다.
 * - pci_iov_add_virtfn()/pci_iov_remove_virtfn(): 한 개의 VF 에 대응하는
 *   struct pci_dev 를 만들어 등록하거나 되돌린다. VF BAR 는 PF 가 예약한
 *   큰 영역을 stride 간격으로 잘라 쓴다.
 * - pci_sriov_configure_simple(): PF 쪽에 아무 준비가 필요 없는 드라이버가
 *   .sriov_configure 로 그대로 등록해 쓰는 기본 구현.
 *   drivers/nvme/host/pci.c 의 nvme_driver 가 바로 이 함수를 등록한다.
 * - struct pci_sriov(drivers/pci/pci.h 정의): PF 하나의 SR-IOV 상태 전부.
 *   pos(capability 오프셋), cap/ctrl(레지스터 사본), total_VFs/initial_VFs/num_VFs,
 *   offset/stride(RID 계산), vf_device(VF Device ID), pgsz(System Page Size),
 *   barsz[6](VF BAR 하나의 크기), max_VF_buses(VF 가 쓰는 최대 버스 번호).
 *
 * === SR-IOV capability 레지스터 지도 (pos 로부터의 오프셋) ===
 * 이 파일이 실제로 읽고 쓰는 레지스터만 적는다. 이름은 pci_regs.h 의 정의다.
 *   +0x04 PCI_SRIOV_CAP        VF Migration 지원 여부 등 (bit0 = PCI_SRIOV_CAP_VFM)
 *   +0x08 PCI_SRIOV_CTRL       bit0 VFE(VF Enable), bit3 MSE(VF Memory Space Enable),
 *                              bit4 ARI(ARI Capable Hierarchy)
 *   +0x0c PCI_SRIOV_INITIAL_VF InitialVFs — 부팅 시각에 쓸 수 있는 VF 수
 *   +0x0e PCI_SRIOV_TOTAL_VF   TotalVFs — 하드웨어가 지원하는 최대 VF 수
 *   +0x10 PCI_SRIOV_NUM_VF     NumVFs — 소프트웨어가 "몇 개를 켤지" 쓰는 곳
 *   +0x12 PCI_SRIOV_FUNC_LINK  Function Dependency Link — 함께 켜야 하는 PF 번호
 *   +0x14 PCI_SRIOV_VF_OFFSET  First VF Offset — VF0 의 RID 가 PF RID 에서 떨어진 거리
 *   +0x16 PCI_SRIOV_VF_STRIDE  VF Stride — 이웃한 VF 사이의 RID 간격
 *   +0x1a PCI_SRIOV_VF_DID     VF Device ID — VF 의 Device ID(Vendor ID 는 PF 와 같다)
 *   +0x1c PCI_SRIOV_SUP_PGSIZE Supported Page Sizes — 비트 n 이 4KB<<n 지원을 뜻한다
 *   +0x20 PCI_SRIOV_SYS_PGSIZE System Page Size — 실제로 쓸 페이지 크기를 하나 고른다
 *   +0x24 PCI_SRIOV_BAR        VF BAR0 ~ VF BAR5 (dword 6개, PCI_SRIOV_NUM_BARS=6)
 *
 * === VF 의 RID 와 VF BAR 가 보통 장치와 다른 점 ===
 * (1) RID: VF 는 config space 에 자기 BAR 를 갖되 Bus/Device/Function 번호는
 *     하드웨어가 정해 준다. 그 값이 First VF Offset 과 VF Stride 이고,
 *     VF k 의 RID = PF RID + offset + stride * k 다. 이 합이 256(=한 버스의
 *     devfn 공간)을 넘으면 VF 는 PF 와 다른 버스 번호로 넘어간다. 그래서
 *     compute_max_vf_buses() 로 필요한 버스 수를 미리 재어 두고,
 *     pci_iov_bus_range() 가 열거 때 그만큼의 버스 번호를 남겨 둔다.
 * (2) VF BAR: 보통 장치의 BAR 는 각 함수의 config space 0x10~0x24 에 있고
 *     함수마다 크기가 다를 수 있다. 반면 VF 의 BAR 는 VF 자신의 config space 가
 *     아니라 PF 의 SR-IOV capability 안 VF BAR 레지스터(pos+0x24~)로 크기가
 *     정해지며, 모든 VF 가 같은 크기를 쓴다. 커널은 이 크기(iov->barsz[i])에
 *     TotalVFs 를 곱한 큰 MMIO 영역 하나를 PF 의 resource[PCI_IOV_RESOURCES+i] 로
 *     잡아 두고, VF k 의 BAR 는 그 영역 안의 [start + barsz*k, +barsz) 구간을
 *     request_resource() 로 잘라 쓴다. 따라서 VF 의 BAR 는 "따로 할당되는 것"이
 *     아니라 "PF 가 잡아 둔 연속 영역을 균등 분할한 창"이다.
 *
 * === NVMe 와의 접점 (근거를 확인한 것만) ===
 * drivers/nvme/host/pci.c 의 struct pci_driver nvme_driver 는
 *   .sriov_configure = pci_sriov_configure_simple
 * 로 이 파일의 함수를 그대로 등록한다(grep 으로 확인). 즉 NVMe 호스트
 * 드라이버는 SR-IOV 설정에 대해 VF 별 특별 처리를 전혀 하지 않고 커널 공통
 * 구현에 위임한다. 그 결과 NVMe PF 의 sriov_numvfs 에 값을 쓰면
 * pci_sriov_configure_simple() -> sriov_enable() 이 그대로 돌고,
 * 만들어진 VF 는 다시 보통의 PCI 장치로 열거되어 (id_table 이 맞으면)
 * 같은 nvme 드라이버가 probe 한다. NVMe 스펙이 말하는 Secondary Controller
 * 자원 배분(VF 당 큐/인터럽트 수 조정)은 이 파일이 아니라 관리 커맨드
 * 영역이며, 이 트리의 nvme 호스트 드라이버에는 그 경로가 없다.
 * 또한 nvme_driver 는 .sriov_get_vf_total_msix 나 .sriov_set_msix_vec_count 를
 * 등록하지 않으므로, 이 파일의 sriov_vf_total_msix / sriov_vf_msix_count
 * sysfs 파일은 NVMe 장치에서는 -EOPNOTSUPP 로 동작한다(해당 함수 참조).
 */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP 매크로 — VF Resizable BAR 제어 레지스터의 비트필드(BAR index, BAR size)를 뽑고 넣는 데 쓴다 */
#include <linux/bits.h>	/* [한국어] BIT 계열 비트 조작 정의 — 레지스터 비트 마스크 계산에 필요 */
#include <linux/log2.h>	/* [한국어] ilog2/roundup_pow_of_two — VF BAR 크기를 2의 거듭제곱 인코딩으로 바꿀 때 쓴다(pci_iov_vf_bar_get_sizes) */
#include <linux/pci.h>	/* [한국어] struct pci_dev, pci_read_config_word 계열 등 PCI 코어 API 전부의 선언 */
#include <linux/sizes.h>	/* [한국어] SZ_1M 같은 크기 상수 — ReBAR 인코딩의 기준 단위(1MB)를 표현하는 데 쓴다 */
#include <linux/slab.h>	/* [한국어] kzalloc_obj/kfree — PF 하나당 struct pci_sriov 를 힙에 잡는다 */
#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL_GPL 매크로 — 이 파일의 SR-IOV API 를 모듈에 공개하기 위해 필요 */
#include <linux/string.h>	/* [한국어] sprintf 등 문자열 함수 — virtfn%u sysfs 링크 이름을 만든다 */
#include <linux/delay.h>	/* [한국어] msleep/ssleep — VF Enable 을 쓰고 나서 스펙이 요구하는 대기 시간을 지킨다 */
#include <asm/div64.h>	/* [한국어] do_div — 64비트 나눗셈(VF BAR 길이를 VF 수로 나눔)을 32비트 아키텍처에서도 쓰기 위해 */
#include "pci.h"	/* [한국어] drivers/pci 내부 전용 헤더 — struct pci_sriov 정의와 pci_iov_* 내부 선언이 여기 있다 */

#define VIRTFN_ID_LEN	17	/* "virtfn%u\0" for 2^32 - 1 */	/* [한국어] sysfs 링크 이름 "virtfn%u" 를 담을 버퍼 크기. VF ID 가 32비트 최대값(4294967295, 10자리)이어도 "virtfn"(6) + 10 + NUL(1) = 17 바이트면 충분하다 */

/*
 * [한국어]
 * pci_iov_virtfn_bus - VF 하나가 앉게 될 PCI 버스 번호를 계산한다
 *
 * @dev: PF 의 pci_dev. is_physfn 이 1 이어야 하며 dev->sriov 가 이미 채워져 있어야 한다.
 * @vf_id: 몇 번째 VF 인지(0 부터 시작). NumVFs-1 까지가 유효하다.
 * @return: 성공하면 0~255 범위의 버스 번호, PF 가 아니면 -EINVAL.
 *          호출자는 이 값을 pci_find_bus()/pci_add_new_bus() 의 인자나
 *          busn_res 범위 검사(sriov_enable)에 그대로 쓴다.
 *
 * 왜 필요한가: VF 는 config space 상에 존재하지만 자기 RID(Requester ID,
 * 곧 Bus/Device/Function 번호)를 스스로 정하지 않는다. SR-IOV 스펙은
 * VF 의 RID 를 "PF 의 RID + First VF Offset + VF Stride * vf_id" 로 정의하고,
 * 그 두 값을 capability 의 PCI_SRIOV_VF_OFFSET(+0x14) 과
 * PCI_SRIOV_VF_STRIDE(+0x16) 에 노출한다. 커널은 VF 를 실제로 열거해서
 * 찾는 것이 아니라 이 식으로 주소를 유도한다.
 * 계산 과정: (devfn + offset + stride*vf_id) 는 "PF 의 devfn 에서 몇 칸
 * 떨어졌는가"를 devfn 단위로 센 값이다. 한 버스에는 devfn 이 256개(5비트
 * device x 3비트 function)뿐이므로, 이 합을 8비트 오른쪽 시프트하면
 * "버스를 몇 개 넘어갔는가"가 나온다. 그것을 PF 의 버스 번호에 더한다.
 * 즉 VF 는 PF 와 같은 버스에 있을 수도 있고, VF 수가 많으면 PF 보다
 * 뒤쪽 버스로 넘어갈 수도 있다 — 그래서 열거 단계에서 미리 버스 번호를
 * 더 확보해 두어야 한다(compute_max_vf_buses, pci_iov_bus_range).
 * 실행 컨텍스트: 순수 계산 + 이미 읽어 둔 iov 필드 참조뿐이라 락이 없다.
 * offset/stride 는 pci_iov_set_numvfs() 가 NumVFs 를 쓸 때마다 갱신하므로,
 * 이 함수는 "현재 설정된 NumVFs 기준"의 값을 돌려준다는 점에 주의한다.
 *
 * 호출 체인:
 *   sriov_enable(), pci_iov_add_virtfn(), pci_iov_remove_virtfn(),
 *   compute_max_vf_buses(), drivers/pci/ide.c, drivers/pci/tsm.c
 *     -> [pci_iov_virtfn_bus]
 */
int pci_iov_virtfn_bus(struct pci_dev *dev, int vf_id)	/* [한국어] PF 의 pci_dev 와 VF 번호를 받아 버스 번호를 돌려준다. include/linux/pci.h 에 선언되어 다른 파일에서도 쓴다 */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 dev->sriov 가 NULL 이라 아래 식이 널 역참조가 된다. 먼저 걸러 낸다 */
		return -EINVAL;	/* [한국어] PF 가 아닌 장치에 물은 것은 호출자의 논리 오류이므로 -EINVAL */
	return dev->bus->number + ((dev->devfn + dev->sriov->offset +	/* [한국어] devfn + First VF Offset + VF Stride * vf_id 로 "PF 로부터의 devfn 거리"를 구하고 */
				    dev->sriov->stride * vf_id) >> 8);	/* [한국어] 8비트 시프트로 그 거리를 버스 수로 환산해 PF 의 버스 번호에 더한다(한 버스 = devfn 256개) */
}

/*
 * [한국어]
 * pci_iov_virtfn_devfn - VF 하나의 devfn(Device 번호 5비트 + Function 번호 3비트)을 계산한다
 *
 * @dev: PF 의 pci_dev. is_physfn 이 1 이어야 한다.
 * @vf_id: 몇 번째 VF 인지(0 부터).
 * @return: 0~255 의 devfn 값, PF 가 아니면 -EINVAL.
 *          호출자는 pci_get_domain_bus_and_slot() 이나 새 VF pci_dev 의
 *          virtfn->devfn 필드에 이 값을 넣는다.
 *
 * 왜 필요한가: pci_iov_virtfn_bus() 가 같은 합의 상위 비트(버스)를 뽑았다면
 * 이 함수는 하위 8비트(버스 안에서의 위치)를 뽑는다. 두 함수를 합치면
 * VF 의 완전한 RID 가 된다. 0xff 마스크가 곧 "한 버스 안 devfn 공간"이다.
 * ARI(Alternative Routing-ID Interpretation)가 켜져 있으면 이 8비트를
 * device 5비트 + function 3비트로 쪼개지 않고 통째로 8비트 function 번호로
 * 해석한다. 그래서 한 버스에 8개가 아니라 256개의 함수를 놓을 수 있고,
 * VF 를 많이 만들 때 필요한 버스 수가 줄어든다.
 * 실행 컨텍스트: 계산뿐. 락 없음. 재진입 안전.
 *
 * 호출 체인:
 *   pci_iov_scan_device(), pci_iov_remove_virtfn(), drivers/pci/ide.c,
 *   drivers/pci/tsm.c -> [pci_iov_virtfn_devfn]
 */
int pci_iov_virtfn_devfn(struct pci_dev *dev, int vf_id)	/* [한국어] VF 의 devfn 계산. EXPORT_SYMBOL_GPL 로 모듈에도 공개된다 */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 dev->sriov 가 없으므로 계산 자체가 불가능하다 */
		return -EINVAL;	/* [한국어] 잘못된 인자임을 알린다 */
	return (dev->devfn + dev->sriov->offset +	/* [한국어] pci_iov_virtfn_bus 와 완전히 같은 합을 만들고 */
		dev->sriov->stride * vf_id) & 0xff;	/* [한국어] 0xff 로 하위 8비트만 남긴다 — 버스 안에서의 devfn. ARI 가 켜져 있으면 이 8비트 전체가 function 번호다 */
}
EXPORT_SYMBOL_GPL(pci_iov_virtfn_devfn);	/* [한국어] VF 를 다루는 외부 모듈(예: vfio VF 드라이버)이 쓸 수 있게 GPL 심볼로 공개 */

/*
 * [한국어]
 * pci_iov_vf_id - VF 의 pci_dev 로부터 "몇 번째 VF 인가"를 역산한다
 *
 * @dev: VF 의 pci_dev. is_virtfn 이 1 이어야 한다.
 * @return: 0 부터 시작하는 VF 번호, VF 가 아니면 -EINVAL.
 *          호출자(vfio 계열 VF 드라이버)는 이 번호로 PF 에게
 *          "몇 번 VF 의 상태를 달라"고 요청한다.
 *
 * 왜 필요한가: pci_iov_virtfn_bus/devfn 이 vf_id -> RID 방향이라면,
 * 이 함수는 RID -> vf_id 의 역방향이다. VF 드라이버는 자기 pci_dev 만
 * 갖고 있으므로, PF 와의 RID 차이를 stride 로 나누어 자기 번호를 구한다.
 * pci_dev_id() 는 (bus << 8) | devfn 으로 16비트 RID 를 만드는 헬퍼라
 * 버스 경계를 넘는 경우도 뺄셈 한 번으로 처리된다.
 * 실행 컨텍스트: VF 드라이버의 probe 문맥(프로세스 컨텍스트). PF 의
 * sriov 구조체를 읽지만 offset/stride 는 VF 가 존재하는 동안 바뀌지
 * 않으므로(NumVFs 를 바꾸려면 먼저 모든 VF 를 지워야 한다) 락이 없다.
 *
 * 호출 체인:
 *   drivers/vfio/pci 아래 VF 드라이버들(mlx5, hisi_acc, qat, pds, xe)
 *     -> [pci_iov_vf_id] -> pci_physfn(), pci_dev_id()
 */
int pci_iov_vf_id(struct pci_dev *dev)	/* [한국어] VF 번호 역산 함수. 외부 모듈용으로 공개된다 */
{
	struct pci_dev *pf;	/* [한국어] 이 VF 를 낳은 PF 의 pci_dev 를 담을 지역 변수 */

	if (!dev->is_virtfn)	/* [한국어] VF 가 아니면 physfn 포인터가 없어 기준점을 잡을 수 없다 */
		return -EINVAL;	/* [한국어] VF 가 아닌 장치에 물은 것이므로 -EINVAL */

	pf = pci_physfn(dev);	/* [한국어] pci_physfn 은 is_virtfn 일 때 dev->physfn 을, 아니면 dev 자신을 돌려주는 인라인 헬퍼 */
	return (pci_dev_id(dev) - (pci_dev_id(pf) + pf->sriov->offset)) /	/* [한국어] VF 의 16비트 RID 에서 "VF0 의 RID"(= PF RID + First VF Offset)를 빼면 VF0 로부터의 거리가 나오고 */
	       pf->sriov->stride;	/* [한국어] 그 거리를 VF Stride 로 나누면 몇 번째 VF 인지가 된다 */
}
EXPORT_SYMBOL_GPL(pci_iov_vf_id);	/* [한국어] vfio VF 드라이버들이 모듈로 빌드되므로 심볼을 공개해야 한다 */

/**
 * pci_iov_get_pf_drvdata - Return the drvdata of a PF
 * @dev: VF pci_dev
 * @pf_driver: Device driver required to own the PF
 *
 * This must be called from a context that ensures that a VF driver is attached.
 * The value returned is invalid once the VF driver completes its remove()
 * callback.
 *
 * Locking is achieved by the driver core. A VF driver cannot be probed until
 * pci_enable_sriov() is called and pci_disable_sriov() does not return until
 * all VF drivers have completed their remove().
 *
 * The PF driver must call pci_disable_sriov() before it begins to destroy the
 * drvdata.
 */
/*
 * [한국어]
 * pci_iov_get_pf_drvdata - VF 드라이버가 PF 드라이버의 사설 데이터를 안전하게 얻는다
 *
 * @dev: VF 의 pci_dev.
 * @pf_driver: PF 를 소유하고 있어야 하는 드라이버(호출자가 자기 PF 드라이버를 넘긴다).
 * @return: 성공 시 PF 의 drvdata 포인터, 실패 시 ERR_PTR(-EINVAL).
 *          호출자는 IS_ERR() 로 검사한 뒤 PF 쪽 구조체로 캐스팅해 쓴다.
 *
 * 왜 필요한가: VF 를 다루는 드라이버(주로 vfio 의 마이그레이션 지원 코드)는
 * VF 만으로는 할 수 없는 일 — 예를 들어 "이 VF 의 상태를 덤프하라"는 요청 —
 * 을 PF 드라이버에게 부탁해야 한다. 그러려면 PF 드라이버의 사설 구조체가
 * 필요한데, 아무 PF 나 믿을 수는 없으므로 "PF 에 붙어 있는 드라이버가 정말
 * 내가 기대하는 그 드라이버인가"를 먼저 확인한다.
 * 위 영어 주석이 설명하듯 락은 드라이버 코어가 대신 잡아 준다: VF 드라이버는
 * pci_enable_sriov() 이후에야 probe 될 수 있고, pci_disable_sriov() 는 모든
 * VF 드라이버의 remove() 가 끝날 때까지 돌아오지 않는다. 따라서 VF 드라이버가
 * 살아 있는 동안에는 PF 의 drvdata 가 사라지지 않는다.
 * 에러 경로: VF 가 아니거나 PF 에 다른 드라이버가 붙어 있으면 포인터 대신
 * ERR_PTR 을 돌려주어 호출자가 기능을 포기하게 만든다.
 *
 * 호출 체인:
 *   VF 드라이버(예: drivers/vfio/pci/hisilicon/hisi_acc_vfio_pci.c)
 *     -> [pci_iov_get_pf_drvdata] -> pci_get_drvdata()
 */
void *pci_iov_get_pf_drvdata(struct pci_dev *dev, struct pci_driver *pf_driver)	/* [한국어] VF 의 pci_dev 와 기대하는 PF 드라이버를 받아 PF 의 drvdata 를 돌려준다 */
{
	struct pci_dev *pf_dev;	/* [한국어] PF 의 pci_dev 를 담을 지역 변수 */

	if (!dev->is_virtfn)	/* [한국어] VF 가 아니면 physfn 이 없어 PF 를 특정할 수 없다 */
		return ERR_PTR(-EINVAL);	/* [한국어] 포인터를 반환하는 함수이므로 오류도 ERR_PTR 로 포장해 돌려준다 */
	pf_dev = dev->physfn;	/* [한국어] VF pci_dev 는 생성 시점에 physfn 이 PF 를 가리키도록 채워진다(pci_iov_scan_device 참조) */
	if (pf_dev->driver != pf_driver)	/* [한국어] PF 에 붙은 드라이버가 호출자가 기대한 드라이버가 아니면, 그 drvdata 의 형식을 알 수 없다 */
		return ERR_PTR(-EINVAL);	/* [한국어] 엉뚱한 구조체를 넘겨 주는 대신 오류로 처리한다 */
	return pci_get_drvdata(pf_dev);	/* [한국어] PF 드라이버가 pci_set_drvdata() 로 넣어 둔 포인터를 그대로 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_iov_get_pf_drvdata);	/* [한국어] vfio VF 드라이버들이 모듈이므로 공개 필요 */

/*
 * Per SR-IOV spec sec 3.3.10 and 3.3.11, First VF Offset and VF Stride may
 * change when NumVFs changes.
 *
 * Update iov->offset and iov->stride when NumVFs is written.
 */
/*
 * [한국어]
 * pci_iov_set_numvfs - NumVFs 레지스터에 값을 쓰고, 그에 따라 바뀐 offset/stride 를 다시 읽는다
 *
 * @dev: PF 의 pci_dev.
 * @nr_virtfn: NumVFs 에 쓸 값. 0 이면 "VF 를 하나도 두지 않음".
 * @return: 없음. config write 실패는 PCI 코어가 삼키므로 여기서 오류를 보지 않는다.
 *
 * 왜 필요한가: 바로 위 영어 주석이 근거다 — SR-IOV 스펙 3.3.10/3.3.11 에 따르면
 * First VF Offset 과 VF Stride 는 NumVFs 가 바뀌면 하드웨어가 값을 바꿀 수 있다.
 * 따라서 "NumVFs 를 쓴다"와 "offset/stride 사본을 갱신한다"는 절대 떨어져서는
 * 안 되는 한 쌍이고, 그 쌍을 이 헬퍼 하나로 묶어 두었다. 이 갱신을 빠뜨리면
 * pci_iov_virtfn_bus/devfn 이 옛 값으로 엉뚱한 RID 를 계산하게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출 경로가 sysfs store(device_lock 보유)
 * 이거나 열거 초기화라 별도 락은 잡지 않는다. config space 접근은 PCI 코어의
 * 버스별 락으로 직렬화된다.
 *
 * 호출 체인:
 *   compute_max_vf_buses(), sriov_enable(), sriov_disable(), sriov_restore_state()
 *     -> [pci_iov_set_numvfs] -> pci_write_config_word()/pci_read_config_word()
 */
static inline void pci_iov_set_numvfs(struct pci_dev *dev, int nr_virtfn)	/* [한국어] static inline — 파일 안에서만 쓰는 짧은 헬퍼라 호출 비용을 없앤다 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 하나의 SR-IOV 상태 전부를 담은 구조체(drivers/pci/pci.h 정의) */

	pci_write_config_word(dev, iov->pos + PCI_SRIOV_NUM_VF, nr_virtfn);	/* [한국어] capability 시작 오프셋 + 0x10(PCI_SRIOV_NUM_VF) 에 "켤 VF 개수"를 쓴다. 이 값이 VF Enable 과 함께 실제 VF 수를 정한다 */
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_OFFSET, &iov->offset);	/* [한국어] +0x14(First VF Offset) 를 다시 읽어 사본을 갱신 — NumVFs 를 바꾸면 하드웨어가 이 값을 바꿀 수 있다 */
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_STRIDE, &iov->stride);	/* [한국어] +0x16(VF Stride) 도 같은 이유로 다시 읽는다 */
}

/*
 * The PF consumes one bus number.  NumVFs, First VF Offset, and VF Stride
 * determine how many additional bus numbers will be consumed by VFs.
 *
 * Iterate over all valid NumVFs, validate offset and stride, and calculate
 * the maximum number of bus numbers that could ever be required.
 */
/*
 * [한국어]
 * compute_max_vf_buses - VF 들이 쓸 수 있는 최대 버스 번호를 미리 조사한다
 *
 * @dev: PF 의 pci_dev. 이 시점에 dev->sriov 는 이미 채워져 있다.
 * @return: 0 이면 성공, -EIO 면 하드웨어가 말이 안 되는 offset/stride 를 보고했다.
 *          호출자 sriov_init() 은 -EIO 를 받으면 SR-IOV 초기화 전체를 되돌린다.
 *
 * 왜 필요한가: 버스 번호는 PCI 열거 단계에서 한 번 배분되면 나중에 늘릴 수 없다.
 * 그런데 VF 는 사용자가 한참 뒤에 sysfs 로 켜는 것이고, 켜는 개수에 따라
 * 필요한 버스 수가 달라진다. 그래서 열거 시각에 "가능한 모든 NumVFs 값"을
 * 시험해 보고 최악의 경우 필요한 버스 번호를 구해 iov->max_VF_buses 에 남긴다.
 * 나중에 pci_iov_bus_range() 가 이 값을 보고 다리(bridge) 뒤에 버스 번호를
 * 넉넉히 예약한다. 이 조사를 안 하면 "VF 를 켜려는데 버스 번호가 없다"는
 * 상황이 생기고, 그때는 이미 되돌릴 수 없다.
 * 동작 과정: TotalVFs 부터 1 까지 거꾸로 내려가며 NumVFs 를 실제로 써 보고,
 * 하드웨어가 돌려준 offset/stride 로 마지막 VF(nr_virtfn-1)의 버스 번호를
 * 계산해 최대값을 갱신한다. 내림차순인 이유는 큰 NumVFs 일수록 더 먼 버스를
 * 쓰기 때문에 루프 초반에 최대값이 확정되기 때문이다.
 * 유효성 검사: offset 이 0 이면 VF0 이 PF 자신과 같은 RID 라는 뜻이라 불가능하고,
 * VF 가 2개 이상인데 stride 가 0 이면 모든 VF 가 겹친다는 뜻이라 역시 불가능하다.
 * 실행 컨텍스트: 열거 중(pci_device_add 경로), 프로세스 컨텍스트.
 * 이 시점에는 아직 VF 가 없고 드라이버도 붙지 않아 경쟁자가 없다.
 * 에러 경로: 어느 NumVFs 에서든 검사에 걸리면 out 으로 빠져 NumVFs 를 0 으로
 * 되돌린 뒤 -EIO 를 반환한다. 레지스터를 조사만 하고 원상복구하는 것이 핵심이다.
 *
 * 호출 체인:
 *   pci_iov_init() -> sriov_init() -> [compute_max_vf_buses]
 *     -> pci_iov_set_numvfs(), pci_iov_virtfn_bus()
 */
static int compute_max_vf_buses(struct pci_dev *dev)	/* [한국어] 파일 내부 전용(static). 열거 시각에 단 한 번 불린다 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태 구조체를 지역 변수로 잡아 접근을 짧게 한다 */
	int nr_virtfn, busnr, rc = 0;	/* [한국어] nr_virtfn: 시험 중인 VF 개수, busnr: 그때의 마지막 VF 버스 번호, rc: 반환값(기본 성공) */

	for (nr_virtfn = iov->total_VFs; nr_virtfn; nr_virtfn--) {	/* [한국어] TotalVFs 에서 1 까지 내려가며 모든 NumVFs 값을 시험한다. nr_virtfn 이 0 이 되면 루프 종료 */
		pci_iov_set_numvfs(dev, nr_virtfn);	/* [한국어] NumVFs 를 실제로 써서 하드웨어가 보고하는 offset/stride 를 이 개수 기준으로 갱신시킨다 */
		if (!iov->offset || (nr_virtfn > 1 && !iov->stride)) {	/* [한국어] offset 0 은 VF0 이 PF 와 같은 RID 라는 뜻이고, VF 2개 이상에서 stride 0 은 VF 들이 서로 겹친다는 뜻이라 둘 다 스펙 위반이다 */
			rc = -EIO;	/* [한국어] 하드웨어가 앞뒤가 안 맞는 값을 보고했으므로 입출력 오류로 판정 */
			goto out;	/* [한국어] 레지스터를 원상복구해야 하므로 바로 반환하지 않고 out 으로 간다 */
		}

		busnr = pci_iov_virtfn_bus(dev, nr_virtfn - 1);	/* [한국어] 이 NumVFs 에서 가장 멀리 있는 VF 는 마지막 것(인덱스 nr_virtfn-1)이다 */
		if (busnr > iov->max_VF_buses)	/* [한국어] 지금까지 본 최대 버스 번호보다 크면 */
			iov->max_VF_buses = busnr;	/* [한국어] 갱신한다. 최종값이 "이 PF 가 쓸 수 있는 가장 높은 버스 번호"가 된다 */
	}

out:	/* [한국어] 정상 종료와 오류 종료가 함께 지나가는 정리 지점 */
	pci_iov_set_numvfs(dev, 0);	/* [한국어] 조사 때문에 건드린 NumVFs 를 0 으로 되돌린다 — 실제 VF 활성화는 나중에 sriov_enable 이 한다 */
	return rc;	/* [한국어] 0(성공) 또는 -EIO 를 그대로 sriov_init 에 돌려준다 */
}

/*
 * [한국어]
 * virtfn_add_bus - VF 가 앉을 버스를 찾거나, 없으면 새로 만든다
 *
 * @bus: PF 가 앉아 있는 버스(struct pci_bus). 새 버스의 부모가 된다.
 * @busnr: pci_iov_virtfn_bus() 가 계산한 VF 의 버스 번호.
 * @return: 해당 번호의 struct pci_bus, 만들지 못하면 NULL.
 *          호출자 pci_iov_add_virtfn() 은 NULL 이면 -ENOMEM 으로 실패시킨다.
 *
 * 왜 필요한가: VF 의 RID 는 PF 의 버스를 넘어갈 수 있다(offset/stride 때문).
 * 그런데 PCI 코어의 자료구조에서 pci_dev 는 반드시 어떤 pci_bus 에 속해야
 * 하므로, VF 를 등록하려면 그 번호의 버스 객체가 실제로 있어야 한다.
 * 이 버스는 실물 다리(bridge) 뒤에 있는 물리 버스가 아니라, VF 를 담기 위한
 * "가상 버스"다. 그래서 pci_add_new_bus(bus, NULL, busnr) 처럼 두 번째 인자
 * (다리 역할을 하는 pci_dev)를 NULL 로 넘긴다.
 * 동작 과정: (1) VF 가 PF 와 같은 버스면 그냥 그 버스를 쓴다. (2) 이미 앞선
 * VF 때문에 만들어 둔 버스가 있으면 재사용한다. (3) 없으면 새로 만들고
 * busn_res(그 버스가 차지하는 버스 번호 구간)를 [busnr, busnr] 한 칸으로 등록한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자 sriov_enable() 경로가
 * pci_lock_rescan_remove() 를 잡은 상태(sysfs 경로)이거나 PF 드라이버의
 * probe 안이므로 버스 목록 조작이 직렬화된다.
 *
 * 호출 체인:
 *   sriov_enable() -> sriov_add_vfs() -> pci_iov_add_virtfn() -> [virtfn_add_bus]
 *     -> pci_find_bus(), pci_add_new_bus(), pci_bus_insert_busn_res()
 */
static struct pci_bus *virtfn_add_bus(struct pci_bus *bus, int busnr)	/* [한국어] static — 이 파일 안에서 VF 등록/해제 경로만 쓴다 */
{
	struct pci_bus *child;	/* [한국어] 새로 만들거나 찾아낸 하위 버스를 담을 지역 변수 */

	if (bus->number == busnr)	/* [한국어] VF 의 버스 번호가 PF 와 같다면 새 버스가 필요 없다(offset+stride 합이 256 미만인 흔한 경우) */
		return bus;	/* [한국어] PF 가 앉은 버스를 그대로 돌려준다 */

	child = pci_find_bus(pci_domain_nr(bus), busnr);	/* [한국어] 같은 PCI 도메인(세그먼트) 안에서 그 번호의 버스가 이미 있는지 찾는다 — 앞선 VF 가 만들어 뒀을 수 있다 */
	if (child)	/* [한국어] 있으면 */
		return child;	/* [한국어] 재사용한다. VF 여러 개가 한 가상 버스를 공유한다 */

	child = pci_add_new_bus(bus, NULL, busnr);	/* [한국어] 없으면 새로 만든다. 두 번째 인자(다리 pci_dev)가 NULL 인 것은 이 버스가 실물 다리 뒤가 아닌 VF 전용 가상 버스이기 때문이다 */
	if (!child)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] NULL 을 돌려 호출자가 -ENOMEM 으로 처리하게 한다 */

	pci_bus_insert_busn_res(child, busnr, busnr);	/* [한국어] 이 버스가 차지하는 버스 번호 구간을 [busnr, busnr] 한 칸으로 등록한다. 이후의 버스 번호 배분이 이 구간을 침범하지 않게 하려는 것 */

	return child;	/* [한국어] VF pci_dev 를 매달 버스를 돌려준다 */
}

/*
 * [한국어]
 * virtfn_remove_bus - VF 를 담으려고 만들었던 가상 버스를 비었으면 제거한다
 *
 * @physbus: PF 가 앉아 있는 버스.
 * @virtbus: VF 가 앉아 있던 버스. physbus 와 같을 수도 있다.
 * @return: 없음.
 *
 * 왜 필요한가: virtfn_add_bus() 가 만든 가상 버스는 VF 가 모두 사라지면
 * 남겨 둘 이유가 없다. 남겨 두면 버스 번호와 sysfs 디렉터리가 계속 점유된다.
 * 다만 두 가지를 반드시 확인해야 한다: (1) 그것이 PF 의 진짜 버스면 지우면
 * 안 된다(다른 장치들이 거기 있다), (2) 아직 다른 VF 가 남아 있으면 안 된다.
 * 그래서 physbus 와 다르고 devices 리스트가 비었을 때만 지운다.
 * 실행 컨텍스트: pci_iov_remove_virtfn()/pci_iov_add_virtfn() 의 실패 경로.
 * 프로세스 컨텍스트이며 rescan/remove 락 아래에서 불린다.
 *
 * 호출 체인:
 *   pci_iov_remove_virtfn(), pci_iov_add_virtfn()(실패 되감기)
 *     -> [virtfn_remove_bus] -> pci_remove_bus()
 */
static void virtfn_remove_bus(struct pci_bus *physbus, struct pci_bus *virtbus)	/* [한국어] static — VF 해제 경로 전용 헬퍼 */
{
	if (physbus != virtbus && list_empty(&virtbus->devices))	/* [한국어] PF 의 실제 버스가 아니고(그것을 지우면 다른 장치가 날아간다) 그 버스에 매달린 장치가 하나도 없을 때만 */
		pci_remove_bus(virtbus);	/* [한국어] VF 전용으로 만들었던 가상 버스를 PCI 코어에서 제거한다 */
}

/*
 * [한국어]
 * pci_iov_resource_size - VF 한 개의 BAR 크기를 돌려준다
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF 의 resource[] 인덱스. VF BAR 는 PCI_IOV_RESOURCES 부터 6칸을 쓴다.
 * @return: VF 하나가 쓰는 BAR 크기(바이트). PF 가 아니면 0.
 *
 * 왜 필요한가: PF 의 resource[PCI_IOV_RESOURCES + i] 에 들어 있는 크기는
 * "VF BAR 크기 x TotalVFs" 로 부풀려진 값이다(sriov_init 에서 그렇게 만든다).
 * 정렬 계산이나 VF 별 구간 잘라내기에는 부풀리기 전의 원래 크기가 필요한데,
 * 그 값이 iov->barsz[] 에 따로 보관되어 있다. 이 함수가 그 접근자다.
 * 이것이 VF BAR 가 보통 BAR 와 다른 핵심 지점이다 — 크기가 VF 자신의
 * config space 가 아니라 PF 의 SR-IOV capability 로 정해지고, 모든 VF 가
 * 그 하나의 크기를 공유한다.
 * 실행 컨텍스트: 리소스 배분(setup-res.c) 및 VF 생성 경로. 락 없음(읽기 전용).
 *
 * 호출 체인:
 *   pci_sriov_resource_alignment() -> pcibios_iov_resource_alignment(),
 *   pci_iov_add_virtfn(), sriov_enable() -> [pci_iov_resource_size]
 *     -> pci_resource_num_to_vf_bar()(pci.h 인라인: resno - PCI_IOV_RESOURCES)
 */
resource_size_t pci_iov_resource_size(struct pci_dev *dev, int resno)	/* [한국어] drivers/pci 안에서 여러 파일이 쓰므로 static 이 아니다(선언은 pci.h) */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 dev->sriov 가 NULL 이라 barsz[] 를 볼 수 없다 */
		return 0;	/* [한국어] 크기 0 = "VF BAR 가 아니다" 로 호출자가 해석한다 */

	return dev->sriov->barsz[pci_resource_num_to_vf_bar(resno)];	/* [한국어] resource[] 인덱스를 VF BAR 번호(0~5)로 바꾸어 barsz[] 를 찾는다. barsz 는 VF "한 개"의 크기다 */
}

/*
 * [한국어]
 * pci_iov_resource_set_size - VF Resizable BAR 로 크기를 바꾼 뒤 barsz[] 사본을 맞춘다
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF resource[] 인덱스. 반드시 VF BAR 구간(PCI_IOV_RESOURCES 이상)이어야 한다.
 * @size: PCIe ReBAR 인코딩의 크기 값(0 = 1MB, 31 = 128TB).
 * @return: 없음. 잘못된 resno 면 경고만 찍고 아무 것도 하지 않는다.
 *
 * 왜 필요한가: VF BAR 도 Resizable BAR capability(PCI_EXT_CAP_ID_VF_REBAR)로
 * 크기를 바꿀 수 있다. 그런데 커널은 VF BAR 하나의 크기를 iov->barsz[] 에
 * 캐시해 두고 VF 구간을 자를 때 그 값을 쓰므로, 레지스터만 바꾸고 캐시를
 * 그대로 두면 VF BAR 구간 계산이 전부 어긋난다. 그래서 rebar.c 가 실제
 * 레지스터를 쓴 직후 이 함수로 캐시를 갱신한다.
 * 실행 컨텍스트: sysfs 를 통한 BAR 크기 변경 경로(프로세스 컨텍스트).
 * 이 시점에는 VF 가 켜져 있으면 안 된다 — pci_iov_vf_bar_set_size() 가
 * pci_iov_is_memory_decoding_enabled() 로 그것을 먼저 확인한다.
 * 에러 경로: IOV 리소스가 아니면 pci_warn 만 남기고 조용히 돌아간다.
 *
 * 호출 체인:
 *   drivers/pci/rebar.c 의 pci_rebar_set_size() -> [pci_iov_resource_set_size]
 *     -> pci_rebar_size_to_bytes()(1MB << size)
 */
void pci_iov_resource_set_size(struct pci_dev *dev, int resno, int size)	/* [한국어] rebar.c 가 호출하므로 static 이 아니다 */
{
	if (!pci_resource_is_iov(resno)) {	/* [한국어] resno 가 PCI_IOV_RESOURCES ~ PCI_IOV_RESOURCE_END 범위 밖이면 VF BAR 가 아니다 */
		pci_warn(dev, "%s is not an IOV resource\n",	/* [한국어] 호출자의 버그이므로 어떤 리소스였는지 이름과 함께 경고를 남긴다 */
			 pci_resource_name(dev, resno));	/* [한국어] pci_resource_name 은 "BAR 0" 같은 사람이 읽을 이름을 만들어 준다 */
		return;	/* [한국어] 캐시를 건드리지 않고 그대로 돌아간다 */
	}

	resno = pci_resource_num_to_vf_bar(resno);	/* [한국어] resource[] 인덱스를 VF BAR 번호 0~5 로 변환 */
	dev->sriov->barsz[resno] = pci_rebar_size_to_bytes(size);	/* [한국어] ReBAR 인코딩(0=1MB)을 실제 바이트 수(1MB << size)로 풀어 캐시에 저장한다 */
}

/*
 * [한국어]
 * pci_iov_is_memory_decoding_enabled - VF 들의 메모리 디코딩(MSE)이 켜져 있는지 본다
 *
 * @dev: PF 의 pci_dev. dev->sriov 가 있어야 한다.
 * @return: true 면 VF Memory Space Enable 이 켜져 있어 VF BAR 가 응답 중이다.
 *
 * 왜 필요한가: BAR 의 크기나 주소를 바꾸려면 그 BAR 가 주소 디코딩을 하고
 * 있지 않아야 한다. 보통 장치는 Command 레지스터의 Memory Space 비트를 보지만,
 * VF 는 자기 Command 레지스터의 그 비트가 하드와이어 0 이고 대신 PF 의
 * SR-IOV Control 레지스터 bit3(PCI_SRIOV_CTRL_MSE)이 모든 VF 의 메모리
 * 디코딩을 한꺼번에 제어한다. 그래서 VF BAR 를 다루는 코드는 이 함수로
 * "지금 건드려도 되는가"를 판단한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. config read 한 번뿐이라 락 없음.
 *
 * 호출 체인:
 *   drivers/pci/rebar.c, pci_iov_vf_bar_set_size()
 *     -> [pci_iov_is_memory_decoding_enabled] -> pci_read_config_word()
 */
bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev)	/* [한국어] rebar.c 와 이 파일이 함께 쓰므로 외부 링크 */
{
	u16 cmd;	/* [한국어] SR-IOV Control 레지스터 값을 담을 16비트 변수 */

	pci_read_config_word(dev, dev->sriov->pos + PCI_SRIOV_CTRL, &cmd);	/* [한국어] capability 오프셋 + 0x08(PCI_SRIOV_CTRL) 을 읽는다. 이 한 레지스터가 VF Enable, MSE, ARI 를 모두 담고 있다 */

	return cmd & PCI_SRIOV_CTRL_MSE;	/* [한국어] bit3(0x0008) 이 VF Memory Space Enable — 켜져 있으면 VF BAR 들이 주소를 디코딩 중이라 크기를 바꾸면 안 된다 */
}

/*
 * [한국어]
 * pci_read_vf_config_common - 모든 VF 가 공유하는 config 값들을 VF0 에서 한 번만 읽는다
 *
 * @virtfn: VF0 의 pci_dev(호출자가 id == 0 일 때만 부른다).
 * @return: 없음. 결과는 PF 의 sriov 구조체(class, hdr_type, subsystem_*)에 저장된다.
 *
 * 왜 필요한가: VF 수십~수백 개를 만들 때마다 같은 값을 config space 에서
 * 반복해 읽으면 느리다. config 접근은 한 번에 수 마이크로초가 걸리는
 * 비싼 연산이고, VF 가 많으면 열거 시간이 눈에 띄게 길어진다. VF 들은
 * 사실상 같은 장치의 복제이므로 Class Code/Revision, Header Type,
 * Subsystem ID 가 같다고 보고 VF0 에서 한 번만 읽어 PF 쪽에 캐시해 둔다.
 * 이후 VF1 부터는 pci_setup_device() 가 이 캐시를 쓴다.
 * 원문 영어 주석이 밝히듯 PCIe r4.0 9.3.4.1 이 "모든 VF 의 Revision/Subsystem
 * ID 가 같아야 한다"고 강제하지는 않는다 — 커널이 그렇다고 가정하는 것이다.
 * 실행 컨텍스트: VF 생성 경로(pci_iov_scan_device). 프로세스 컨텍스트.
 * 아직 이 VF 는 아무 리스트에도 등록되지 않아 다른 스레드가 볼 수 없다.
 *
 * 호출 체인:
 *   pci_iov_add_virtfn() -> pci_iov_scan_device() -> [pci_read_vf_config_common]
 *     -> pci_read_config_dword/byte/word()
 */
static void pci_read_vf_config_common(struct pci_dev *virtfn)	/* [한국어] static — VF 생성 경로에서만 쓰는 헬퍼 */
{
	struct pci_dev *physfn = virtfn->physfn;	/* [한국어] 읽은 값을 저장할 곳은 VF 가 아니라 PF 의 sriov 구조체다. VF 전부가 그 값을 공유하기 때문 */

	/*
	 * Some config registers are the same across all associated VFs.
	 * Read them once from VF0 so we can skip reading them from the
	 * other VFs.
	 *
	 * PCIe r4.0, sec 9.3.4.1, technically doesn't require all VFs to
	 * have the same Revision ID and Subsystem ID, but we assume they
	 * do.
	 */
	pci_read_config_dword(virtfn, PCI_CLASS_REVISION,	/* [한국어] config space 오프셋 0x08(PCI_CLASS_REVISION): 상위 24비트가 Class Code, 하위 8비트가 Revision ID */
			      &physfn->sriov->class);	/* [한국어] PF 의 sriov->class 에 캐시한다 */
	pci_read_config_byte(virtfn, PCI_HEADER_TYPE,	/* [한국어] 오프셋 0x0e(PCI_HEADER_TYPE): 헤더 유형(0=일반 장치)과 multifunction 비트 */
			     &physfn->sriov->hdr_type);	/* [한국어] PF 의 sriov->hdr_type 에 캐시 */
	pci_read_config_word(virtfn, PCI_SUBSYSTEM_VENDOR_ID,	/* [한국어] 오프셋 0x2c(PCI_SUBSYSTEM_VENDOR_ID): 보드 제조사 ID */
			     &physfn->sriov->subsystem_vendor);	/* [한국어] PF 의 sriov->subsystem_vendor 에 캐시 */
	pci_read_config_word(virtfn, PCI_SUBSYSTEM_ID,	/* [한국어] 오프셋 0x2e(PCI_SUBSYSTEM_ID): 보드 모델 ID */
			     &physfn->sriov->subsystem_device);	/* [한국어] PF 의 sriov->subsystem_device 에 캐시 */
}

/*
 * [한국어]
 * pci_iov_sysfs_link - PF 와 VF 사이의 sysfs 심볼릭 링크 한 쌍을 만든다
 *
 * @dev: PF 의 pci_dev.
 * @virtfn: 방금 만든 VF 의 pci_dev.
 * @id: VF 번호(0 부터). 링크 이름 "virtfn<id>" 에 들어간다.
 * @return: 0 성공, 음수면 sysfs_create_link 의 오류 코드.
 *          호출자 pci_iov_add_virtfn() 은 실패 시 VF 등록 전체를 되감는다.
 *
 * 왜 필요한가: 사용자 공간(lspci, libvirt, 관리 스크립트)이 "이 PF 의 VF 는
 * 무엇인가", "이 VF 의 PF 는 누구인가"를 알 수 있게 하는 유일한 인터페이스다.
 * 결과적으로 /sys/bus/pci/devices/<PF>/virtfn0 -> ../<VF BDF> 와
 * /sys/bus/pci/devices/<VF>/physfn -> ../<PF BDF> 두 링크가 생긴다.
 * 동작 과정: PF -> VF 링크를 먼저 만들고, 그 다음 VF -> PF 링크를 만든다.
 * 둘 다 성공하면 uevent 를 보내 udev 가 새 속성을 다시 읽게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(VF 생성 중). sysfs 조작이므로 잠들 수 있다.
 * 에러 경로: 두 번째 링크가 실패하면 failed1 에서 첫 링크를 지워
 * "반쪽만 연결된 상태"를 남기지 않는다.
 *
 * 호출 체인:
 *   sriov_enable() -> sriov_add_vfs() -> pci_iov_add_virtfn()
 *     -> [pci_iov_sysfs_link] -> sysfs_create_link(), kobject_uevent()
 */
int pci_iov_sysfs_link(struct pci_dev *dev,	/* [한국어] include/linux/pci.h 에 선언된 외부 공개 함수 — VF 를 스스로 만드는 드라이버도 이 링크를 만들 수 있게 한다 */
		struct pci_dev *virtfn, int id)	/* [한국어] VF 의 pci_dev 와 그 번호 */
{
	char buf[VIRTFN_ID_LEN];	/* [한국어] "virtfn%u" 이름을 담을 스택 버퍼. 크기는 위에서 정의한 VIRTFN_ID_LEN(17) */
	int rc;	/* [한국어] sysfs_create_link 의 반환값을 담을 변수 */

	sprintf(buf, "virtfn%u", id);	/* [한국어] PF 디렉터리에 만들 링크 이름을 만든다. VF 번호가 그대로 이름이 된다 */
	rc = sysfs_create_link(&dev->dev.kobj, &virtfn->dev.kobj, buf);	/* [한국어] PF 의 kobject 아래에 VF 를 가리키는 심볼릭 링크를 만든다 — /sys/.../<PF>/virtfn0 */
	if (rc)	/* [한국어] 실패하면(이름 충돌, 메모리 부족 등) */
		goto failed;	/* [한국어] 아직 만든 것이 없으므로 되돌릴 것 없이 바로 반환 지점으로 간다 */
	rc = sysfs_create_link(&virtfn->dev.kobj, &dev->dev.kobj, "physfn");	/* [한국어] 반대 방향 링크: VF 디렉터리 아래 "physfn" 이라는 이름으로 PF 를 가리킨다 */
	if (rc)	/* [한국어] 두 번째 링크가 실패하면 */
		goto failed1;	/* [한국어] 먼저 만든 링크를 지워야 하므로 failed1 로 간다 */

	kobject_uevent(&virtfn->dev.kobj, KOBJ_CHANGE);	/* [한국어] KOBJ_CHANGE uevent 를 보내 udev 가 이 VF 의 속성 변화를 다시 읽게 한다 */

	return 0;	/* [한국어] 두 링크가 모두 생겼다 */

failed1:	/* [한국어] VF->PF 링크 생성 실패 되감기 지점 */
	sysfs_remove_link(&dev->dev.kobj, buf);	/* [한국어] 이미 만들어 둔 PF->VF 링크를 제거해 상태를 원래대로 되돌린다 */
failed:	/* [한국어] 아무 것도 만들지 못한 경우가 합류하는 지점 */
	return rc;	/* [한국어] 어느 단계에서 실패했든 그 오류 코드를 그대로 전달한다 */
}

#ifdef CONFIG_PCI_MSI	/* [한국어] MSI-X 관련 sysfs 속성은 CONFIG_PCI_MSI 가 켜졌을 때만 의미가 있다 */
/*
 * [한국어]
 * sriov_vf_total_msix_show - PF 의 sysfs 파일 sriov_vf_total_msix 읽기 핸들러
 *
 * @dev: PF 의 struct device(감싸고 있는 pci_dev 를 to_pci_dev 로 꺼낸다).
 * @attr: 어떤 attribute 를 읽는지 — 이 핸들러는 하나뿐이라 쓰지 않는다.
 * @buf: 사용자에게 돌려줄 한 페이지짜리 출력 버퍼.
 * @return: buf 에 채운 바이트 수. 항상 성공한다(값을 못 구하면 0 을 출력).
 *
 * 왜 필요한가: 어떤 장치는 VF 들이 나눠 쓸 MSI-X 벡터의 총량이 정해져 있고,
 * 관리자가 그 총량을 알아야 VF 별로 몇 개씩 배분할지 정할 수 있다. 그 총량은
 * 장치마다 계산 방법이 달라 PCI 코어가 알 수 없으므로, PF 드라이버가 등록한
 * .sriov_get_vf_total_msix 콜백에 물어본다.
 * 동작 과정: device_lock 으로 PF 에 드라이버가 붙었다 떨어지는 것을 막고,
 * 콜백이 있으면 부르고, 없으면 0 을 그대로 출력한다.
 * 실행 컨텍스트: sysfs read(프로세스 컨텍스트). device_lock 은 뮤텍스라 잠들 수 있다.
 * 왜 락이 필요한가: pdev->driver 를 검사한 직후 드라이버가 언바인드되면
 * 이미 해제된 pci_driver 의 함수 포인터를 부르게 된다. device_lock 이
 * 드라이버 바인딩/언바인딩과 이 읽기를 직렬화해 그 경쟁을 막는다.
 * NVMe: drivers/nvme/host/pci.c 의 nvme_driver 는 이 콜백을 등록하지 않으므로
 * NVMe PF 에서 이 파일을 읽으면 항상 0 이 나온다(grep 으로 확인).
 *
 * 호출 체인:
 *   사용자 cat /sys/.../sriov_vf_total_msix -> sysfs -> [sriov_vf_total_msix_show]
 *     -> pdev->driver->sriov_get_vf_total_msix()(드라이버가 등록했을 때만)
 */
static ssize_t sriov_vf_total_msix_show(struct device *dev,	/* [한국어] DEVICE_ATTR_RO 가 요구하는 show 시그니처(device, attribute, buf) */
					struct device_attribute *attr,
					char *buf)	/* [한국어] 출력 버퍼. sysfs 는 PAGE_SIZE 를 보장한다 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] struct device 를 감싸고 있는 pci_dev 로 되돌린다(container_of 매크로) */
	u32 vf_total_msix = 0;	/* [한국어] 콜백이 없을 때 출력할 기본값 0 */

	device_lock(dev);	/* [한국어] PF 에 드라이버가 붙어 있는 상태를 이 함수 동안 고정한다 — 아래 두 줄 사이에 언바인드가 끼어들면 해제된 포인터를 부르게 된다 */
	if (!pdev->driver || !pdev->driver->sriov_get_vf_total_msix)	/* [한국어] 드라이버가 없거나 이 콜백을 등록하지 않았으면 물어볼 곳이 없다 */
		goto unlock;	/* [한국어] 0 을 그대로 출력하도록 잠금 해제 지점으로 간다 */

	vf_total_msix = pdev->driver->sriov_get_vf_total_msix(pdev);	/* [한국어] PF 드라이버에게 "VF 들이 나눠 쓸 수 있는 MSI-X 벡터 총수"를 묻는다 */
unlock:	/* [한국어] 성공/실패 경로가 합류해 락을 푸는 지점 */
	device_unlock(dev);	/* [한국어] device_lock 해제 — 이 뒤로는 pdev->driver 를 만지지 않는다 */
	return sysfs_emit(buf, "%u\n", vf_total_msix);	/* [한국어] sysfs_emit 은 버퍼 오버런 없이 한 페이지 안에 안전하게 포맷해 준다 */
}
static DEVICE_ATTR_RO(sriov_vf_total_msix);	/* [한국어] 읽기 전용 속성 dev_attr_sriov_vf_total_msix 를 만든다(모드 0444). PF 디렉터리에 붙는다 */

/*
 * [한국어]
 * sriov_vf_msix_count_store - VF 의 sysfs 파일 sriov_vf_msix_count 쓰기 핸들러
 *
 * @dev: 대상 VF 의 struct device. 이 속성은 VF 쪽 디렉터리에 붙는다.
 * @attr: 사용하지 않는다.
 * @buf: 사용자가 쓴 문자열(십진/십육진 정수).
 * @count: buf 의 길이. 성공 시 그대로 반환해 "전부 소비했다"고 알린다.
 * @return: 성공 시 count, 실패 시 음수 오류 코드
 *          (-EINVAL 형식 오류, -EOPNOTSUPP 콜백 없음, -EBUSY VF 에 드라이버가 붙음).
 *
 * 왜 필요한가: VF 하나가 쓸 MSI-X 벡터 수를 관리자가 바꿀 수 있게 한다.
 * 벡터는 장치 전체가 공유하는 한정 자원이라, 어떤 VF 에 많이 주면 다른
 * VF 가 못 쓴다. 그 배분은 PF 드라이버만 할 수 있으므로 여기서는 검증만 하고
 * .sriov_set_msix_vec_count 콜백으로 넘긴다.
 * 락 순서가 중요하다: 먼저 PF 의 device_lock, 그 다음 VF 의 device_lock 을 잡는다.
 * 두 락을 잡는 이유는 (1) PF 드라이버가 사라지지 않게, (2) VF 에 드라이버가
 * 새로 붙지 않게 하기 위해서다. VF 에 이미 드라이버가 붙어 있으면 그 드라이버가
 * 현재 벡터 수를 전제로 큐를 만들어 두었을 수 있어 -EBUSY 로 거절한다
 * (원문 영어 주석의 근거).
 * 실행 컨텍스트: sysfs write. 프로세스 컨텍스트, 잠들 수 있다.
 * NVMe: nvme_driver 는 .sriov_set_msix_vec_count 를 등록하지 않으므로
 * NVMe VF 에 이 값을 쓰면 -EOPNOTSUPP 가 난다(grep 으로 확인).
 *
 * 호출 체인:
 *   사용자 echo N > /sys/.../<VF>/sriov_vf_msix_count -> sysfs
 *     -> [sriov_vf_msix_count_store] -> pdev->driver->sriov_set_msix_vec_count()
 */
static ssize_t sriov_vf_msix_count_store(struct device *dev,	/* [한국어] DEVICE_ATTR_WO 가 요구하는 store 시그니처 */
					 struct device_attribute *attr,
					 const char *buf, size_t count)	/* [한국어] 사용자가 쓴 바이트열과 그 길이 */
{
	struct pci_dev *vf_dev = to_pci_dev(dev);	/* [한국어] 이 속성이 붙은 장치는 VF 다 */
	struct pci_dev *pdev = pci_physfn(vf_dev);	/* [한국어] pci_physfn 으로 그 VF 의 PF 를 얻는다 — 벡터 배분은 PF 드라이버만 할 수 있다 */
	int val, ret = 0;	/* [한국어] val: 요청 벡터 수, ret: 반환값(기본 0 = 성공) */

	if (kstrtoint(buf, 0, &val) < 0)	/* [한국어] 문자열을 정수로 바꾼다. base 0 이라 0x 접두사도 받는다 */
		return -EINVAL;	/* [한국어] 숫자가 아니면 형식 오류 */

	if (val < 0)	/* [한국어] 음수 벡터 수는 의미가 없다 */
		return -EINVAL;	/* [한국어] 범위 오류도 -EINVAL 로 돌려준다 */

	device_lock(&pdev->dev);	/* [한국어] PF 의 device_lock — 아래에서 PF 드라이버의 콜백을 부르는 동안 언바인드를 막는다 */
	if (!pdev->driver || !pdev->driver->sriov_set_msix_vec_count) {	/* [한국어] PF 에 드라이버가 없거나 벡터 수 조정 콜백을 등록하지 않았다면 */
		ret = -EOPNOTSUPP;	/* [한국어] 이 장치는 이 기능을 지원하지 않는다는 뜻 */
		goto err_pdev;	/* [한국어] PF 락만 잡은 상태이므로 그것만 푸는 지점으로 간다 */
	}

	device_lock(&vf_dev->dev);	/* [한국어] VF 의 device_lock — 검사와 콜백 호출 사이에 VF 드라이버가 붙는 것을 막는다. 순서는 항상 PF 먼저, VF 나중(교착 방지) */
	if (vf_dev->driver) {	/* [한국어] VF 에 이미 드라이버가 붙어 있으면 */
		/*
		 * A driver is already attached to this VF and has configured
		 * itself based on the current MSI-X vector count. Changing
		 * the vector size could mess up the driver, so block it.
		 */
		ret = -EBUSY;	/* [한국어] 그 드라이버가 현재 벡터 수를 전제로 초기화되어 있으므로 바꾸면 안 된다 */
		goto err_dev;	/* [한국어] 두 락을 모두 풀어야 하므로 안쪽 되감기 지점으로 간다 */
	}

	ret = pdev->driver->sriov_set_msix_vec_count(vf_dev, val);	/* [한국어] PF 드라이버에게 "이 VF 의 MSI-X 벡터 수를 val 로 맞춰라"고 요청한다 */

err_dev:	/* [한국어] VF 락을 푸는 지점 */
	device_unlock(&vf_dev->dev);	/* [한국어] 나중에 잡은 VF 락을 먼저 푼다 */
err_pdev:	/* [한국어] PF 락을 푸는 지점 */
	device_unlock(&pdev->dev);	/* [한국어] 마지막으로 PF 락 해제 */
	return ret ? : count;	/* [한국어] GNU 확장 문법: ret 가 0 이 아니면 ret(오류), 0 이면 count 를 돌려준다. sysfs 규약상 "전부 소비했다"는 뜻으로 count 를 준다 */
}
static DEVICE_ATTR_WO(sriov_vf_msix_count);	/* [한국어] 쓰기 전용 속성 dev_attr_sriov_vf_msix_count 를 만든다(모드 0200). VF 디렉터리에 붙는다 */
#endif

/*
 * [한국어]
 * sriov_vf_dev_attrs - VF 의 sysfs 디렉터리에 붙일 속성 목록
 *
 * 역할: attribute_group 이 요구하는 NULL 로 끝나는 배열. 여기에 든 파일만
 * VF 디렉터리에 만들어진다.
 * 설정자: 컴파일 시각에 고정. 런타임에 바뀌지 않는다.
 * 읽는 자: 아래 sriov_vf_dev_attr_group 을 통해 드라이버 코어(sysfs 계층)가 읽는다.
 * 값 범위: struct attribute 포인터들과 마지막 NULL.
 * 동기화: 읽기 전용 정적 데이터라 락이 필요 없다.
 */
static struct attribute *sriov_vf_dev_attrs[] = {	/* [한국어] static — pci-sysfs.c 는 아래 group 만 참조하고 이 배열을 직접 보지 않는다 */
#ifdef CONFIG_PCI_MSI	/* [한국어] MSI-X 가 꺼진 커널에서는 dev_attr_sriov_vf_msix_count 자체가 없다 */
	&dev_attr_sriov_vf_msix_count.attr,	/* [한국어] VF 당 MSI-X 벡터 수를 쓰는 파일 */
#endif
	NULL,	/* [한국어] 배열의 끝을 알리는 표지. sysfs 코어가 이것을 보고 순회를 멈춘다 */
};

/*
 * [한국어]
 * sriov_vf_attrs_are_visible - 이 장치에 VF 용 sysfs 파일을 실제로 만들지 결정한다
 *
 * @kobj: 검사 대상 장치의 kobject.
 * @a: 만들지 말지 판단할 attribute.
 * @n: 그룹 안에서의 인덱스. 여기서는 쓰지 않는다.
 * @return: 0 이면 파일을 만들지 않는다. 0 이 아니면 그 값이 파일의 퍼미션이 된다.
 *
 * 왜 필요한가: sriov_vf_dev_attr_group 은 모든 PCI 장치에 대해 평가된다.
 * 그런데 VF 전용 속성을 VF 가 아닌 장치에까지 만들면 사용자에게 혼란을 주고
 * 핸들러가 엉뚱한 장치에서 불릴 수 있다. is_visible 콜백은 그 필터다.
 * 실행 컨텍스트: 장치 등록 시각(device_add) 프로세스 컨텍스트. 락 없음 —
 * 아직 이 장치는 사용자에게 노출되기 전이다.
 *
 * 호출 체인:
 *   pci_device_add() -> device_add() -> sysfs 그룹 생성
 *     -> [sriov_vf_attrs_are_visible]
 */
static umode_t sriov_vf_attrs_are_visible(struct kobject *kobj,	/* [한국어] attribute_group 의 is_visible 콜백 시그니처 */
					  struct attribute *a, int n)	/* [한국어] 판단할 attribute 와 그룹 안 인덱스 */
{
	struct device *dev = kobj_to_dev(kobj);	/* [한국어] kobject 를 감싸고 있는 struct device 로 되돌린다 */
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] 다시 pci_dev 로 되돌린다 — is_virtfn 플래그를 봐야 하기 때문 */

	if (!pdev->is_virtfn)	/* [한국어] VF 가 아닌 장치라면 */
		return 0;	/* [한국어] 0 을 돌려 이 파일을 만들지 않게 한다 */

	return a->mode;	/* [한국어] VF 라면 선언된 퍼미션(DEVICE_ATTR_WO 이므로 0200)을 그대로 쓴다 */
}

/*
 * [한국어]
 * sriov_vf_dev_attr_group - VF 쪽 sysfs 속성 그룹
 *
 * 역할: drivers/pci/pci-sysfs.c 의 pci_dev_attr_groups 목록에 등록되어
 * 모든 PCI 장치에 대해 평가되고, is_visible 이 통과시킨 VF 에만 파일을 만든다.
 * 설정자: 컴파일 시각 상수. const 라 읽기 전용 섹션에 놓인다.
 * 읽는 자: 드라이버 코어의 device_add()/device_del() 경로.
 * 값 범위: attrs 는 위 배열, is_visible 은 위 콜백.
 * 동기화: 정적 상수라 동기화 불필요.
 */
const struct attribute_group sriov_vf_dev_attr_group = {	/* [한국어] const + 비static — pci-sysfs.c 가 extern 선언으로 참조한다(pci.h) */
	.attrs = sriov_vf_dev_attrs,	/* [한국어] 이 그룹이 만들 파일 목록 */
	.is_visible = sriov_vf_attrs_are_visible,	/* [한국어] 장치별로 만들지 말지 거르는 콜백 */
};

/*
 * [한국어]
 * pci_iov_scan_device - VF 하나에 대응하는 struct pci_dev 를 만들어 초기화한다
 *
 * @dev: PF 의 pci_dev.
 * @id: 만들 VF 의 번호(0 부터).
 * @bus: 이 VF 가 앉을 버스(virtfn_add_bus 가 준비한 것).
 * @return: 성공하면 새 pci_dev 포인터, 실패하면 ERR_PTR(-ENOMEM) 또는
 *          pci_setup_device() 의 오류. 호출자는 IS_ERR() 로 검사한다.
 *
 * 왜 필요한가: VF 는 열거(scan)로 발견되는 장치가 아니다. 보통 장치는
 * config space 를 읽어 Vendor ID 가 0xffff 가 아니면 존재한다고 판단하지만,
 * VF 의 Vendor/Device ID 는 VF 자신의 config space 에서 읽을 수 없고(하드와이어
 * 0xffff 로 읽히는 구현이 있다) PF 의 SR-IOV capability 가 알려 준다.
 * 그래서 커널이 pci_dev 를 손으로 만들고 필요한 식별 정보를 직접 채운다.
 * 동작 과정: (1) 빈 pci_dev 를 할당하고, (2) devfn 은 offset/stride 로 계산하고,
 * (3) Vendor ID 는 PF 와 같고 Device ID 는 capability 의 VF Device ID 를 쓰고,
 * (4) is_virtfn/physfn 으로 PF 와 묶고, (5) VF0 이면 공통 config 값을 캐시하고,
 * (6) pci_setup_device() 로 나머지 표준 헤더 파싱(BAR, IRQ 등)을 시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 아직 어떤 리스트에도 등록되지 않은
 * pci_dev 를 다루므로 다른 스레드가 볼 수 없어 락이 필요 없다.
 * 에러 경로: pci_setup_device() 가 실패하면 PF 참조 카운트, 버스 참조,
 * 할당한 메모리를 손으로 되돌린다(아직 pci_device_add 전이라 kfree 가 맞다).
 *
 * 호출 체인:
 *   pci_iov_add_virtfn() -> [pci_iov_scan_device]
 *     -> pci_alloc_dev(), pci_iov_virtfn_devfn(), pci_read_vf_config_common(),
 *        pci_setup_device()
 */
static struct pci_dev *pci_iov_scan_device(struct pci_dev *dev, int id,	/* [한국어] static — VF 생성 경로 전용 */
					   struct pci_bus *bus)	/* [한국어] VF 가 앉을 버스 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태 — VF Device ID 를 여기서 꺼낸다 */
	struct pci_dev *virtfn;	/* [한국어] 새로 만들 VF 의 pci_dev */
	int rc;	/* [한국어] pci_setup_device 의 반환값 */

	virtfn = pci_alloc_dev(bus);	/* [한국어] bus 에 매달릴 빈 pci_dev 를 할당한다. 내부에서 pci_bus_get 으로 버스 참조도 하나 잡는다 */
	if (!virtfn)	/* [한국어] 메모리 부족이면 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 포인터 반환 함수이므로 ERR_PTR 로 오류를 실어 보낸다 */

	virtfn->devfn = pci_iov_virtfn_devfn(dev, id);	/* [한국어] VF 의 devfn 은 스캔이 아니라 offset/stride 계산으로 정해진다 */
	virtfn->vendor = dev->vendor;	/* [한국어] VF 의 Vendor ID 는 항상 PF 와 같다(SR-IOV 스펙). 그래서 PF 값을 복사한다 */
	virtfn->device = iov->vf_device;	/* [한국어] Device ID 만 다르다 — capability 의 VF Device ID(+0x1a)에서 읽어 둔 값 */
	virtfn->is_virtfn = 1;	/* [한국어] 이 pci_dev 가 VF 임을 표시. 전원 관리, ASPM, 리셋 등 여러 경로가 이 플래그로 동작을 바꾼다 */
	virtfn->physfn = pci_dev_get(dev);	/* [한국어] PF 를 가리키고 동시에 PF 의 참조 카운트를 하나 올린다 — VF 가 사는 동안 PF 구조체가 사라지면 안 된다 */
	virtfn->no_command_memory = 1;	/* [한국어] VF 의 Command 레지스터에는 Memory Space Enable 비트가 없다(하드와이어 0). 메모리 디코딩은 PF 의 SR-IOV Control MSE 비트가 일괄 제어하므로 그 사실을 코어에 알린다 */

	if (id == 0)	/* [한국어] 첫 번째 VF 일 때만 */
		pci_read_vf_config_common(virtfn);	/* [한국어] 모든 VF 가 공유하는 Class/Revision, Header Type, Subsystem ID 를 읽어 PF 에 캐시한다 */

	rc = pci_setup_device(virtfn);	/* [한국어] 표준 config 헤더 파싱 — BAR 크기 측정, IRQ 라인, class 설정 등을 공통 코드가 처리한다 */
	if (rc) {	/* [한국어] 실패하면 지금까지 잡은 것을 손으로 되돌린다 */
		pci_dev_put(dev);	/* [한국어] 위에서 pci_dev_get 으로 올린 PF 참조를 되돌린다 */
		pci_bus_put(virtfn->bus);	/* [한국어] pci_alloc_dev 가 잡은 버스 참조를 되돌린다 */
		kfree(virtfn);	/* [한국어] 아직 pci_device_add 전이라 코어가 모르는 객체다. 따라서 kfree 로 직접 해제하는 것이 맞다 */
		return ERR_PTR(rc);	/* [한국어] pci_setup_device 의 오류 코드를 그대로 포장해 반환 */
	}

	return virtfn;	/* [한국어] 아직 코어에 등록되지 않은, 초기화만 끝난 VF pci_dev 를 돌려준다 */
}

/*
 * [한국어]
 * pci_iov_add_virtfn - VF 한 개를 PCI 코어에 정식으로 등록한다
 *
 * @dev: PF 의 pci_dev.
 * @id: 등록할 VF 번호(0 부터).
 * @return: 0 성공, 음수 오류. 호출자 sriov_add_vfs() 는 실패하면 이미 만든
 *          VF 들을 역순으로 지운다.
 *
 * 왜 필요한가: 하드웨어에 VF Enable 을 세우면 VF 가 config space 에 나타나지만,
 * 커널 입장에서는 아직 아무 것도 아니다. 이 함수가 (1) 버스를 마련하고,
 * (2) pci_dev 를 만들고, (3) VF BAR 구간을 PF 가 잡아 둔 큰 영역에서 잘라
 * request_resource 로 예약하고, (4) 코어에 등록해 드라이버 매칭을 시작시킨다.
 *
 * VF BAR 를 자르는 부분이 이 파일의 핵심이다:
 * PF 의 resource[PCI_IOV_RESOURCES + i] 는 sriov_init() 에서
 * "VF BAR 하나의 크기 x TotalVFs" 로 부풀려져 있다. VF id 의 BAR i 는
 * 그 영역 안에서 [start + size*id, start + size*(id+1)) 구간이다.
 * 즉 모든 VF 의 같은 번호 BAR 가 한 덩어리 안에 stride 없이 균등하게
 * 나란히 놓인다. 보통 BAR 처럼 함수마다 따로 주소를 할당받는 것이 아니다.
 * request_resource 는 그 하위 구간을 부모(PF 영역)에 자식으로 등록해
 * 겹침을 막는다. 계산이 정확하므로 실패는 커널 버그이고, 그래서 BUG_ON 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. sysfs 경로에서는 호출자가
 * pci_lock_rescan_remove() 를 잡은 상태라 장치 추가/제거가 직렬화된다.
 * 에러 경로는 3단 되감기다: failed1(코어 등록까지 마친 뒤 실패) ->
 * failed0(pci_dev 만 만든 뒤 실패) -> failed(버스도 못 얻음).
 *
 * 호출 체인:
 *   sriov_enable() -> sriov_add_vfs() -> [pci_iov_add_virtfn]
 *     -> virtfn_add_bus(), pci_iov_scan_device(), request_resource(),
 *        pci_device_add(), pci_iov_sysfs_link(), pci_bus_add_device()
 */
int pci_iov_add_virtfn(struct pci_dev *dev, int id)	/* [한국어] include/linux/pci.h 에 선언된 공개 함수 — VF 를 직접 관리하는 드라이버도 쓸 수 있다 */
{
	struct pci_bus *bus;	/* [한국어] VF 가 앉을 버스 */
	struct pci_dev *virtfn;	/* [한국어] 새로 만든 VF 의 pci_dev */
	struct resource *res;	/* [한국어] PF 쪽 VF BAR 영역(부모 리소스)을 가리킬 포인터 */
	int rc, i;	/* [한국어] rc: 오류 코드, i: VF BAR 번호(0~5) 루프 변수 */
	u64 size;	/* [한국어] VF BAR 하나의 크기. 64비트 BAR 를 담을 수 있게 u64 */

	bus = virtfn_add_bus(dev->bus, pci_iov_virtfn_bus(dev, id));	/* [한국어] VF 의 버스 번호를 계산해 그 버스를 찾거나 새로 만든다 */
	if (!bus) {	/* [한국어] 버스를 못 만들었다면 */
		rc = -ENOMEM;	/* [한국어] 메모리 부족으로 간주하고 */
		goto failed;	/* [한국어] 되돌릴 것이 없는 지점으로 간다 */
	}

	virtfn = pci_iov_scan_device(dev, id, bus);	/* [한국어] VF 의 pci_dev 를 만들어 식별 정보를 채운다 */
	if (IS_ERR(virtfn)) {	/* [한국어] ERR_PTR 로 포장된 오류인지 검사 */
		rc = PTR_ERR(virtfn);	/* [한국어] 포인터에서 오류 코드를 꺼내고 */
		goto failed0;	/* [한국어] 버스만 되돌리는 지점으로 간다 */
	}

	virtfn->dev.parent = dev->dev.parent;	/* [한국어] VF 의 sysfs 부모를 PF 와 같게 맞춘다 — VF 가 만들어 낸 가상 버스가 아니라 실제 상위 다리 아래에 보이게 하려는 것 */
	virtfn->multifunction = 0;	/* [한국어] VF 는 multifunction 장치의 함수가 아니다. 이 플래그가 서면 코어가 같은 slot 의 다른 함수를 찾으려 하므로 명시적으로 끈다 */

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {	/* [한국어] VF BAR 6개(PCI_SRIOV_NUM_BARS) 각각에 대해 */
		int idx = pci_resource_num_from_vf_bar(i);	/* [한국어] VF BAR 번호 i 를 PF 의 resource[] 인덱스로 바꾼다(i + PCI_IOV_RESOURCES) */

		res = &dev->resource[idx];	/* [한국어] PF 가 잡아 둔 "VF BAR 크기 x TotalVFs" 크기의 큰 영역 */
		if (!res->parent)	/* [한국어] parent 가 없다는 것은 그 영역이 실제로 배정되지 못했다는 뜻이다 */
			continue;	/* [한국어] 그런 BAR 는 VF 에도 만들어 줄 수 없으므로 건너뛴다 */
		virtfn->resource[i].name = pci_name(virtfn);	/* [한국어] proc/iomem 등에 보일 이름을 VF 의 BDF 문자열로 지정 */
		virtfn->resource[i].flags = res->flags;	/* [한국어] MEM/IO, prefetchable, 64비트 여부 같은 속성은 PF 의 VF BAR 영역에서 그대로 물려받는다 */
		size = pci_iov_resource_size(dev, idx);	/* [한국어] VF "한 개"의 BAR 크기(iov->barsz[i])를 얻는다. 부풀려진 전체 크기가 아니다 */
		resource_set_range(&virtfn->resource[i],	/* [한국어] VF id 의 몫은 큰 영역의 시작에서 size*id 만큼 떨어진 곳부터 size 바이트다 */
				   res->start + size * id, size);	/* [한국어] 모든 VF 의 같은 번호 BAR 가 이렇게 균등하게 나란히 놓인다 */
		rc = request_resource(res, &virtfn->resource[i]);	/* [한국어] PF 영역을 부모로 삼아 그 하위 구간을 예약한다. 다른 VF 나 다른 장치와 겹치면 실패한다 */
		BUG_ON(rc);	/* [한국어] 계산이 결정적이라 겹칠 수가 없다. 실패한다면 커널 자료구조가 깨진 것이므로 즉시 멈춘다 */
	}

	pci_device_add(virtfn, virtfn->bus);	/* [한국어] VF 를 PCI 코어에 등록한다 — 이 순간부터 버스 장치 목록과 sysfs 에 나타난다 */
	rc = pci_iov_sysfs_link(dev, virtfn, id);	/* [한국어] PF <-> VF 심볼릭 링크 한 쌍을 만든다 */
	if (rc)	/* [한국어] 링크 생성 실패면 */
		goto failed1;	/* [한국어] 이미 코어에 등록된 상태이므로 정식 제거 경로로 되감아야 한다 */

	pci_bus_add_device(virtfn);	/* [한국어] 드라이버 바인딩을 시작한다. 여기서 VF 에 맞는 드라이버의 probe 가 불릴 수 있다 */

	return 0;	/* [한국어] VF 하나가 온전히 등록되었다 */

failed1:	/* [한국어] pci_device_add 까지 마친 뒤 실패한 경우의 되감기 */
	pci_stop_and_remove_bus_device(virtfn);	/* [한국어] 코어에서 정식으로 떼어 내고 pci_dev 참조를 놓는다 */
	pci_dev_put(dev);	/* [한국어] pci_iov_scan_device 가 잡은 PF 참조를 되돌린다 */
failed0:	/* [한국어] pci_dev 를 만들지 못한 경우가 합류하는 지점 */
	virtfn_remove_bus(dev->bus, bus);	/* [한국어] VF 때문에 만들었던 가상 버스가 비었으면 제거한다 */
failed:	/* [한국어] 버스조차 얻지 못한 경우가 합류하는 지점 — 되돌릴 것이 없다 */

	return rc;	/* [한국어] 어느 단계에서 실패했든 그 오류 코드를 전달한다 */
}

/*
 * [한국어]
 * pci_iov_remove_virtfn - VF 한 개를 코어에서 떼어 내고 관련 자원을 되돌린다
 *
 * @dev: PF 의 pci_dev.
 * @id: 지울 VF 번호.
 * @return: 없음. 이미 사라진 VF 를 지우라고 해도 조용히 돌아간다.
 *
 * 왜 필요한가: pci_iov_add_virtfn() 의 정확한 역연산이다. VF 를 지우지 않고
 * 하드웨어의 VF Enable 만 끄면, 커널에는 존재하지 않는 장치를 가리키는
 * pci_dev 가 남아 접근 시 머신 체크가 난다. 그래서 항상 "커널 쪽 VF 제거"가
 * "하드웨어 VF Enable 끄기"보다 먼저다(sriov_disable 의 순서 참조).
 * 동작 과정: (1) offset/stride 로 VF 의 RID 를 계산해 pci_dev 를 찾고,
 * (2) sysfs 링크를 지우고, (3) 코어에서 제거하고, (4) 가상 버스를 정리하고,
 * (5) 참조 카운트 두 개(조회용, physfn 용)를 되돌린다.
 * 실행 컨텍스트: 프로세스 컨텍스트. sysfs 경로에서는 호출자가
 * pci_lock_rescan_remove() 를 쥐고 있다.
 *
 * 호출 체인:
 *   sriov_disable() -> sriov_del_vfs() -> [pci_iov_remove_virtfn]
 *   sriov_add_vfs() 의 실패 되감기 -> [pci_iov_remove_virtfn]
 *     -> pci_get_domain_bus_and_slot(), pci_stop_and_remove_bus_device(),
 *        virtfn_remove_bus()
 */
void pci_iov_remove_virtfn(struct pci_dev *dev, int id)	/* [한국어] include/linux/pci.h 에 선언된 공개 함수 */
{
	char buf[VIRTFN_ID_LEN];	/* [한국어] "virtfn%u" 링크 이름을 만들 버퍼 */
	struct pci_dev *virtfn;	/* [한국어] 찾아낸 VF 의 pci_dev */

	virtfn = pci_get_domain_bus_and_slot(pci_domain_nr(dev->bus),	/* [한국어] 도메인(세그먼트) 번호 + 버스 + devfn 으로 pci_dev 를 조회한다. 성공하면 참조 카운트를 하나 올려서 준다 */
					     pci_iov_virtfn_bus(dev, id),	/* [한국어] VF 의 버스 번호를 offset/stride 로 계산 */
					     pci_iov_virtfn_devfn(dev, id));	/* [한국어] VF 의 devfn 도 같은 식으로 계산 */
	if (!virtfn)	/* [한국어] 이미 사라졌거나 애초에 만들어지지 않았다면 */
		return;	/* [한국어] 할 일이 없으므로 조용히 돌아간다(이중 해제 방지) */

	sprintf(buf, "virtfn%u", id);	/* [한국어] PF 디렉터리에 만들어 둔 링크 이름을 재구성 */
	sysfs_remove_link(&dev->dev.kobj, buf);	/* [한국어] PF 쪽 virtfn<id> 링크를 제거 */
	/*
	 * pci_stop_dev() could have been called for this virtfn already,
	 * so the directory for the virtfn may have been removed before.
	 * Double check to avoid spurious sysfs warnings.
	 */
	if (virtfn->dev.kobj.sd)	/* [한국어] kobject 의 sysfs 디렉터리 항목(sd)이 아직 살아 있는지 확인 — 위 영어 주석대로 이미 pci_stop_dev 로 디렉터리가 사라졌을 수 있고, 그때 링크를 지우려 하면 헛된 경고가 뜬다 */
		sysfs_remove_link(&virtfn->dev.kobj, "physfn");	/* [한국어] VF 쪽 physfn 링크를 제거 */

	pci_stop_and_remove_bus_device(virtfn);	/* [한국어] 드라이버 remove 를 부르고 코어의 장치 목록에서 뺀 뒤 pci_dev 참조를 놓는다 */
	virtfn_remove_bus(dev->bus, virtfn->bus);	/* [한국어] VF 를 담으려고 만들었던 가상 버스가 비었으면 제거 */

	/* balance pci_get_domain_bus_and_slot() */
	pci_dev_put(virtfn);	/* [한국어] 위 pci_get_domain_bus_and_slot 이 올린 참조를 되돌린다 */
	pci_dev_put(dev);	/* [한국어] pci_iov_scan_device 에서 virtfn->physfn = pci_dev_get(dev) 로 올렸던 PF 참조를 되돌린다 */
}

/*
 * [한국어]
 * sriov_totalvfs_show - PF 의 sysfs 파일 sriov_totalvfs 읽기 핸들러
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * 왜 필요한가: 관리자가 "이 장치에 VF 를 최대 몇 개까지 만들 수 있는가"를
 * 알아야 sriov_numvfs 에 쓸 값을 정할 수 있다. 값의 출처는 capability 의
 * TotalVFs(+0x0e)지만, PF 드라이버가 pci_sriov_set_totalvfs() 로 더 낮춰
 * 잡았을 수 있으므로 그 최종값(driver_max_VFs)을 돌려주는
 * pci_sriov_get_totalvfs() 를 쓴다.
 * 실행 컨텍스트: sysfs read. 단순 읽기라 락을 잡지 않는다 —
 * driver_max_VFs 는 드라이버 probe 시각에 정해지고 그 뒤 잘 바뀌지 않는다.
 *
 * 호출 체인:
 *   cat /sys/.../sriov_totalvfs -> [sriov_totalvfs_show] -> pci_sriov_get_totalvfs()
 */
static ssize_t sriov_totalvfs_show(struct device *dev,	/* [한국어] DEVICE_ATTR_RO 용 show 핸들러 */
				   struct device_attribute *attr,
				   char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] struct device 를 pci_dev 로 되돌린다 */

	return sysfs_emit(buf, "%u\n", pci_sriov_get_totalvfs(pdev));	/* [한국어] TotalVFs 가 아니라 드라이버가 낮춰 잡았을 수도 있는 driver_max_VFs 를 출력한다 */
}

/*
 * [한국어]
 * sriov_numvfs_show - 현재 켜져 있는 VF 개수를 출력한다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수.
 *
 * 왜 필요한가: sriov_numvfs 는 읽고 쓸 수 있는 파일(DEVICE_ATTR_RW)이라
 * 쓰기 핸들러가 도는 중에도 읽기가 들어올 수 있다. 그 사이 num_VFs 는
 * "일부만 만들어진" 중간 상태일 수 있으므로, 원문 영어 주석대로
 * device_lock 으로 쓰기와 직렬화해 독자가 항상 확정된 값을 보게 한다.
 * 실행 컨텍스트: sysfs read. device_lock 은 뮤텍스라 잠들 수 있다.
 *
 * 호출 체인:
 *   cat /sys/.../sriov_numvfs -> [sriov_numvfs_show]
 */
static ssize_t sriov_numvfs_show(struct device *dev,	/* [한국어] DEVICE_ATTR_RW 의 읽기 쪽 */
				 struct device_attribute *attr,
				 char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] pci_dev 로 되돌린다 */
	u16 num_vfs;	/* [한국어] 락 아래에서 읽은 값을 담을 지역 변수 — 락 밖에서 포맷하기 위해 복사한다 */

	/* Serialize vs sriov_numvfs_store() so readers see valid num_VFs */
	device_lock(&pdev->dev);	/* [한국어] sriov_numvfs_store 와 같은 락을 잡아 VF 생성/제거 도중의 중간값이 보이지 않게 한다 */
	num_vfs = pdev->sriov->num_VFs;	/* [한국어] 확정된 VF 개수를 지역 변수로 복사 */
	device_unlock(&pdev->dev);	/* [한국어] 락은 최대한 짧게 — 포맷은 락 밖에서 한다 */

	return sysfs_emit(buf, "%u\n", num_vfs);	/* [한국어] 복사해 둔 값을 십진수로 출력 */
}

/*
 * num_vfs > 0; number of VFs to enable
 * num_vfs = 0; disable all VFs
 *
 * Note: SRIOV spec does not allow partial VF
 *	 disable, so it's all or none.
 */
/*
 * [한국어]
 * sriov_numvfs_store - sysfs 로 VF 개수를 지정해 SR-IOV 를 켜거나 끈다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 사용자가 쓴 문자열. 켤 VF 개수(0 이면 전부 끄기).
 * @count: buf 의 길이. 성공 시 그대로 반환한다.
 * @return: 성공 시 count, 실패 시 음수
 *          (-EINVAL 형식 오류, -ERANGE 상한 초과, -ENOENT 드라이버/콜백 없음,
 *           -EBUSY 이미 VF 가 켜져 있음, 그 밖에는 드라이버 콜백의 오류).
 *
 * 왜 필요한가: 이것이 SR-IOV 를 켜는 사실상 유일한 사용자 인터페이스다.
 * 위 영어 주석이 밝히듯 SR-IOV 스펙은 "일부 VF 만 끄기"를 허용하지 않는다.
 * 전부 켜거나 전부 끄거나 둘 중 하나이므로, 개수를 바꾸려면 0 을 써서
 * 먼저 다 끈 뒤 새 값을 써야 한다. 이 함수는 그 규칙을 강제한다.
 *
 * 이 함수가 직접 하드웨어를 건드리지 않는 것이 핵심이다. 실제 활성화는
 * PF 드라이버가 등록한 .sriov_configure 함수 포인터로 넘어간다. 드라이버가
 * VF 를 켜기 전에 펌웨어 설정이나 자원 재배분을 해야 할 수 있기 때문이다.
 * 특별한 준비가 필요 없는 드라이버는 이 파일의 pci_sriov_configure_simple()
 * 을 그대로 등록하면 되고, drivers/nvme/host/pci.c 의 nvme_driver 가 바로
 * 그렇게 한다(.sriov_configure = pci_sriov_configure_simple, grep 으로 확인).
 *
 * 락: device_lock(PF) 으로 드라이버 언바인드와 다른 sysfs 쓰기를 막고,
 * 그 안에서 pci_lock_rescan_remove() 를 추가로 잡아 VF pci_dev 의
 * 추가/제거가 다른 열거 작업과 겹치지 않게 한다. 락 순서는 항상
 * device_lock -> rescan_remove 다.
 * 실행 컨텍스트: sysfs write. 프로세스 컨텍스트, 안에서 msleep 으로 잠든다.
 *
 * 호출 체인:
 *   echo N > /sys/.../sriov_numvfs -> [sriov_numvfs_store]
 *     -> pdev->driver->sriov_configure() (= NVMe 의 경우 pci_sriov_configure_simple)
 *     -> sriov_enable() 또는 sriov_disable()
 */
static ssize_t sriov_numvfs_store(struct device *dev,	/* [한국어] DEVICE_ATTR_RW 의 쓰기 쪽 */
				  struct device_attribute *attr,
				  const char *buf, size_t count)	/* [한국어] 사용자가 쓴 바이트열과 길이 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PF 의 pci_dev */
	int ret = 0;	/* [한국어] 드라이버 콜백의 반환값을 담는다. 성공 시 "켠 VF 수"가 들어온다 */
	u16 num_vfs;	/* [한국어] 요청받은 VF 개수. VF 수는 16비트 범위(TotalVFs 도 16비트)라 u16 */

	if (kstrtou16(buf, 0, &num_vfs) < 0)	/* [한국어] 문자열을 16비트 부호 없는 정수로 변환. base 0 이라 0x 표기도 받는다 */
		return -EINVAL;	/* [한국어] 숫자가 아니거나 65535 를 넘으면 형식 오류 */

	if (num_vfs > pci_sriov_get_totalvfs(pdev))	/* [한국어] TotalVFs(혹은 드라이버가 낮춘 값)를 넘는 요청이면 */
		return -ERANGE;	/* [한국어] 범위 초과를 뜻하는 -ERANGE 로 거절 */

	device_lock(&pdev->dev);	/* [한국어] 이 뒤로는 PF 의 드라이버 포인터와 sriov->num_VFs 를 만지므로 락이 필요하다 */

	if (num_vfs == pdev->sriov->num_VFs)	/* [한국어] 이미 요청한 개수만큼 켜져 있으면 할 일이 없다 */
		goto exit;	/* [한국어] ret 가 0 이므로 성공(count 반환)으로 빠진다 */

	/* is PF driver loaded */
	if (!pdev->driver) {	/* [한국어] PF 에 드라이버가 붙어 있지 않으면 sriov_configure 를 부를 대상이 없다 */
		pci_info(pdev, "no driver bound to device; cannot configure SR-IOV\n");	/* [한국어] 사용자에게 원인을 알리는 커널 로그 */
		ret = -ENOENT;	/* [한국어] "해당 객체 없음" 으로 거절 */
		goto exit;	/* [한국어] 락을 풀어야 하므로 exit 로 간다 */
	}

	/* is PF driver loaded w/callback */
	if (!pdev->driver->sriov_configure) {	/* [한국어] 드라이버는 있지만 SR-IOV 설정 콜백을 등록하지 않은 경우 */
		pci_info(pdev, "driver does not support SR-IOV configuration via sysfs\n");	/* [한국어] sysfs 로는 설정할 수 없다고 알린다 */
		ret = -ENOENT;	/* [한국어] 같은 -ENOENT 로 거절 */
		goto exit;	/* [한국어] 락 해제 지점으로 */
	}

	if (num_vfs == 0) {	/* [한국어] 0 을 썼다면 전부 끄라는 뜻이다 */
		/* disable VFs */
		pci_lock_rescan_remove();	/* [한국어] VF pci_dev 들이 제거되므로 열거 경로와 배타적으로 실행해야 한다 */
		ret = pdev->driver->sriov_configure(pdev, 0);	/* [한국어] 드라이버 콜백에 0 을 넘긴다. NVMe 는 pci_sriov_configure_simple 이 받아 sriov_disable 을 부른다 */
		pci_unlock_rescan_remove();	/* [한국어] 제거가 끝났으니 락 해제 */
		goto exit;	/* [한국어] ret 는 보통 0 이며 성공 처리된다 */
	}

	/* enable VFs */
	if (pdev->sriov->num_VFs) {	/* [한국어] 켜라는 요청인데 이미 VF 가 켜져 있으면 */
		pci_warn(pdev, "%d VFs already enabled. Disable before enabling %d VFs\n",	/* [한국어] 스펙상 부분 변경이 불가능하므로 먼저 끄라고 안내한다 */
			 pdev->sriov->num_VFs, num_vfs);	/* [한국어] 현재 개수와 요청 개수를 함께 보여 준다 */
		ret = -EBUSY;	/* [한국어] 자원이 사용 중이라는 뜻의 -EBUSY */
		goto exit;	/* [한국어] 락 해제 지점으로 */
	}

	pci_lock_rescan_remove();	/* [한국어] VF pci_dev 들이 새로 생기므로 열거 경로를 막는다 */
	ret = pdev->driver->sriov_configure(pdev, num_vfs);	/* [한국어] 드라이버 콜백에 켤 개수를 넘긴다. 성공 시 실제로 켠 VF 수를 돌려준다 */
	pci_unlock_rescan_remove();	/* [한국어] 생성이 끝났으니 락 해제 */
	if (ret < 0)	/* [한국어] 음수면 실패 */
		goto exit;	/* [한국어] 그 오류를 그대로 사용자에게 전달한다 */

	if (ret != num_vfs)	/* [한국어] 요청한 수와 실제로 켠 수가 다르면(드라이버가 자원 사정으로 줄인 경우) */
		pci_warn(pdev, "%d VFs requested; only %d enabled\n",	/* [한국어] 조용히 넘어가지 않고 경고를 남긴다 */
			 num_vfs, ret);	/* [한국어] 요청 수와 실제 수를 함께 보여 준다 */

exit:	/* [한국어] 모든 경로가 합류하는 락 해제 지점 */
	device_unlock(&pdev->dev);	/* [한국어] device_lock 해제 */

	if (ret < 0)	/* [한국어] 음수 오류면 */
		return ret;	/* [한국어] 그대로 반환해 write(2) 가 실패하게 한다 */

	return count;	/* [한국어] 성공이면 "쓴 바이트를 전부 소비했다"는 뜻으로 count 를 반환한다 */
}

/*
 * [한국어]
 * sriov_offset_show - First VF Offset 값을 사용자에게 보여 준다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 왜 필요한가: VF 의 RID 계산식(PF RID + offset + stride*id)에서 offset 이
 * 무엇인지 사용자 공간이 알 수 있게 노출한다. 관리 도구가 "이 PF 의 VF 는
 * 어느 BDF 에 나타날 것인가"를 미리 계산할 때 쓴다. 값은 capability 의
 * PCI_SRIOV_VF_OFFSET(+0x14) 사본이며 NumVFs 를 바꿀 때마다 갱신된다.
 * 실행 컨텍스트: sysfs read. 단순 16비트 읽기라 락 없음.
 *
 * 호출 체인:
 *   cat /sys/.../sriov_offset -> [sriov_offset_show]
 */
static ssize_t sriov_offset_show(struct device *dev,	/* [한국어] 읽기 전용 show 핸들러 */
				 struct device_attribute *attr,
				 char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] pci_dev 로 되돌린다 */

	return sysfs_emit(buf, "%u\n", pdev->sriov->offset);	/* [한국어] pci_iov_set_numvfs 가 갱신해 둔 First VF Offset 사본을 십진수로 출력 */
}

/*
 * [한국어]
 * sriov_stride_show - VF Stride 값을 사용자에게 보여 준다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 왜 필요한가: offset 과 짝을 이루는 값이다. 이웃한 VF 의 RID 가 몇 칸씩
 * 떨어져 있는지를 알려 준다. stride 가 1 이면 VF 들이 devfn 을 연속으로
 * 쓰고, 2 이상이면 사이사이를 건너뛴다. 값은 PCI_SRIOV_VF_STRIDE(+0x16) 사본.
 * 실행 컨텍스트: sysfs read. 락 없음.
 *
 * 호출 체인:
 *   cat /sys/.../sriov_stride -> [sriov_stride_show]
 */
static ssize_t sriov_stride_show(struct device *dev,	/* [한국어] 읽기 전용 show 핸들러 */
				 struct device_attribute *attr,
				 char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] pci_dev 로 되돌린다 */

	return sysfs_emit(buf, "%u\n", pdev->sriov->stride);	/* [한국어] VF Stride 사본을 십진수로 출력 */
}

/*
 * [한국어]
 * sriov_vf_device_show - VF 들이 갖게 될 Device ID 를 보여 준다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 왜 필요한가: VF 의 Vendor ID 는 PF 와 같지만 Device ID 는 다르다. 그 값이
 * capability 의 VF Device ID(+0x1a)에 있고 sriov_init() 이 읽어 두었다.
 * 관리 도구가 "이 PF 를 켜면 어떤 ID 의 장치가 나타나는가"를 미리 알 수 있게
 * 노출한다. 십육진수로 찍는 이유는 PCI ID 관례가 16진 표기이기 때문이다.
 * 실행 컨텍스트: sysfs read. 락 없음(초기화 후 불변).
 *
 * 호출 체인:
 *   cat /sys/.../sriov_vf_device -> [sriov_vf_device_show]
 */
static ssize_t sriov_vf_device_show(struct device *dev,	/* [한국어] 읽기 전용 show 핸들러 */
				    struct device_attribute *attr,
				    char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] pci_dev 로 되돌린다 */

	return sysfs_emit(buf, "%x\n", pdev->sriov->vf_device);	/* [한국어] PCI ID 관례에 맞춰 십육진수로 출력한다 */
}

/*
 * [한국어]
 * sriov_drivers_autoprobe_show - VF 에 드라이버를 자동으로 붙일지 여부를 보여 준다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 왜 필요한가: VF 를 게스트에 넘기려는 경우(vfio) 호스트 드라이버가 먼저
 * VF 를 잡아 버리면 곤란하다. 그래서 "VF 를 만들되 드라이버는 붙이지 마라"는
 * 스위치가 필요하고, 그 상태를 읽는 것이 이 파일이다. 실제 판정은
 * drivers/pci/pci-driver.c 의 pci_device_can_probe() 가
 * pdev->physfn->sriov->drivers_autoprobe 를 보고 한다(grep 으로 확인).
 * 실행 컨텍스트: sysfs read. bool 한 개 읽기라 락 없음.
 *
 * 호출 체인:
 *   cat /sys/.../sriov_drivers_autoprobe -> [sriov_drivers_autoprobe_show]
 */
static ssize_t sriov_drivers_autoprobe_show(struct device *dev,	/* [한국어] DEVICE_ATTR_RW 의 읽기 쪽 */
					    struct device_attribute *attr,
					    char *buf)	/* [한국어] 출력 버퍼 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] pci_dev 로 되돌린다 */

	return sysfs_emit(buf, "%u\n", pdev->sriov->drivers_autoprobe);	/* [한국어] true 면 1, false 면 0 으로 찍힌다 */
}

/*
 * [한국어]
 * sriov_drivers_autoprobe_store - VF 자동 드라이버 바인딩을 켜고 끈다
 *
 * @dev: PF 의 struct device.
 * @attr: 쓰지 않는다.
 * @buf: "0"/"1"/"y"/"n" 등 kstrtobool 이 이해하는 문자열.
 * @count: 쓴 길이. 성공 시 그대로 반환.
 * @return: 성공 시 count, 형식 오류면 -EINVAL.
 *
 * 왜 필요한가: VF 를 만들기 "전에" 이 값을 0 으로 해 두면, 그 뒤 sriov_numvfs
 * 로 만든 VF 들에 호스트 드라이버가 붙지 않는다. 그래야 그대로 vfio 에
 * 바인딩해 게스트에 넘길 수 있다. 이미 만들어진 VF 에는 소급 적용되지
 * 않는다 — 판정이 probe 시각에 한 번 일어나기 때문이다.
 * 실행 컨텍스트: sysfs write. 단일 bool 대입이라 락을 잡지 않는다.
 * 이 값을 읽는 pci_device_can_probe() 도 락 없이 읽으며, 잘못 읽어도
 * "드라이버가 붙느냐 마느냐"의 정책 판단일 뿐 자료구조를 깨지 않는다.
 *
 * 호출 체인:
 *   echo 0 > /sys/.../sriov_drivers_autoprobe -> [sriov_drivers_autoprobe_store]
 */
static ssize_t sriov_drivers_autoprobe_store(struct device *dev,	/* [한국어] DEVICE_ATTR_RW 의 쓰기 쪽 */
					     struct device_attribute *attr,
					     const char *buf, size_t count)	/* [한국어] 사용자가 쓴 문자열과 길이 */
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PF 의 pci_dev */
	bool drivers_autoprobe;	/* [한국어] 파싱 결과를 담을 지역 변수 */

	if (kstrtobool(buf, &drivers_autoprobe) < 0)	/* [한국어] "1", "y", "on" 등을 true 로, "0", "n", "off" 를 false 로 해석한다 */
		return -EINVAL;	/* [한국어] 해석할 수 없는 문자열이면 형식 오류 */

	pdev->sriov->drivers_autoprobe = drivers_autoprobe;	/* [한국어] PF 의 sriov 구조체에 정책을 저장한다. 다음에 만들어질 VF 들의 probe 여부가 여기서 갈린다 */

	return count;	/* [한국어] 전부 소비했다는 뜻으로 count 반환 */
}

/*
 * [한국어]
 * 아래 여섯 줄은 위에서 정의한 show/store 함수를 struct device_attribute 로
 * 포장해 dev_attr_<이름> 이라는 전역 변수를 만든다. _RO 는 0444(읽기 전용),
 * _RW 는 0644(읽기/쓰기) 퍼미션을 뜻하며, 매크로가 <이름>_show 와
 * <이름>_store 함수를 이름 규칙으로 찾아 연결한다. 이 변수들이 바로 아래
 * sriov_pf_dev_attrs[] 배열에 실려 PF 의 sysfs 디렉터리에 파일로 나타난다.
 */
static DEVICE_ATTR_RO(sriov_totalvfs);	/* [한국어] sriov_totalvfs: 최대 VF 수 — 읽기 전용 */
static DEVICE_ATTR_RW(sriov_numvfs);	/* [한국어] sriov_numvfs: 현재/설정할 VF 수 — 읽고 쓸 수 있다. SR-IOV 를 켜는 입구 */
static DEVICE_ATTR_RO(sriov_offset);	/* [한국어] sriov_offset: First VF Offset — 읽기 전용 */
static DEVICE_ATTR_RO(sriov_stride);	/* [한국어] sriov_stride: VF Stride — 읽기 전용 */
static DEVICE_ATTR_RO(sriov_vf_device);	/* [한국어] sriov_vf_device: VF Device ID — 읽기 전용 */
static DEVICE_ATTR_RW(sriov_drivers_autoprobe);	/* [한국어] sriov_drivers_autoprobe: VF 자동 바인딩 정책 — 읽고 쓸 수 있다 */

/*
 * [한국어]
 * sriov_pf_dev_attrs - PF 의 sysfs 디렉터리에 만들 속성 목록
 *
 * 역할: NULL 로 끝나는 attribute 포인터 배열. 아래 attribute_group 이 참조한다.
 * 설정자: 컴파일 시각 고정.
 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성/제거 경로.
 * 값 범위: 위에서 DEVICE_ATTR_* 로 만든 dev_attr_* 들의 .attr 멤버.
 * 동기화: 정적 데이터라 불필요.
 */
static struct attribute *sriov_pf_dev_attrs[] = {	/* [한국어] static — 외부에는 아래 group 만 공개한다 */
	&dev_attr_sriov_totalvfs.attr,	/* [한국어] 최대 VF 수를 보여 주는 파일 */
	&dev_attr_sriov_numvfs.attr,	/* [한국어] VF 수를 읽고 쓰는 파일 — SR-IOV 활성화의 입구 */
	&dev_attr_sriov_offset.attr,	/* [한국어] First VF Offset 을 보여 주는 파일 */
	&dev_attr_sriov_stride.attr,	/* [한국어] VF Stride 를 보여 주는 파일 */
	&dev_attr_sriov_vf_device.attr,	/* [한국어] VF Device ID 를 보여 주는 파일 */
	&dev_attr_sriov_drivers_autoprobe.attr,	/* [한국어] VF 자동 바인딩 정책 파일 */
#ifdef CONFIG_PCI_MSI	/* [한국어] MSI-X 가 켜진 커널에서만 존재하는 항목 */
	&dev_attr_sriov_vf_total_msix.attr,	/* [한국어] VF 들이 나눠 쓸 MSI-X 벡터 총수 파일(PF 쪽에 붙는다) */
#endif
	NULL,	/* [한국어] 배열 끝 표지 */
};

/*
 * [한국어]
 * sriov_pf_attrs_are_visible - PF 용 sysfs 파일을 만들지 결정한다
 *
 * @kobj: 검사 대상 장치의 kobject.
 * @a: 판단할 attribute.
 * @n: 그룹 안 인덱스. 쓰지 않는다.
 * @return: 0 이면 파일을 만들지 않는다. 아니면 그 값이 퍼미션이 된다.
 *
 * 왜 필요한가: 이 그룹도 모든 PCI 장치에 대해 평가되므로, SR-IOV capability 가
 * 없는 장치에는 sriov_* 파일을 만들면 안 된다. dev_is_pf() 는 pci_dev 의
 * is_physfn 플래그를 보는 매크로이고, 그 플래그는 sriov_init() 이 성공했을
 * 때만 1 이 된다. 즉 "SR-IOV capability 를 실제로 초기화한 장치"만 통과한다.
 * 실행 컨텍스트: 장치 등록 시각. 락 없음.
 *
 * 호출 체인:
 *   pci_device_add() -> device_add() -> [sriov_pf_attrs_are_visible]
 */
static umode_t sriov_pf_attrs_are_visible(struct kobject *kobj,	/* [한국어] attribute_group 의 is_visible 콜백 */
					  struct attribute *a, int n)	/* [한국어] 판단할 attribute 와 인덱스 */
{
	struct device *dev = kobj_to_dev(kobj);	/* [한국어] kobject 에서 struct device 로 되돌린다 */

	if (!dev_is_pf(dev))	/* [한국어] dev_is_pf 는 PCI 장치이면서 is_physfn 이 1 인지 검사하는 매크로다. sriov_init 이 성공해야 그 플래그가 선다 */
		return 0;	/* [한국어] PF 가 아니면 파일을 만들지 않는다 */

	return a->mode;	/* [한국어] PF 이면 선언된 퍼미션(RO 는 0444, RW 는 0644)을 그대로 쓴다 */
}

/*
 * [한국어]
 * sriov_pf_dev_attr_group - PF 쪽 sysfs 속성 그룹
 *
 * 역할: drivers/pci/pci-sysfs.c 의 pci_dev_attr_groups 에 등록되어 모든 PCI
 * 장치에 대해 평가되고, is_visible 이 통과시킨 PF 에만 sriov_* 파일을 만든다.
 * 설정자: 컴파일 시각 상수.
 * 읽는 자: 드라이버 코어(device_add / device_del).
 * 값 범위: attrs 는 위 배열, is_visible 은 위 콜백.
 * 동기화: 정적 상수라 불필요.
 */
const struct attribute_group sriov_pf_dev_attr_group = {	/* [한국어] const + 비static — pci-sysfs.c 가 참조한다 */
	.attrs = sriov_pf_dev_attrs,	/* [한국어] 만들 파일 목록 */
	.is_visible = sriov_pf_attrs_are_visible,	/* [한국어] PF 인지 거르는 콜백 */
};

/*
 * [한국어]
 * pcibios_sriov_enable - VF 를 켜기 직전에 아키텍처가 끼어들 수 있는 훅(기본 구현)
 *
 * @pdev: PF 의 pci_dev.
 * @num_vfs: 켜려는 VF 개수.
 * @return: 0 이면 계속 진행. 음수면 sriov_enable() 이 전체를 되감는다.
 *
 * 왜 필요한가: 어떤 플랫폼은 VF 를 켜기 전에 IOMMU 테이블이나 PE(Partitionable
 * Endpoint) 같은 플랫폼 자원을 미리 잡아야 한다. 그런 일은 아키텍처 코드만
 * 할 수 있으므로 __weak 심볼로 훅을 열어 둔다. 아무 것도 할 필요가 없는
 * 아키텍처에서는 이 기본 구현이 그대로 링크되어 0 만 돌려준다.
 * __weak 의 의미: 같은 이름의 강한(strong) 심볼이 커널 어딘가에 있으면
 * 링커가 그쪽을 쓰고, 없으면 이 약한 정의가 쓰인다. 실제로 powerpc 가
 * arch/powerpc/kernel/pci-common.c 에서 이 이름을 재정의한다.
 * 실행 컨텍스트: sriov_enable() 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sriov_enable() -> [pcibios_sriov_enable]
 */
int __weak pcibios_sriov_enable(struct pci_dev *pdev, u16 num_vfs)	/* [한국어] __weak — 아키텍처가 같은 이름으로 재정의하면 그쪽이 쓰인다 */
{
	return 0;	/* [한국어] 기본 동작은 "특별히 할 일 없음" */
}

/*
 * [한국어]
 * pcibios_sriov_disable - VF 를 끈 뒤 아키텍처가 정리할 기회를 주는 훅(기본 구현)
 *
 * @pdev: PF 의 pci_dev.
 * @return: 0. 호출자는 이 반환값을 검사하지 않는다(끄는 경로에서는 되돌릴 것이 없다).
 *
 * 왜 필요한가: pcibios_sriov_enable() 의 짝이다. 플랫폼이 VF 를 위해 잡아 둔
 * 자원을 되돌린다. 기본 구현은 아무 것도 하지 않는다.
 * 실행 컨텍스트: sriov_disable() 과 sriov_enable() 의 실패 되감기 경로.
 *
 * 호출 체인:
 *   sriov_disable(), sriov_enable() 실패 경로 -> [pcibios_sriov_disable]
 */
int __weak pcibios_sriov_disable(struct pci_dev *pdev)	/* [한국어] __weak — 아키텍처가 필요하면 재정의한다 */
{
	return 0;	/* [한국어] 기본 동작은 "정리할 것 없음" */
}

/*
 * [한국어]
 * sriov_add_vfs - 하드웨어가 만들어 준 VF 들에 대응하는 pci_dev 를 차례로 등록한다
 *
 * @dev: PF 의 pci_dev.
 * @num_vfs: 등록할 VF 개수.
 * @return: 0 성공, 음수면 pci_iov_add_virtfn() 의 오류.
 *
 * 왜 필요한가: sriov_enable() 이 VF Enable 비트를 세우면 하드웨어에는 VF 가
 * 생기지만 커널은 아직 모른다. 이 함수가 0..num_vfs-1 을 돌며 각 VF 의
 * pci_dev 를 만들어 등록한다.
 * no_vf_scan 예외: 플랫폼(커널 본체에서는 s390 PCI 백엔드)이 VF 를 자기
 * 방식으로 등록하는 경우 이 플래그를 세우고, 그러면 여기서는 아무 것도 하지
 * 않고 성공으로 돌아간다. 그런 플랫폼은 config space 를 직접 스캔하는 대신
 * 펌웨어가 알려 주는 목록으로 장치를 만든다.
 * 실행 컨텍스트: sriov_enable() 안. 프로세스 컨텍스트이며 sysfs 경로에서는
 * pci_lock_rescan_remove() 를 쥔 상태다.
 * 에러 경로: 중간에 실패하면 이미 만든 VF 를 역순으로 지워 "절반만 만들어진"
 * 상태를 남기지 않는다.
 *
 * 호출 체인:
 *   sriov_enable() -> [sriov_add_vfs] -> pci_iov_add_virtfn()
 */
static int sriov_add_vfs(struct pci_dev *dev, u16 num_vfs)	/* [한국어] static — sriov_enable 전용 헬퍼 */
{
	unsigned int i;	/* [한국어] VF 번호 루프 변수. 아래 while (i--) 되감기를 위해 부호 없는 정수를 쓴다 */
	int rc;	/* [한국어] pci_iov_add_virtfn 의 반환값 */

	if (dev->no_vf_scan)	/* [한국어] 플랫폼이 VF pci_dev 생성을 스스로 담당하는 경우(커널 본체에서는 s390) */
		return 0;	/* [한국어] 커널 공통 코드는 손대지 않고 성공으로 돌아간다 */

	for (i = 0; i < num_vfs; i++) {	/* [한국어] VF 0 번부터 num_vfs-1 번까지 */
		rc = pci_iov_add_virtfn(dev, i);	/* [한국어] VF 하나를 만들어 코어에 등록한다 */
		if (rc)	/* [한국어] 어느 하나라도 실패하면 */
			goto failed;	/* [한국어] 이미 만든 것들을 지우러 간다 */
	}
	return 0;	/* [한국어] 전부 등록 성공 */
failed:	/* [한국어] 부분 실패 되감기 지점 */
	while (i--)	/* [한국어] i 는 실패한 인덱스이므로 먼저 감소시켜 "성공한 마지막 VF" 부터 역순으로 돈다. i 가 0 이 되면 루프가 끝난다 */
		pci_iov_remove_virtfn(dev, i);	/* [한국어] 만들었던 VF 를 하나씩 제거 */

	return rc;	/* [한국어] 처음 실패한 원인을 그대로 전달한다 */
}

/*
 * [한국어]
 * sriov_enable - SR-IOV 를 실제로 켜는 핵심 함수
 *
 * @dev: PF 의 pci_dev. is_physfn 이 1 이고 dev->sriov 가 채워져 있어야 한다.
 * @nr_virtfn: 켤 VF 개수. 0 이면 아무 것도 하지 않고 성공.
 * @return: 0 성공, 음수 오류
 *          (-EINVAL 이미 켜져 있거나 인자 범위 초과, -EIO 하드웨어가 모순된 값 보고,
 *           -ENOMEM MMIO/버스 번호 부족, -ENODEV/-ENOSYS 의존 PF 문제).
 *
 * 왜 필요한가: "NumVFs 를 쓰고 VF Enable 비트를 세운다"는 한 줄처럼 보이지만,
 * 그 전에 반드시 확인하고 준비해야 할 것이 많다. 이 함수의 대부분은 그
 * 사전 검증이다. 순서대로:
 *   (1) 이미 VF 가 켜져 있지 않은가 — 스펙상 부분 변경이 불가능하다.
 *   (2) InitialVFs 가 말이 되는가 — VF Migration 을 지원하지 않는 장치라면
 *       InitialVFs 는 반드시 TotalVFs 와 같아야 한다(스펙 요구).
 *   (3) 요청 개수가 상한 안인가 — VF Migration 이 없으면 InitialVFs 가 상한이다.
 *   (4) VF BAR 6개가 요청한 VF 수를 담을 만큼 MMIO 를 확보하고 있는가.
 *   (5) 마지막 VF 의 버스 번호가 이 버스에 예약된 범위(busn_res) 안인가.
 *   (6) PF 의 메모리/IO 디코딩이 켜져 있는가(pci_enable_resources).
 *   (7) Function Dependency Link 가 자기 자신이 아니면, 함께 켜야 하는 PF 를
 *       찾아 dep_link 심볼릭 링크를 만들어 사용자에게 알린다.
 *   (8) 아키텍처 훅(pcibios_sriov_enable)에 기회를 준다.
 * 그런 다음에야 NumVFs 를 쓰고 VFE|MSE 를 세우고 100ms 를 기다린 뒤
 * VF 들의 pci_dev 를 만든다.
 *
 * 왜 msleep(100) 인가: VF Enable 을 쓴 직후에는 VF 의 config space 가 아직
 * 응답하지 않을 수 있다. 그 사이 접근하면 잘못된 값을 읽는다. 그래서
 * pci_cfg_access_lock() 으로 다른 경로의 config 접근을 막아 둔 채 기다린다.
 * 되돌릴 때는 ssleep(1) 로 더 길게 기다린다(VF 가 사라지는 데 더 오래 걸린다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠들므로 인터럽트 문맥 불가.
 * 호출자가 device_lock(PF) 과 pci_lock_rescan_remove() 를 쥐고 있다
 * (sysfs 경로). PF 드라이버가 직접 pci_enable_sriov() 를 부르는 경우도 있다.
 * 에러 경로: err_pcibios 하나로 모아 VFE|MSE 를 지우고, 아키텍처 훅을 되돌리고,
 * dep_link 를 지우고, NumVFs 를 0 으로 돌린다.
 *
 * 호출 체인:
 *   sriov_numvfs_store() -> driver->sriov_configure()
 *     -> pci_sriov_configure_simple() 또는 pci_enable_sriov() -> [sriov_enable]
 *     -> pci_enable_resources(), pcibios_sriov_enable(), pci_iov_set_numvfs(),
 *        sriov_add_vfs()
 */
static int sriov_enable(struct pci_dev *dev, int nr_virtfn)	/* [한국어] static — 이 파일의 pci_enable_sriov 와 pci_sriov_configure_simple 만 부른다 */
{
	int rc;	/* [한국어] 중간 함수들의 반환값 */
	int i;	/* [한국어] VF BAR 루프 변수 */
	int nres;	/* [한국어] "실제로 배정된 VF BAR 개수" 를 세는 변수 */
	u16 initial;	/* [한국어] InitialVFs 레지스터 값을 담는다 */
	struct resource *res;	/* [한국어] VF BAR 영역을 가리킬 포인터 */
	struct pci_dev *pdev;	/* [한국어] Function Dependency Link 가 가리키는 다른 PF */
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태 구조체 */
	int bars = 0;	/* [한국어] pci_enable_resources 에 넘길 BAR 비트마스크. resource 인덱스마다 한 비트 */
	int bus;	/* [한국어] 마지막 VF 의 버스 번호 */

	if (!nr_virtfn)	/* [한국어] 0 개를 켜라는 것은 아무 것도 하지 말라는 뜻 */
		return 0;	/* [한국어] 성공으로 돌아간다 */

	if (iov->num_VFs)	/* [한국어] 이미 VF 가 켜져 있으면 개수를 바꿀 수 없다(스펙상 전부 끄고 다시 켜야 한다) */
		return -EINVAL;	/* [한국어] 잘못된 요청 */

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_INITIAL_VF, &initial);	/* [한국어] capability + 0x0c(InitialVFs)를 읽는다 — 부팅 직후 쓸 수 있는 VF 수 */
	if (initial > iov->total_VFs ||	/* [한국어] InitialVFs 가 TotalVFs 보다 크면 모순이고 */
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (initial != iov->total_VFs)))	/* [한국어] VF Migration(PCI_SRIOV_CAP_VFM, cap bit0)을 지원하지 않는 장치라면 스펙상 InitialVFs 는 TotalVFs 와 같아야 한다 */
		return -EIO;	/* [한국어] 하드웨어가 스펙에 맞지 않는 값을 보고했으므로 입출력 오류 */

	if (nr_virtfn < 0 || nr_virtfn > iov->total_VFs ||	/* [한국어] 음수이거나 TotalVFs 를 넘으면 안 되고 */
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (nr_virtfn > initial)))	/* [한국어] VF Migration 이 없으면 InitialVFs 가 실제 상한이다(그 이상은 마이그레이션으로만 늘릴 수 있다) */
		return -EINVAL;	/* [한국어] 요청 자체가 잘못되었다 */

	nres = 0;	/* [한국어] 실제로 배정된 VF BAR 개수를 셀 준비 */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {	/* [한국어] VF BAR 6개를 모두 확인한다 */
		int idx = pci_resource_num_from_vf_bar(i);	/* [한국어] VF BAR 번호를 PF 의 resource[] 인덱스로 변환 */
		resource_size_t vf_bar_sz = pci_iov_resource_size(dev, idx);	/* [한국어] VF 하나가 쓰는 BAR 크기(barsz[i]) */

		bars |= (1 << idx);	/* [한국어] 이 resource 인덱스에 해당하는 비트를 마스크에 세운다 — 아래 pci_enable_resources 가 이 BAR 들의 디코딩을 켠다 */
		res = &dev->resource[idx];	/* [한국어] PF 가 잡아 둔 VF BAR 전체 영역 */
		if (vf_bar_sz * nr_virtfn > resource_size(res))	/* [한국어] 요청한 VF 수 x VF BAR 크기가 확보된 영역보다 크면 그 BAR 로는 이 개수를 감당할 수 없다 */
			continue;	/* [한국어] 세지 않고 넘어간다 — 그러면 아래에서 nres 가 모자라 오류가 난다 */
		if (res->parent)	/* [한국어] parent 가 있다는 것은 그 영역이 실제로 상위 리소스에 배정되었다는 뜻 */
			nres++;	/* [한국어] 쓸 수 있는 VF BAR 로 센다 */
	}
	if (nres != iov->nres) {	/* [한국어] sriov_init 이 열거 때 센 개수(iov->nres)와 다르면, 그 사이 리소스가 회수되었거나 요청 VF 수가 너무 많다는 뜻 */
		pci_err(dev, "not enough MMIO resources for SR-IOV\n");	/* [한국어] MMIO 가 모자란다는 것을 관리자에게 알린다 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 거절 */
	}

	bus = pci_iov_virtfn_bus(dev, nr_virtfn - 1);	/* [한국어] 요청한 개수에서 가장 마지막 VF 가 앉을 버스 번호를 계산 */
	if (bus > dev->bus->busn_res.end) {	/* [한국어] 이 버스에 예약된 버스 번호 구간(busn_res)의 끝을 넘어가면 그 VF 를 놓을 자리가 없다 */
		pci_err(dev, "can't enable %d VFs (bus %02x out of range of %pR)\n",	/* [한국어] 어떤 버스 번호가 어느 범위를 벗어났는지 함께 찍는다 */
			nr_virtfn, bus, &dev->bus->busn_res);	/* [한국어] %pR 은 struct resource 를 사람이 읽을 형태로 찍는 커널 포맷 지정자 */
		return -ENOMEM;	/* [한국어] 버스 번호 자원 부족도 -ENOMEM 으로 알린다 */
	}

	if (pci_enable_resources(dev, bars)) {	/* [한국어] bars 마스크에 든 VF BAR 들을 쓸 수 있도록 PF 의 Command 레지스터에서 메모리/IO 디코딩을 켠다 */
		pci_err(dev, "SR-IOV: IOV BARS not allocated\n");	/* [한국어] 리소스가 배정되지 않았다면 VF BAR 가 응답할 수 없다 */
		return -ENOMEM;	/* [한국어] 자원 부족으로 거절 */
	}

	if (iov->link != dev->devfn) {	/* [한국어] Function Dependency Link(+0x12)가 자기 devfn 과 다르면, 이 PF 는 다른 PF 와 함께 켜져야 하는 그룹에 속한다 */
		pdev = pci_get_slot(dev->bus, iov->link);	/* [한국어] 같은 버스에서 그 의존 대상 함수를 찾는다(참조 카운트를 올려서 준다) */
		if (!pdev)	/* [한국어] 그 함수가 없다면 */
			return -ENODEV;	/* [한국어] 장치 구성이 잘못된 것이므로 -ENODEV */

		if (!pdev->is_physfn) {	/* [한국어] 찾긴 했는데 그것이 PF 가 아니면 */
			pci_dev_put(pdev);	/* [한국어] 올린 참조를 먼저 되돌리고 */
			return -ENOSYS;	/* [한국어] 커널이 다룰 수 없는 구성이므로 -ENOSYS */
		}

		rc = sysfs_create_link(&dev->dev.kobj,	/* [한국어] 사용자 공간이 의존 관계를 볼 수 있도록 PF 디렉터리에 dep_link 링크를 만든다 */
					&pdev->dev.kobj, "dep_link");	/* [한국어] 링크가 가리키는 대상은 함께 켜져야 하는 그 PF 다 */
		pci_dev_put(pdev);	/* [한국어] 링크를 만들었으니 조회로 올린 참조는 되돌린다 */
		if (rc)	/* [한국어] 링크 생성 실패면 */
			return rc;	/* [한국어] 아직 하드웨어를 건드리지 않았으므로 그대로 반환한다 */
	}

	iov->initial_VFs = initial;	/* [한국어] 이번에 읽은 InitialVFs 를 사본에 보관 — 나중에 참조용 */
	if (nr_virtfn < initial)	/* [한국어] 요청 개수가 InitialVFs 보다 적으면 */
		initial = nr_virtfn;	/* [한국어] 실제로 pci_dev 를 만들 개수는 요청 개수다. 즉 initial 은 이제 "만들 VF 수"로 재사용된다 */

	rc = pcibios_sriov_enable(dev, initial);	/* [한국어] 아키텍처가 VF 를 위한 플랫폼 자원(IOMMU 테이블 등)을 준비할 기회를 준다 */
	if (rc) {	/* [한국어] 아키텍처 훅이 실패하면 */
		pci_err(dev, "failure %d from pcibios_sriov_enable()\n", rc);	/* [한국어] 어느 훅이 무슨 코드로 실패했는지 남긴다 */
		goto err_pcibios;	/* [한국어] 공통 되감기 경로로 간다(아직 VFE 는 세우지 않았지만 되감기 코드가 그 경우도 안전하게 처리한다) */
	}

	pci_iov_set_numvfs(dev, nr_virtfn);	/* [한국어] NumVFs 레지스터에 개수를 쓰고 그에 맞춰 offset/stride 사본을 갱신한다. 반드시 VF Enable 보다 먼저다 */
	iov->ctrl |= PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE;	/* [한국어] VF Enable(bit0)과 VF Memory Space Enable(bit3)을 사본에 세운다. VFE 는 VF 를 존재하게 하고, MSE 는 VF BAR 가 주소를 디코딩하게 한다 */
	pci_cfg_access_lock(dev);	/* [한국어] 이 시점부터 다른 경로의 config space 접근을 막는다 — VF 가 나타나는 과도 구간에 읽으면 쓰레기 값을 본다 */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);	/* [한국어] 실제 레지스터에 써서 하드웨어에 VF 를 만들게 한다. 이 한 줄이 SR-IOV 활성화의 핵심이다 */
	msleep(100);	/* [한국어] VF 의 config space 가 응답할 때까지 100ms 기다린다. 이 대기 없이 접근하면 VF 를 없는 장치로 오인한다 */
	pci_cfg_access_unlock(dev);	/* [한국어] config 접근 금지를 푼다 */

	rc = sriov_add_vfs(dev, initial);	/* [한국어] 이제 커널 쪽에 VF pci_dev 들을 만든다(no_vf_scan 플랫폼은 건너뛴다) */
	if (rc)	/* [한국어] VF 등록에 실패하면 */
		goto err_pcibios;	/* [한국어] 하드웨어 VF 도 다시 꺼야 하므로 되감기 경로로 간다 */

	kobject_uevent(&dev->dev.kobj, KOBJ_CHANGE);	/* [한국어] PF 의 속성이 바뀌었음을 udev 에 알린다 — virtfn 링크가 새로 생겼다 */
	iov->num_VFs = nr_virtfn;	/* [한국어] 성공을 확정한 뒤에야 사본을 갱신한다. sriov_numvfs_show 가 이 값을 읽는다 */

	return 0;	/* [한국어] SR-IOV 활성화 성공 */

err_pcibios:	/* [한국어] 아키텍처 훅 실패와 VF 등록 실패가 함께 오는 되감기 지점 */
	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);	/* [한국어] VF Enable 과 MSE 를 사본에서 지운다 */
	pci_cfg_access_lock(dev);	/* [한국어] 되돌리는 동안에도 config 접근을 막는다 */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);	/* [한국어] 실제 레지스터에 써서 VF 를 없앤다 */
	ssleep(1);	/* [한국어] VF 가 사라지는 데는 더 오래 걸리므로 1초를 기다린다 */
	pci_cfg_access_unlock(dev);	/* [한국어] config 접근 금지 해제 */

	pcibios_sriov_disable(dev);	/* [한국어] 아키텍처가 잡아 둔 플랫폼 자원을 되돌린다 */

	if (iov->link != dev->devfn)	/* [한국어] dep_link 를 만들었던 경우에만 */
		sysfs_remove_link(&dev->dev.kobj, "dep_link");	/* [한국어] 그 링크를 지운다 */

	pci_iov_set_numvfs(dev, 0);	/* [한국어] NumVFs 를 0 으로 되돌려 하드웨어 상태를 원점으로 만든다 */
	return rc;	/* [한국어] 처음 실패 원인을 그대로 전달한다 */
}

/*
 * [한국어]
 * sriov_del_vfs - 등록해 둔 VF pci_dev 를 전부 지운다
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: sriov_add_vfs() 의 역연산. sriov_disable() 이 하드웨어의
 * VF Enable 을 끄기 "전에" 반드시 먼저 불려야 한다. 순서가 뒤바뀌면
 * 이미 사라진 장치의 config space 에 접근하게 되어 잘못된 값을 읽거나
 * 플랫폼에 따라 머신 체크가 난다.
 * no_vf_scan 플랫폼에서는 애초에 VF pci_dev 를 만들지 않았으므로
 * pci_iov_remove_virtfn() 이 pci_get_domain_bus_and_slot 에서 NULL 을 받아
 * 조용히 돌아간다 — 따로 분기하지 않아도 안전하다.
 * 실행 컨텍스트: 프로세스 컨텍스트, rescan/remove 락 아래.
 *
 * 호출 체인:
 *   sriov_disable() -> [sriov_del_vfs] -> pci_iov_remove_virtfn()
 */
static void sriov_del_vfs(struct pci_dev *dev)	/* [한국어] static — sriov_disable 전용 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태 — num_VFs 를 읽기 위해 */
	int i;	/* [한국어] VF 번호 루프 변수 */

	for (i = 0; i < iov->num_VFs; i++)	/* [한국어] 켜 두었던 VF 개수만큼 0 번부터 돈다 */
		pci_iov_remove_virtfn(dev, i);	/* [한국어] VF 하나씩 코어에서 제거한다 */
}

/*
 * [한국어]
 * sriov_disable - SR-IOV 를 끈다. sriov_enable() 의 역연산
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음. 끄는 경로에는 실패가 없다(되돌릴 대상이 없으므로).
 *
 * 왜 필요한가: VF 를 없애려면 커널 자료구조와 하드웨어를 정해진 순서로
 * 정리해야 한다. 순서가 핵심이다:
 *   (1) 먼저 VF pci_dev 를 전부 제거한다 — 이때 VF 드라이버의 remove() 가
 *       불려 VF 하드웨어를 정상적으로 멈춘다.
 *   (2) 그 다음에 VF Enable/MSE 를 끈다 — 이 순간 VF 가 config space 에서 사라진다.
 *   (3) 1초를 기다린다(VF 소멸에 시간이 걸린다).
 *   (4) 아키텍처 훅으로 플랫폼 자원을 되돌린다.
 *   (5) dep_link 를 지우고 NumVFs 를 0 으로 되돌린다.
 * 순서를 뒤집으면 이미 없는 장치의 드라이버 remove 가 config 접근을 하다
 * 실패한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, ssleep 으로 1초 잠든다. sysfs 경로에서는
 * device_lock 과 rescan/remove 락을 쥔 상태.
 *
 * 호출 체인:
 *   sriov_numvfs_store() -> driver->sriov_configure(pdev, 0)
 *     -> pci_sriov_configure_simple() 또는 pci_disable_sriov() -> [sriov_disable]
 *     -> sriov_del_vfs(), pcibios_sriov_disable(), pci_iov_set_numvfs()
 */
static void sriov_disable(struct pci_dev *dev)	/* [한국어] static — 이 파일의 pci_disable_sriov 와 pci_sriov_configure_simple 만 부른다 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태 구조체 */

	if (!iov->num_VFs)	/* [한국어] 켜져 있는 VF 가 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	sriov_del_vfs(dev);	/* [한국어] 반드시 하드웨어를 끄기 전에 커널 쪽 VF 를 먼저 제거한다 */
	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);	/* [한국어] VF Enable(bit0)과 VF Memory Space Enable(bit3)을 사본에서 지운다 */
	pci_cfg_access_lock(dev);	/* [한국어] VF 가 사라지는 과도 구간의 config 접근을 막는다 */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);	/* [한국어] 실제 레지스터에 써서 하드웨어의 VF 를 없앤다 */
	ssleep(1);	/* [한국어] VF 소멸이 끝날 때까지 1초 기다린다 */
	pci_cfg_access_unlock(dev);	/* [한국어] config 접근 금지 해제 */

	pcibios_sriov_disable(dev);	/* [한국어] 아키텍처가 잡았던 플랫폼 자원을 되돌린다 */

	if (iov->link != dev->devfn)	/* [한국어] sriov_enable 에서 dep_link 를 만들었던 경우에만 */
		sysfs_remove_link(&dev->dev.kobj, "dep_link");	/* [한국어] 그 링크를 제거한다 */

	iov->num_VFs = 0;	/* [한국어] VF 가 하나도 없음을 사본에 기록. sriov_numvfs_show 가 이 값을 보여 준다 */
	pci_iov_set_numvfs(dev, 0);	/* [한국어] NumVFs 레지스터도 0 으로 되돌려 하드웨어 상태를 원점으로 */
}

/*
 * [한국어]
 * sriov_init - 열거 시각에 SR-IOV capability 를 읽어 PF 를 SR-IOV 가능 상태로 만든다
 *
 * @dev: 방금 발견된 PCIe 장치의 pci_dev.
 * @pos: pci_find_ext_capability() 가 찾아 준 SR-IOV 확장 capability 의 오프셋
 *       (확장 capability 이므로 0x100 이상).
 * @return: 0 성공(TotalVFs 가 0 이라 할 일이 없는 경우도 0),
 *          -EIO 하드웨어 값이 모순, -ENOMEM 할당 실패.
 *
 * 왜 필요한가: VF 를 나중에 켜려면 열거 시각에 미리 해 두어야 하는 일이 있다.
 * 특히 VF BAR 는 "VF 하나 크기 x TotalVFs" 만큼의 연속 MMIO 를 필요로 하는데,
 * 주소 배분은 열거 직후에 한 번만 일어나므로 그때 최대치를 잡아 두지 않으면
 * 나중에 확보할 수 없다. 이 함수가 그 예약을 한다.
 *
 * 동작 과정:
 *  (1) 혹시 펌웨어가 VF Enable 을 켜 둔 채 넘겨줬으면 먼저 끄고 1초 기다린다.
 *      VF 가 살아 있는 상태에서 BAR 를 재면 실제 트래픽을 방해하기 때문이다.
 *  (2) 같은 버스에 이미 초기화된 PF 가 있는지 찾는다. 없다면 이 PF 가 가장
 *      낮은 번호의 PF 이고, 이때만 ARI Capable Hierarchy 비트를 다룬다.
 *  (3) TotalVFs 를 읽는다. 0 이면 VF 를 만들 수 없는 장치이므로 조용히 끝낸다.
 *  (4) Supported Page Sizes 에서 CPU 페이지 크기 이상인 것 중 가장 작은 것을
 *      골라 System Page Size 에 쓴다.
 *  (5) VF BAR 6개의 크기를 재고(모두 1 을 쓰고 되읽는 표준 방법),
 *      각 BAR 의 크기를 iov->barsz[i] 에 보관한 뒤 PF 의 resource 크기를
 *      TotalVFs 배로 부풀린다.
 *  (6) 나머지 capability 값들을 사본에 담고 dev->sriov 를 연결, is_physfn 을 세운다.
 *  (7) compute_max_vf_buses() 로 필요한 버스 수를 조사한다.
 *
 * 왜 System Page Size 를 정해야 하는가: VF BAR 는 반드시 시스템 페이지 크기의
 * 배수로 정렬되어야 게스트에게 페이지 단위로 매핑해 줄 수 있다. 장치는
 * 자기가 지원하는 페이지 크기들을 비트맵으로 알려 주고, 소프트웨어가 그중
 * 하나를 골라 System Page Size 에 써 준다. 커널은 CPU 의 PAGE_SIZE 보다 작은
 * 후보를 지운 뒤 남은 것 중 가장 작은 것을 고른다.
 *
 * 실행 컨텍스트: 열거 중(pci_device_add 경로), 프로세스 컨텍스트.
 * ssleep 으로 잠들 수 있다. 아직 드라이버가 붙기 전이라 경쟁자가 없다.
 * 에러 경로: failed 는 VF BAR 플래그를 모두 지워 "이 장치에는 VF BAR 가 없다"로
 * 되돌리고 iov 를 해제한다. fail_max_buses 는 거기에 더해 dev->sriov 연결과
 * is_physfn 플래그까지 되돌린다.
 *
 * 호출 체인:
 *   pci_device_add() -> pci_init_capabilities() -> pci_iov_init() -> [sriov_init]
 *     -> __pci_size_stdbars(), __pci_read_base(), compute_max_vf_buses()
 */
static int sriov_init(struct pci_dev *dev, int pos)	/* [한국어] static — 이 파일의 pci_iov_init 만 부른다 */
{
	int i, bar64;	/* [한국어] i: VF BAR 루프 변수 겸 페이지 시프트 계산용, bar64: 방금 읽은 BAR 가 64비트인지 */
	int rc;	/* [한국어] 오류 코드 */
	int nres;	/* [한국어] 실제로 쓸 수 있는 VF BAR 개수 */
	u32 pgsz;	/* [한국어] Supported/System Page Size 계산용 */
	u16 ctrl, total;	/* [한국어] ctrl: SR-IOV Control 에 쓸 값, total: TotalVFs */
	struct pci_sriov *iov;	/* [한국어] 새로 할당할 PF 의 SR-IOV 상태 구조체 */
	struct resource *res;	/* [한국어] VF BAR 에 대응하는 PF 의 resource 항목 */
	const char *res_name;	/* [한국어] 로그에 찍을 리소스 이름 */
	struct pci_dev *pdev;	/* [한국어] 같은 버스에서 먼저 초기화된 PF 를 담을 변수 */
	u32 sriovbars[PCI_SRIOV_NUM_BARS];	/* [한국어] VF BAR 6개의 크기 마스크를 한 번에 받아 둘 배열 */

	pci_read_config_word(dev, pos + PCI_SRIOV_CTRL, &ctrl);	/* [한국어] capability + 0x08(SR-IOV Control)을 읽어 현재 상태를 본다 */
	if (ctrl & PCI_SRIOV_CTRL_VFE) {	/* [한국어] 펌웨어나 이전 커널이 VF Enable 을 켜 둔 채 넘겨줬다면 */
		pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, 0);	/* [한국어] Control 전체를 0 으로 써서 VF 를 끈다 — 살아 있는 VF 위에서 BAR 를 재면 안 된다 */
		ssleep(1);	/* [한국어] VF 가 완전히 사라질 때까지 1초 기다린다 */
	}

	ctrl = 0;	/* [한국어] 이제부터 만들 Control 값. 기본은 모든 비트 0 */
	list_for_each_entry(pdev, &dev->bus->devices, bus_list)	/* [한국어] 같은 버스에 매달린 장치들을 순회한다. 버스 목록은 devfn 오름차순이므로 처음 만나는 PF 가 가장 낮은 번호의 PF 다 */
		if (pdev->is_physfn)	/* [한국어] 이미 sriov_init 을 마친 PF 가 있으면(is_physfn 이 그때 세워진다) */
			goto found;	/* [한국어] ARI 비트를 건드리지 않고(ctrl 은 0 인 채) 곧바로 레지스터 쓰기로 간다. pdev 에는 그 PF 가 남는다 */

	pdev = NULL;	/* [한국어] 루프를 끝까지 돌았다면 앞선 PF 가 없다는 뜻이므로 pdev 를 NULL 로 확정한다 */
	if (pci_ari_enabled(dev->bus))	/* [한국어] 이 버스 위의 다리에서 ARI Forwarding 이 켜져 있는지 본다(pci_ari_enabled 는 bus->self->ari_enabled 를 읽는 인라인) */
		ctrl |= PCI_SRIOV_CTRL_ARI;	/* [한국어] ARI 가 가능하면 SR-IOV Control 의 ARI Capable Hierarchy(bit4)를 세운다. 이 비트가 서면 VF 의 devfn 8비트를 device+function 이 아니라 8비트 function 번호로 해석해, 한 버스에 256개까지 VF 를 놓을 수 있다 */

found:	/* [한국어] 앞선 PF 를 찾은 경로와 못 찾은 경로가 합류한다 */
	pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, ctrl);	/* [한국어] 결정된 Control 값을 실제 레지스터에 쓴다(앞선 PF 가 있으면 0, 없고 ARI 가능하면 ARI 비트만) */

	pci_read_config_word(dev, pos + PCI_SRIOV_TOTAL_VF, &total);	/* [한국어] capability + 0x0e(TotalVFs)를 읽는다 — 하드웨어가 지원하는 최대 VF 수 */
	if (!total)	/* [한국어] 0 이면 VF 를 만들 수 없는 장치다 */
		return 0;	/* [한국어] 오류가 아니라 "SR-IOV 를 쓰지 않는다" 이므로 0 을 반환한다. dev->sriov 는 NULL 로 남는다 */

	pci_read_config_dword(dev, pos + PCI_SRIOV_SUP_PGSIZE, &pgsz);	/* [한국어] capability + 0x1c(Supported Page Sizes)를 읽는다. 비트 n 이 서 있으면 4KB<<n 크기를 지원한다는 뜻 */
	i = PAGE_SHIFT > 12 ? PAGE_SHIFT - 12 : 0;	/* [한국어] CPU 의 PAGE_SIZE 가 4KB 보다 크면 그 차이만큼의 비트 수를 구한다(PAGE_SHIFT 12 는 4KB). 4KB 페이지 시스템이면 0 */
	pgsz &= ~((1 << i) - 1);	/* [한국어] CPU 페이지보다 작은 후보 비트들을 지운다 — VF BAR 를 CPU 페이지 단위로 매핑할 수 없으면 게스트에 넘길 수 없다 */
	if (!pgsz)	/* [한국어] 남은 후보가 하나도 없으면 이 장치는 이 커널의 페이지 크기를 지원하지 않는다 */
		return -EIO;	/* [한국어] 하드웨어 구성 불일치이므로 -EIO */

	pgsz &= ~(pgsz - 1);	/* [한국어] x & -x 와 같은 관용구로 가장 낮은 1 비트만 남긴다 — 즉 지원 가능한 것 중 가장 작은 페이지 크기를 고른다 */
	pci_write_config_dword(dev, pos + PCI_SRIOV_SYS_PGSIZE, pgsz);	/* [한국어] capability + 0x20(System Page Size)에 그 값을 써서 VF BAR 정렬 단위를 확정한다 */

	iov = kzalloc_obj(*iov);	/* [한국어] PF 하나당 하나씩 필요한 SR-IOV 상태 구조체를 0 으로 초기화해 할당한다 */
	if (!iov)	/* [한국어] 할당 실패면 */
		return -ENOMEM;	/* [한국어] 메모리 부족을 알린다 */

	/* Sizing SR-IOV BARs with VF Enable cleared - no decode */
	__pci_size_stdbars(dev, PCI_SRIOV_NUM_BARS,	/* [한국어] VF BAR 6개에 모두 1 을 쓰고 되읽어 크기 마스크를 한 번에 얻는다. 위에서 VF Enable 을 꺼 두었으므로 디코딩 중이 아니라 안전하다 */
			   pos + PCI_SRIOV_BAR, sriovbars);	/* [한국어] 읽을 위치는 capability + 0x24(VF BAR0)부터 dword 6개 */

	nres = 0;	/* [한국어] 실제로 쓸 수 있는 VF BAR 개수를 셀 준비 */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {	/* [한국어] VF BAR 0~5 를 차례로 처리한다 */
		int idx = pci_resource_num_from_vf_bar(i);	/* [한국어] VF BAR 번호를 PF 의 resource[] 인덱스로 변환(i + PCI_IOV_RESOURCES) */

		res = &dev->resource[idx];	/* [한국어] PF 의 resource 배열에서 이 VF BAR 가 쓸 칸 */
		res_name = pci_resource_name(dev, idx);	/* [한국어] 로그용 이름 문자열을 얻는다 */

		/*
		 * If it is already FIXED, don't change it, something
		 * (perhaps EA or header fixups) wants it this way.
		 */
		if (res->flags & IORESOURCE_PCI_FIXED)	/* [한국어] IORESOURCE_PCI_FIXED 면 누군가(EA capability 나 헤더 quirk)가 이미 주소를 확정해 둔 것이라 다시 재면 안 된다 */
			bar64 = (res->flags & IORESOURCE_MEM_64) ? 1 : 0;	/* [한국어] 그때는 크기만 그대로 두고 64비트 BAR 인지 여부만 플래그에서 알아낸다 */
		else
			bar64 = __pci_read_base(dev, pci_bar_unknown, res,	/* [한국어] 표준 BAR 파싱 루틴으로 크기와 속성을 해석해 res 에 채운다. 반환값은 이 BAR 가 64비트면 1, 32비트면 0 */
						pos + PCI_SRIOV_BAR + i * 4,	/* [한국어] 읽을 config 오프셋: capability + 0x24 + (BAR 번호 x 4바이트) */
						&sriovbars[i]);	/* [한국어] 위에서 미리 읽어 둔 크기 마스크를 넘겨 다시 측정하지 않게 한다 */
		if (!res->flags)	/* [한국어] flags 가 0 이면 구현되지 않은 BAR 이거나 64비트 BAR 의 상위 dword 자리다 */
			continue;	/* [한국어] 건너뛴다 */
		if (resource_size(res) & (PAGE_SIZE - 1)) {	/* [한국어] VF BAR 크기가 CPU 페이지 크기의 배수가 아니면 VF 별로 페이지 단위 매핑을 할 수 없다 */
			rc = -EIO;	/* [한국어] 하드웨어 구성 오류 */
			goto failed;	/* [한국어] 이미 건드린 resource 플래그를 되돌리러 간다 */
		}
		iov->barsz[i] = resource_size(res);	/* [한국어] "VF 한 개"의 BAR 크기를 사본에 보관한다. pci_iov_resource_size 가 이 값을 돌려준다 */
		resource_set_size(res, resource_size(res) * total);	/* [한국어] PF 의 resource 크기를 TotalVFs 배로 부풀린다 — 모든 VF 의 이 BAR 를 한 덩어리로 예약하기 위해서다. 이 부풀리기가 VF BAR 가 보통 BAR 와 다른 결정적 지점이다 */
		pci_info(dev, "%s %pR: contains BAR %d for %d VFs\n",	/* [한국어] 어떤 리소스가 몇 개의 VF 를 위한 몇 번 BAR 인지 로그로 남긴다 */
			 res_name, res, i, total);	/* [한국어] res_name, 주소 범위(%pR), BAR 번호, VF 수 */
		i += bar64;	/* [한국어] 64비트 BAR 였다면 다음 칸(상위 dword)은 건너뛴다 */
		nres++;	/* [한국어] 쓸 수 있는 VF BAR 하나를 세었다. sriov_enable 이 나중에 이 개수와 대조한다 */
	}

	iov->pos = pos;	/* [한국어] capability 오프셋을 사본에 보관 — 이후 모든 SR-IOV 레지스터 접근의 기준점 */
	iov->nres = nres;	/* [한국어] 배정된 VF BAR 개수 */
	iov->ctrl = ctrl;	/* [한국어] 위에서 결정한 Control 값(ARI 비트 포함 여부)을 사본에 남긴다. 복원 경로가 이 값을 되쓴다 */
	iov->total_VFs = total;	/* [한국어] 하드웨어 상한 */
	iov->driver_max_VFs = total;	/* [한국어] 드라이버가 pci_sriov_set_totalvfs 로 낮추기 전의 초기 상한 */
	pci_read_config_word(dev, pos + PCI_SRIOV_VF_DID, &iov->vf_device);	/* [한국어] capability + 0x1a(VF Device ID)를 읽어 둔다. VF pci_dev 의 device 필드가 이 값이 된다 */
	iov->pgsz = pgsz;	/* [한국어] 위에서 고른 System Page Size 를 보관 — 복원 경로가 다시 써야 한다 */
	iov->self = dev;	/* [한국어] 자기 자신을 가리키는 포인터 */
	iov->drivers_autoprobe = true;	/* [한국어] 기본 정책은 "VF 에 드라이버를 자동으로 붙인다". sysfs 로 끌 수 있다 */
	pci_read_config_dword(dev, pos + PCI_SRIOV_CAP, &iov->cap);	/* [한국어] capability + 0x04(SR-IOV Capabilities)를 읽어 둔다. sriov_enable 이 VF Migration 비트를 본다 */
	pci_read_config_byte(dev, pos + PCI_SRIOV_FUNC_LINK, &iov->link);	/* [한국어] capability + 0x12(Function Dependency Link)를 읽는다 — 함께 켜야 하는 PF 의 함수 번호 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END)	/* [한국어] Root Complex Integrated Endpoint(PCIe type 0x9)인 경우 */
		iov->link = PCI_DEVFN(PCI_SLOT(dev->devfn), iov->link);	/* [한국어] 그 장치의 Function Dependency Link 는 함수 번호만 담고 있으므로, 자기 device 번호와 합쳐 완전한 devfn 으로 만든다 */
	iov->vf_rebar_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_VF_REBAR);	/* [한국어] VF Resizable BAR 확장 capability(ID 0x24)의 위치를 찾아 둔다. 없으면 0 */

	if (pdev)	/* [한국어] 같은 버스에 먼저 초기화된 PF 가 있었다면 */
		iov->dev = pci_dev_get(pdev);	/* [한국어] 그 "가장 낮은 번호의 PF" 를 참조 카운트를 올려 붙잡아 둔다 */
	else
		iov->dev = dev;	/* [한국어] 자기 자신이 가장 낮은 번호의 PF 다(참조를 따로 올리지 않는다) */

	dev->sriov = iov;	/* [한국어] 완성된 구조체를 pci_dev 에 연결한다. 이 순간부터 다른 코드가 dev->sriov 를 볼 수 있다 */
	dev->is_physfn = 1;	/* [한국어] PF 임을 표시. dev_is_pf 매크로와 sysfs 그룹 가시성이 이 플래그를 본다 */
	rc = compute_max_vf_buses(dev);	/* [한국어] 가능한 모든 NumVFs 를 시험해 최대 버스 번호를 조사한다 */
	if (rc)	/* [한국어] 하드웨어가 모순된 offset/stride 를 보고했다면 */
		goto fail_max_buses;	/* [한국어] 방금 세운 sriov 연결까지 되돌려야 한다 */

	return 0;	/* [한국어] PF 초기화 성공 — 이제 sysfs 에 sriov_* 파일이 나타난다 */

fail_max_buses:	/* [한국어] compute_max_vf_buses 실패 되감기 */
	dev->sriov = NULL;	/* [한국어] 연결을 끊어 다른 코드가 반쯤 초기화된 구조체를 보지 못하게 한다 */
	dev->is_physfn = 0;	/* [한국어] PF 표시도 지운다 */
failed:	/* [한국어] BAR 크기 측정 도중 실패한 경우가 합류하는 지점 */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {	/* [한국어] VF BAR 6칸 모두에 대해 */
		res = &dev->resource[pci_resource_num_from_vf_bar(i)];	/* [한국어] PF 의 resource 항목을 찾아 */
		res->flags = 0;	/* [한국어] flags 를 0 으로 만들어 "이 장치에는 VF BAR 가 없다" 상태로 되돌린다. 부풀려 둔 크기도 이로써 무의미해진다 */
	}

	kfree(iov);	/* [한국어] 할당했던 SR-IOV 상태 구조체를 해제 */
	return rc;	/* [한국어] -EIO 또는 -ENOMEM 을 pci_iov_init 에 전달한다 */
}

/*
 * [한국어]
 * sriov_release - PF 가 사라질 때 SR-IOV 상태 구조체를 해제한다
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: sriov_init() 이 kzalloc 한 struct pci_sriov 와 거기서 올린
 * 참조 카운트를 되돌린다. pci_dev 가 해제되는 마지막 단계에서 불린다.
 * BUG_ON 의 근거: 여기 도달했다는 것은 PF 의 pci_dev 가 사라진다는 뜻인데
 * VF 가 남아 있다면 그 VF 들은 이미 존재하지 않는 PF 를 physfn 으로
 * 가리키고 있는 셈이다. 그 상태로 진행하면 확실히 죽으므로 즉시 멈춘다.
 * 실행 컨텍스트: pci_release_dev() 안, 프로세스 컨텍스트. 이 시점에는
 * 장치가 이미 모든 목록에서 빠져 있어 경쟁자가 없다.
 *
 * 호출 체인:
 *   pci_release_dev() -> pci_iov_release() -> [sriov_release]
 */
static void sriov_release(struct pci_dev *dev)	/* [한국어] static — pci_iov_release 만 부른다 */
{
	BUG_ON(dev->sriov->num_VFs);	/* [한국어] VF 가 남아 있는데 PF 를 해제하는 것은 회복 불가능한 상태이므로 즉시 멈춘다 */

	if (dev != dev->sriov->dev)	/* [한국어] iov->dev 가 자기 자신이 아니라면 sriov_init 에서 다른 PF 를 pci_dev_get 으로 잡아 둔 것이다 */
		pci_dev_put(dev->sriov->dev);	/* [한국어] 그 참조를 되돌린다 */

	kfree(dev->sriov);	/* [한국어] sriov_init 이 할당한 구조체를 해제 */
	dev->sriov = NULL;	/* [한국어] 남아 있는 포인터로 접근하지 못하도록 NULL 로 만든다 */
}

/*
 * [한국어]
 * sriov_restore_vf_rebar_state - VF Resizable BAR 로 정한 크기를 복원한다
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음. VF ReBAR capability 가 없으면 아무 것도 하지 않는다.
 *
 * 왜 필요한가: 전원 상태 전이(D3hot -> D0), FLR, 슬롯 리셋 등을 거치면
 * config space 의 많은 레지스터가 기본값으로 돌아간다. VF BAR 크기를
 * ReBAR 로 바꿔 두었다면 그 설정도 날아가므로, 커널이 기억하고 있는
 * iov->barsz[] 값을 다시 레지스터에 써 넣어야 한다. 그러지 않으면
 * PF 의 resource 가 가리키는 크기와 하드웨어의 실제 크기가 어긋난다.
 *
 * ReBAR capability 의 구조: 헤더 뒤에 (Capability, Control) dword 쌍이
 * 8바이트씩 이어진다. 첫 Control 의 NBAR 필드가 그런 쌍이 몇 개인지 알려 주고,
 * 각 Control 의 BAR Index 필드가 "이 쌍이 어느 BAR 를 다루는가"를 알려 준다.
 * 그래서 pos 를 8씩 늘리며 순회하고, 각 항목의 BAR Index 로 barsz[] 를 찾는다.
 *
 * 실행 컨텍스트: pci_restore_state() 경로. 프로세스 컨텍스트.
 * 반드시 sriov_restore_state() 보다 먼저 불려야 한다 — 크기를 먼저 맞춰야
 * 그 다음에 쓰는 BAR 주소가 유효하기 때문이다(pci_restore_iov_state 의 순서).
 *
 * 호출 체인:
 *   pci_restore_state() -> pci_restore_iov_state() -> [sriov_restore_vf_rebar_state]
 *     -> pci_iov_vf_rebar_cap(), pci_rebar_bytes_to_size()
 */
static void sriov_restore_vf_rebar_state(struct pci_dev *dev)	/* [한국어] static — pci_restore_iov_state 만 부른다 */
{
	unsigned int pos, nbars, i;	/* [한국어] pos: 현재 보고 있는 ReBAR 항목의 오프셋, nbars: 항목 개수, i: 루프 변수 */
	u32 ctrl;	/* [한국어] ReBAR Control 레지스터 값 */

	pos = pci_iov_vf_rebar_cap(dev);	/* [한국어] sriov_init 이 찾아 둔 VF Resizable BAR capability 오프셋을 얻는다(PF 가 아니거나 없으면 0) */
	if (!pos)	/* [한국어] capability 가 없으면 */
		return;	/* [한국어] 복원할 것이 없다 */

	pci_read_config_dword(dev, pos + PCI_VF_REBAR_CTRL, &ctrl);	/* [한국어] capability + 8(PCI_VF_REBAR_CTRL)의 첫 Control 레지스터를 읽는다 */
	nbars = FIELD_GET(PCI_VF_REBAR_CTRL_NBAR_MASK, ctrl);	/* [한국어] NBAR 필드(마스크 0x000000E0, 즉 비트 5~7)에서 리사이즈 가능 BAR 항목 수를 뽑는다 */

	for (i = 0; i < nbars; i++, pos += 8) {	/* [한국어] 항목마다 8바이트씩 떨어져 있으므로 pos 를 8씩 늘리며 돈다 */
		int bar_idx, size;	/* [한국어] bar_idx: 이 항목이 다루는 BAR 번호, size: 되돌릴 크기의 인코딩 값 */

		pci_read_config_dword(dev, pos + PCI_VF_REBAR_CTRL, &ctrl);	/* [한국어] 이 항목의 Control 레지스터를 읽는다 */
		bar_idx = FIELD_GET(PCI_VF_REBAR_CTRL_BAR_IDX, ctrl);	/* [한국어] BAR Index 필드(마스크 0x00000007, 비트 0~2)에서 대상 BAR 번호를 뽑는다 */
		size = pci_rebar_bytes_to_size(dev->sriov->barsz[bar_idx]);	/* [한국어] 커널이 기억하는 그 VF BAR 의 바이트 크기를 ReBAR 인코딩(0=1MB)으로 되돌린다 */
		ctrl &= ~PCI_VF_REBAR_CTRL_BAR_SIZE;	/* [한국어] BAR Size 필드(마스크 0x00001F00, 비트 8~12)를 일단 지우고 */
		ctrl |= FIELD_PREP(PCI_VF_REBAR_CTRL_BAR_SIZE, size);	/* [한국어] 거기에 복원할 크기 인코딩을 넣는다 */
		pci_write_config_dword(dev, pos + PCI_VF_REBAR_CTRL, ctrl);	/* [한국어] 수정한 Control 을 실제 레지스터에 써서 하드웨어의 VF BAR 크기를 되돌린다 */
	}
}

/*
 * [한국어]
 * sriov_restore_state - 전원/리셋 이후 SR-IOV 레지스터 전체를 복원한다
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: PF 가 D3hot 에서 D0 으로 돌아오거나 FLR/슬롯 리셋을 겪으면
 * SR-IOV capability 의 레지스터가 초기값으로 돌아간다. 그런데 커널의
 * pci_dev 트리에는 여전히 VF 들이 살아 있다고 기록되어 있다. 이 함수가
 * 하드웨어를 커널의 기억(iov 사본)에 맞춰 되돌려 그 불일치를 없앤다.
 *
 * 복원 순서가 중요하다:
 *  (1) 이미 VF Enable 이 서 있으면 복원이 필요 없다(레지스터가 살아남았다).
 *  (2) ARI 비트를 먼저 되돌린다 — 원문 영어 주석의 근거대로,
 *      pci_iov_set_numvfs() 가 읽어 오는 offset/stride 값 자체가
 *      ARI 설정에 따라 달라지기 때문이다.
 *  (3) VF BAR 6개의 주소를 다시 쓴다(pci_update_resource -> pci_iov_update_resource).
 *  (4) System Page Size 를 다시 쓴다.
 *  (5) NumVFs 를 되돌린다.
 *  (6) 마지막으로 Control 사본 전체(VFE|MSE 포함)를 써서 VF 를 되살린다.
 *  (7) VF 가 켜졌다면 config space 가 응답할 때까지 100ms 기다린다.
 * 주소를 VF Enable 보다 먼저 쓰는 이유는, VF 가 켜진 상태에서는 VF BAR 를
 * 바꿀 수 없기 때문이다(pci_iov_update_resource 가 그 경우 경고를 낸다).
 *
 * 실행 컨텍스트: pci_restore_state() 경로. 프로세스 컨텍스트, msleep 으로 잠든다.
 *
 * 호출 체인:
 *   pci_restore_state() -> pci_restore_iov_state() -> [sriov_restore_state]
 *     -> pci_update_resource(), pci_iov_set_numvfs()
 */
static void sriov_restore_state(struct pci_dev *dev)	/* [한국어] static — pci_restore_iov_state 만 부른다 */
{
	int i;	/* [한국어] VF BAR 루프 변수 */
	u16 ctrl;	/* [한국어] 현재 하드웨어의 SR-IOV Control 값 */
	struct pci_sriov *iov = dev->sriov;	/* [한국어] 커널이 기억하는 SR-IOV 상태 */

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_CTRL, &ctrl);	/* [한국어] 현재 Control 레지스터를 읽어 실제로 초기화되었는지 확인한다 */
	if (ctrl & PCI_SRIOV_CTRL_VFE)	/* [한국어] VF Enable 이 아직 서 있다면 레지스터가 살아남은 것이므로 */
		return;	/* [한국어] 복원할 필요가 없다. 오히려 되쓰면 살아 있는 VF 를 방해한다 */

	/*
	 * Restore PCI_SRIOV_CTRL_ARI before pci_iov_set_numvfs() because
	 * it reads offset & stride, which depend on PCI_SRIOV_CTRL_ARI.
	 */
	ctrl &= ~PCI_SRIOV_CTRL_ARI;	/* [한국어] 현재 값에서 ARI 비트를 지우고 */
	ctrl |= iov->ctrl & PCI_SRIOV_CTRL_ARI;	/* [한국어] 커널이 기억하는 ARI 설정으로 대체한다(다른 비트는 아직 건드리지 않는다) */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, ctrl);	/* [한국어] ARI 만 먼저 반영한다 — 아래에서 읽을 offset/stride 가 이 설정에 따라 달라지기 때문 */

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++)	/* [한국어] VF BAR 6칸 각각에 대해 */
		pci_update_resource(dev, pci_resource_num_from_vf_bar(i));	/* [한국어] resource 에 기록된 주소를 하드웨어에 다시 쓴다. 내부에서 VF BAR 이면 pci_iov_update_resource 로 넘어간다 */

	pci_write_config_dword(dev, iov->pos + PCI_SRIOV_SYS_PGSIZE, iov->pgsz);	/* [한국어] System Page Size(+0x20)를 sriov_init 때 고른 값으로 되돌린다 */
	pci_iov_set_numvfs(dev, iov->num_VFs);	/* [한국어] NumVFs 를 되돌리고 그에 맞춰 offset/stride 사본을 다시 읽는다 */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);	/* [한국어] 마지막으로 Control 사본 전체를 쓴다 — VF Enable 과 MSE 가 여기 들어 있어 이 한 줄로 VF 가 되살아난다 */
	if (iov->ctrl & PCI_SRIOV_CTRL_VFE)	/* [한국어] 실제로 VF 를 켜는 경우에만 */
		msleep(100);	/* [한국어] VF 의 config space 가 응답할 때까지 100ms 기다린다(sriov_enable 과 같은 이유) */
}

/**
 * pci_iov_init - initialize the IOV capability
 * @dev: the PCI device
 *
 * Returns 0 on success, or negative on failure.
 */
/*
 * [한국어]
 * pci_iov_init - 이 장치에 SR-IOV capability 가 있는지 보고 있으면 초기화한다
 *
 * @dev: 방금 발견된 PCI 장치의 pci_dev.
 * @return: 0 성공, -ENODEV 면 SR-IOV 를 쓸 수 없는 장치(PCIe 가 아니거나
 *          capability 가 없음). sriov_init() 의 오류가 그대로 나오기도 한다.
 *          호출자 pci_init_capabilities() 는 반환값을 무시한다 —
 *          SR-IOV 가 없다고 해서 장치 열거가 실패하면 안 되기 때문이다.
 *
 * 왜 필요한가: SR-IOV 는 PCIe 확장 capability(Extended Capability)로만 존재한다.
 * 확장 capability 는 config space 의 0x100 번지부터 링크드 리스트로 이어지며,
 * 그 공간은 PCIe 에만 있다(전통 PCI 는 256바이트뿐). 그래서 먼저 PCIe 인지
 * 확인하고, 그 다음 ID 0x10(PCI_EXT_CAP_ID_SRIOV)을 찾는다.
 * 실행 컨텍스트: 열거 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_scan_single_device() -> pci_device_add() -> pci_init_capabilities()
 *     -> [pci_iov_init] -> pci_find_ext_capability() -> sriov_init()
 */
int pci_iov_init(struct pci_dev *dev)	/* [한국어] drivers/pci/pci.h 에 선언되어 probe.c 가 부른다 */
{
	int pos;	/* [한국어] SR-IOV capability 의 config space 오프셋을 담을 변수 */

	if (!pci_is_pcie(dev))	/* [한국어] 확장 capability 공간(0x100 이상)은 PCIe 에만 있다 */
		return -ENODEV;	/* [한국어] 전통 PCI 장치에는 SR-IOV 가 있을 수 없다 */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV);	/* [한국어] ID 0x10(SR-IOV)을 확장 capability 리스트에서 찾는다. 리스트는 0x100 에서 시작해 각 헤더가 다음 오프셋을 가리키는 사슬 구조다 */
	if (pos)	/* [한국어] 찾았으면 */
		return sriov_init(dev, pos);	/* [한국어] 본격적인 초기화로 넘어간다 */

	return -ENODEV;	/* [한국어] SR-IOV capability 가 없는 보통 PCIe 장치 — 오류가 아니라 정상이다 */
}

/**
 * pci_iov_release - release resources used by the IOV capability
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_iov_release - PF 의 pci_dev 가 해제될 때 SR-IOV 자원을 반납한다
 *
 * @dev: 해제되는 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: pci_iov_init() 의 짝이다. 모든 pci_dev 의 해제 경로에서
 * 불리므로 PF 인지 먼저 확인한다. is_physfn 은 sriov_init() 이 끝까지
 * 성공했을 때만 1 이므로, 이 검사가 곧 "해제할 sriov 구조체가 있는가"다.
 * 실행 컨텍스트: pci_release_dev(), 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_release_dev() -> [pci_iov_release] -> sriov_release()
 */
void pci_iov_release(struct pci_dev *dev)	/* [한국어] drivers/pci/pci.h 에 선언되어 probe.c 가 부른다 */
{
	if (dev->is_physfn)	/* [한국어] PF 일 때만 해제할 sriov 구조체가 존재한다 */
		sriov_release(dev);	/* [한국어] 구조체 해제와 참조 카운트 반납을 맡긴다 */
}

/**
 * pci_iov_remove - clean up SR-IOV state after PF driver is detached
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_iov_remove - PF 드라이버가 언바인드될 때 SR-IOV 상태를 정리한다
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: driver_max_VFs 는 PF 드라이버가 pci_sriov_set_totalvfs() 로
 * 낮춰 놓았을 수 있는 값이다. 그 드라이버가 떠났으니 그 제한도 함께
 * 사라져야 한다. 그래서 하드웨어 상한인 total_VFs 로 되돌린다. 다음에
 * 다른 드라이버가 붙으면 다시 자기 값으로 낮출 수 있다.
 * 경고의 의미: 드라이버는 remove 하기 전에 자기가 켠 VF 를 전부 꺼야 한다.
 * 끄지 않고 떠났다면 VF pci_dev 들이 주인 없는 상태로 남으므로 경고한다.
 * 여기서 강제로 끄지는 않는다 — 이미 게스트에 넘어간 VF 를 커널이 임의로
 * 없애면 더 큰 문제가 되기 때문이다.
 * 실행 컨텍스트: 드라이버 언바인드 경로(pci_device_remove), 프로세스 컨텍스트.
 * device_lock 을 쥔 상태에서 불린다.
 *
 * 호출 체인:
 *   pci_device_remove()(drivers/pci/pci-driver.c) -> [pci_iov_remove]
 */
void pci_iov_remove(struct pci_dev *dev)	/* [한국어] drivers/pci/pci.h 에 선언되어 pci-driver.c 가 부른다 */
{
	struct pci_sriov *iov = dev->sriov;	/* [한국어] PF 의 SR-IOV 상태. is_physfn 검사보다 앞서 대입하지만 아래에서 검사 후에만 역참조한다 */

	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 정리할 것이 없다 */
		return;	/* [한국어] 조용히 돌아간다 */

	iov->driver_max_VFs = iov->total_VFs;	/* [한국어] 드라이버가 낮춰 놓았을 수 있는 상한을 하드웨어 상한으로 되돌린다 */
	if (iov->num_VFs)	/* [한국어] VF 를 켜 둔 채 드라이버가 떠났다면 */
		pci_warn(dev, "driver left SR-IOV enabled after remove\n");	/* [한국어] 경고만 남긴다. 강제로 끄면 이미 게스트에 넘어간 VF 를 빼앗게 되므로 하지 않는다 */
}

/**
 * pci_iov_update_resource - update a VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 *
 * Update a VF BAR in the SR-IOV capability of a PF.
 */
/*
 * [한국어]
 * pci_iov_update_resource - PF 의 SR-IOV capability 안에 있는 VF BAR 레지스터를 다시 쓴다
 *
 * @dev: 대상 pci_dev(PF 가 아닐 수도 있다 — 아래 참조).
 * @resno: PF 의 resource[] 인덱스. VF BAR 구간이어야 의미가 있다.
 * @return: 없음.
 *
 * 왜 필요한가: 커널이 resource[] 에 정한 주소를 실제 하드웨어 레지스터에
 * 반영하는 함수다. 보통 BAR 는 pci_std_update_resource() 가 config space
 * 0x10~0x24 에 쓰지만, VF BAR 는 그 자리가 아니라 PF 의 SR-IOV capability
 * 안(pos + 0x24 + 4*번호)에 있다. 그래서 별도 구현이 필요하다.
 * 이것이 VF BAR 가 일반 BAR 와 다른 또 하나의 지점이다 — 레지스터의 위치가
 * 표준 헤더가 아니라 확장 capability 안에 있다.
 *
 * 왜 PF 검사부터 하는가: 원문 영어 주석대로 pci_restore_bars() 는 모든
 * 장치에 대해 이 경로를 탄다. VF 자신이나 SR-IOV 가 없는 장치가 들어와도
 * 안전해야 하므로 is_physfn 이 아니면 iov 를 NULL 로 두고 곧바로 돌아간다.
 *
 * 왜 VFE|MSE 를 검사하는가: VF 가 켜져 주소를 디코딩하고 있는 중에 BAR 를
 * 바꾸면 진행 중인 DMA 가 엉뚱한 곳을 가리키게 된다. 그래서 그 상태면
 * 쓰지 않고 경고만 남긴다(커널 버그이므로 dev_WARN 으로 스택까지 찍는다).
 *
 * 64비트 BAR 처리: PCI 는 64비트 주소를 dword 두 칸에 나누어 담는다.
 * 아래 코드는 하위 32비트를 쓰고, MEM_64 플래그가 있으면 상위 32비트를
 * 다음 칸(reg + 4)에 쓴다.
 *
 * 실행 컨텍스트: 리소스 배분/복원 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_update_resource()(drivers/pci/setup-res.c) -> [pci_iov_update_resource]
 *   sriov_restore_state() -> pci_update_resource() -> [pci_iov_update_resource]
 */
void pci_iov_update_resource(struct pci_dev *dev, int resno)	/* [한국어] drivers/pci/pci.h 에 선언되어 setup-res.c 가 부른다 */
{
	struct pci_sriov *iov = dev->is_physfn ? dev->sriov : NULL;	/* [한국어] PF 일 때만 sriov 를 잡고, 아니면 NULL 로 두어 아래에서 안전하게 빠져나가게 한다 */
	struct resource *res = pci_resource_n(dev, resno);	/* [한국어] PF 의 resource 배열에서 해당 칸(&dev->resource[resno]) */
	int vf_bar = pci_resource_num_to_vf_bar(resno);	/* [한국어] resource 인덱스를 VF BAR 번호 0~5 로 변환 — 레지스터 오프셋 계산에 쓴다 */
	struct pci_bus_region region;	/* [한국어] CPU 주소를 PCI 버스 주소로 변환한 결과를 담을 구조체 */
	u16 cmd;	/* [한국어] SR-IOV Control 값을 읽어 둘 변수 */
	u32 new;	/* [한국어] BAR 레지스터에 쓸 32비트 값 */
	int reg;	/* [한국어] 쓸 config space 오프셋 */

	/*
	 * The generic pci_restore_bars() path calls this for all devices,
	 * including VFs and non-SR-IOV devices.  If this is not a PF, we
	 * have nothing to do.
	 */
	if (!iov)	/* [한국어] PF 가 아니면(VF 자신이거나 SR-IOV 없는 장치) 이 경로는 무의미하다 */
		return;	/* [한국어] 아무 것도 하지 않고 돌아간다 */

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_CTRL, &cmd);	/* [한국어] capability + 0x08(SR-IOV Control)을 읽어 현재 VF 상태를 확인한다 */
	if ((cmd & PCI_SRIOV_CTRL_VFE) && (cmd & PCI_SRIOV_CTRL_MSE)) {	/* [한국어] VF Enable(bit0)과 VF Memory Space Enable(bit3)이 모두 서 있으면 VF 들이 이 BAR 로 주소를 디코딩하는 중이다 */
		dev_WARN(&dev->dev, "can't update enabled VF BAR%d %pR\n",	/* [한국어] 그 상태에서 BAR 를 바꾸는 것은 커널 버그이므로 스택 추적까지 남기는 dev_WARN 을 쓴다 */
			 vf_bar, res);	/* [한국어] 어느 VF BAR 의 어떤 범위였는지 함께 찍는다 */
		return;	/* [한국어] 쓰지 않고 돌아간다 */
	}

	/*
	 * Ignore unimplemented BARs, unused resource slots for 64-bit
	 * BARs, and non-movable resources, e.g., those described via
	 * Enhanced Allocation.
	 */
	if (!res->flags)	/* [한국어] flags 가 0 이면 구현되지 않은 BAR 이거나 64비트 BAR 의 상위 dword 자리다 */
		return;	/* [한국어] 쓸 것이 없다 */

	if (res->flags & IORESOURCE_UNSET)	/* [한국어] IORESOURCE_UNSET 은 아직 주소가 배정되지 않았다는 뜻 */
		return;	/* [한국어] 쓸 주소가 없으므로 돌아간다 */

	if (res->flags & IORESOURCE_PCI_FIXED)	/* [한국어] IORESOURCE_PCI_FIXED 는 EA(Enhanced Allocation) 등으로 주소가 고정된 경우 */
		return;	/* [한국어] 옮길 수 없는 자원이므로 건드리지 않는다 */

	pcibios_resource_to_bus(dev->bus, &region, res);	/* [한국어] CPU 물리 주소를 PCI 버스가 보는 주소로 변환한다. 두 주소 공간이 다른 아키텍처가 있기 때문 */
	new = region.start;	/* [한국어] 버스 주소의 하위 32비트를 쓸 값으로 삼는다 */
	new |= res->flags & ~PCI_BASE_ADDRESS_MEM_MASK;	/* [한국어] BAR 의 하위 4비트는 주소가 아니라 타입 표시(bit0 IO/MEM, bit1~2 32/64비트, bit3 prefetchable)다. PCI_BASE_ADDRESS_MEM_MASK 가 ~0x0f 이므로 그 보수는 하위 4비트이며, res->flags 에 보존된 그 비트들을 되살려 넣는다 */

	reg = iov->pos + PCI_SRIOV_BAR + 4 * vf_bar;	/* [한국어] 쓸 위치 = capability 시작 + 0x24(VF BAR0) + 4바이트 x BAR 번호 */
	pci_write_config_dword(dev, reg, new);	/* [한국어] 하위 32비트를 쓴다 */
	if (res->flags & IORESOURCE_MEM_64) {	/* [한국어] 64비트 BAR 이면 상위 절반도 써야 한다 */
		new = region.start >> 16 >> 16;	/* [한국어] 32비트 시프트를 16씩 두 번 나눠 하는 것은 32비트 타입에서 정의되지 않는 시프트를 피하려는 커널 관용구다 */
		pci_write_config_dword(dev, reg + 4, new);	/* [한국어] 다음 dword 칸에 상위 32비트를 쓴다 */
	}
}

/*
 * [한국어]
 * pcibios_iov_resource_alignment - VF BAR 의 정렬 요구를 아키텍처가 바꿀 수 있는 훅(기본 구현)
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF 의 resource[] 인덱스.
 * @return: 이 VF BAR 가 요구하는 정렬(바이트). 기본은 VF BAR 하나의 크기.
 *
 * 왜 필요한가: 어떤 플랫폼(대표적으로 powerpc PowerNV)은 VF BAR 를
 * IOMMU 나 PE 단위 경계에 맞춰야 해서 더 큰 정렬을 요구한다. __weak 로
 * 열어 두어 그런 아키텍처가 재정의할 수 있게 한다. 기본 구현은
 * "VF BAR 하나 크기" 를 그대로 정렬로 쓴다.
 * 왜 그것이 정답인가: PF 의 resource 는 그 크기의 TotalVFs 배지만,
 * VF 별 구간이 barsz 간격으로 나뉘므로 시작 주소가 barsz 로 정렬되어
 * 있으면 모든 VF 의 BAR 가 자기 크기에 맞게 정렬된다.
 * 실행 컨텍스트: 리소스 배분(setup-bus.c) 경로, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_resource_alignment()(pci.h) -> pci_sriov_resource_alignment()
 *     -> [pcibios_iov_resource_alignment] -> pci_iov_resource_size()
 */
resource_size_t __weak pcibios_iov_resource_alignment(struct pci_dev *dev,	/* [한국어] __weak — 아키텍처가 같은 이름으로 재정의할 수 있다 */
						      int resno)	/* [한국어] PF 의 resource 인덱스 */
{
	return pci_iov_resource_size(dev, resno);	/* [한국어] 기본 정렬은 VF BAR 하나의 크기다 */
}

/**
 * pci_sriov_resource_alignment - get resource alignment for VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 *
 * Returns the alignment of the VF BAR found in the SR-IOV capability.
 * This is not the same as the resource size which is defined as
 * the VF BAR size multiplied by the number of VFs.  The alignment
 * is just the VF BAR size.
 */
/*
 * [한국어]
 * pci_sriov_resource_alignment - VF BAR 의 정렬 요구를 PCI 코어에 알려 준다
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF 의 resource[] 인덱스(VF BAR 구간).
 * @return: 요구되는 정렬 바이트 수.
 *
 * 왜 필요한가: 리소스 배분기는 각 resource 의 정렬 요구를 알아야 주소를
 * 정할 수 있다. 보통 BAR 는 "크기 = 정렬" 이지만 VF BAR 는 다르다.
 * 원문 영어 주석이 밝히듯 resource 의 크기는 "VF BAR 크기 x VF 수" 인 반면
 * 정렬은 "VF BAR 하나의 크기" 다. 이 구분을 놓치면 필요 이상으로 큰
 * 정렬을 요구해 주소 공간을 낭비하게 된다.
 * 실행 컨텍스트: 리소스 배분 경로. 순수 조회라 락 없음.
 *
 * 호출 체인:
 *   pci_resource_alignment()(drivers/pci/pci.h 인라인, IOV 리소스일 때)
 *     -> [pci_sriov_resource_alignment] -> pcibios_iov_resource_alignment()
 */
resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev, int resno)	/* [한국어] drivers/pci/pci.h 에 선언되어 pci_resource_alignment 인라인이 부른다 */
{
	return pcibios_iov_resource_alignment(dev, resno);	/* [한국어] 아키텍처 훅을 거쳐 정렬 값을 얻는다. 기본 구현은 VF BAR 하나의 크기 */
}

/**
 * pci_restore_iov_state - restore the state of the IOV capability
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_restore_iov_state - 전원 복귀/리셋 후 SR-IOV 관련 하드웨어 상태를 되살린다
 *
 * @dev: 상태를 복원 중인 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: pci_restore_state() 는 표준 config 헤더(BAR, Command, PCIe
 * capability 등)를 되돌리지만 SR-IOV capability 는 다루지 않는다. PF 였다면
 * 그 부분을 추가로 되살려야 커널이 기억하는 VF 구성과 하드웨어가 일치한다.
 * 순서가 중요하다: VF BAR 의 "크기"(ReBAR)를 먼저 되돌리고 나서
 * "주소와 활성화"(sriov_restore_state)를 복원한다. 크기가 틀린 상태에서
 * 주소를 쓰면 VF 구간이 서로 겹치거나 어긋나기 때문이다.
 * 실행 컨텍스트: pci_restore_state() 안. 프로세스 컨텍스트, msleep 으로 잠들 수 있다.
 * 이 경로는 D3hot -> D0 복귀, FLR, AER/DPC 복구 후 슬롯 리셋 등에서 지나간다.
 *
 * 호출 체인:
 *   pci_restore_state()(drivers/pci/pci.c) -> [pci_restore_iov_state]
 *     -> sriov_restore_vf_rebar_state(), sriov_restore_state()
 */
void pci_restore_iov_state(struct pci_dev *dev)	/* [한국어] drivers/pci/pci.h 에 선언되어 pci.c 의 pci_restore_state 가 부른다 */
{
	if (dev->is_physfn) {	/* [한국어] PF 일 때만 복원할 SR-IOV 상태가 있다 */
		sriov_restore_vf_rebar_state(dev);	/* [한국어] 먼저 VF BAR 의 크기 설정(VF Resizable BAR)을 되돌린다 */
		sriov_restore_state(dev);	/* [한국어] 그 다음 VF BAR 주소, System Page Size, NumVFs, Control 을 되돌린다 */
	}
}

/**
 * pci_vf_drivers_autoprobe - set PF property drivers_autoprobe for VFs
 * @dev: the PCI device
 * @auto_probe: set VF drivers auto probe flag
 */
/*
 * [한국어]
 * pci_vf_drivers_autoprobe - VF 에 드라이버를 자동으로 붙일지 여부를 코드로 설정한다
 *
 * @dev: PF 의 pci_dev.
 * @auto_probe: true 면 자동 바인딩, false 면 VF 를 만들되 드라이버를 붙이지 않는다.
 * @return: 없음.
 *
 * 왜 필요한가: sysfs 파일 sriov_drivers_autoprobe 와 같은 값을 커널 코드에서
 * 직접 설정하는 통로다. 플랫폼이 VF 를 만들되 호스트 드라이버가 잡지 못하게
 * 해야 할 때 쓴다. 실제로 커널 본체에서 이 함수를 부르는 곳은
 * arch/powerpc/platforms/pseries/pci.c 다(참조 커널 트리에서 grep 으로 확인).
 * 이 트리에는 arch/ 가 없어 호출자를 직접 볼 수는 없다.
 * 실제 판정은 drivers/pci/pci-driver.c 의 pci_device_can_probe() 가 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. bool 대입 하나라 락 없음.
 *
 * 호출 체인:
 *   플랫폼 코드 -> [pci_vf_drivers_autoprobe]
 *   (읽는 쪽) pci_device_probe() -> pci_device_can_probe() -> drivers_autoprobe
 */
void pci_vf_drivers_autoprobe(struct pci_dev *dev, bool auto_probe)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	if (dev->is_physfn)	/* [한국어] PF 가 아니면 설정할 sriov 구조체가 없다 */
		dev->sriov->drivers_autoprobe = auto_probe;	/* [한국어] 앞으로 만들어질 VF 들의 자동 바인딩 정책을 정한다. 이미 만들어진 VF 에는 소급 적용되지 않는다 */
}

/**
 * pci_iov_bus_range - find bus range used by Virtual Function
 * @bus: the PCI bus
 *
 * Returns max number of buses (exclude current one) used by Virtual
 * Functions.
 */
/*
 * [한국어]
 * pci_iov_bus_range - 이 버스의 PF 들이 VF 를 위해 필요로 하는 추가 버스 개수를 구한다
 *
 * @bus: 방금 스캔을 마친 PCI 버스.
 * @return: 이 버스 번호를 제외하고 VF 들이 더 쓸 버스의 개수. 0 이면 추가 불필요.
 *
 * 왜 필요한가: VF 는 PF 보다 뒤쪽 버스에 놓일 수 있는데, 버스 번호는 열거
 * 단계에서 한 번만 배분된다. 그래서 열거가 이 버스의 장치들을 다 찾은 직후
 * "여기 있는 PF 들이 최악의 경우 몇 개의 버스를 더 쓸까"를 물어 그만큼을
 * 미리 예약한다. 그 최악값은 sriov_init 때 compute_max_vf_buses() 가 조사해
 * iov->max_VF_buses 에 넣어 둔 값이다.
 * 계산: max_VF_buses 는 "가장 높은 버스 번호" 이므로 현재 버스 번호를 빼면
 * "추가로 필요한 버스 개수" 가 된다. PF 가 하나도 없으면 max 가 0 이고
 * 그때는 0 을 돌려준다(0 - bus->number 라는 음수를 내지 않기 위한 삼항 연산).
 * 실행 컨텍스트: 열거 중(pci_scan_child_bus_extend), 프로세스 컨텍스트.
 * 이 버스의 장치 목록은 방금 만들어졌고 아직 변하지 않으므로 락이 없다.
 *
 * 호출 체인:
 *   pci_scan_child_bus_extend()(drivers/pci/probe.c) -> [pci_iov_bus_range]
 */
int pci_iov_bus_range(struct pci_bus *bus)	/* [한국어] drivers/pci/pci.h 에 선언되어 probe.c 가 부른다 */
{
	int max = 0;	/* [한국어] 이 버스의 PF 들이 보고한 최대 버스 번호를 누적할 변수 */
	struct pci_dev *dev;	/* [한국어] 버스에 매달린 장치를 훑을 반복자 */

	list_for_each_entry(dev, &bus->devices, bus_list) {	/* [한국어] 이 버스에 있는 모든 장치를 순회한다 */
		if (!dev->is_physfn)	/* [한국어] PF 가 아닌 장치는 VF 를 만들지 않으므로 */
			continue;	/* [한국어] 건너뛴다 */
		if (dev->sriov->max_VF_buses > max)	/* [한국어] 그 PF 가 최악의 경우 도달하는 버스 번호가 지금까지 본 최대보다 크면 */
			max = dev->sriov->max_VF_buses;	/* [한국어] 갱신한다 */
	}

	return max ? max - bus->number : 0;	/* [한국어] PF 가 있었다면 (최대 버스 번호 - 현재 버스 번호) 가 추가로 필요한 버스 개수다. PF 가 없으면 0 */
}

/**
 * pci_enable_sriov - enable the SR-IOV capability
 * @dev: the PCI device
 * @nr_virtfn: number of virtual functions to enable
 *
 * Returns 0 on success, or negative on failure.
 */
/*
 * [한국어]
 * pci_enable_sriov - PF 드라이버가 코드에서 직접 SR-IOV 를 켤 때 쓰는 공개 API
 *
 * @dev: PF 의 pci_dev.
 * @nr_virtfn: 켤 VF 개수.
 * @return: 0 성공, -ENOSYS 면 SR-IOV 를 쓸 수 없는 장치, 그 밖에는
 *          sriov_enable() 이 낸 오류.
 *
 * 왜 필요한가: sysfs 를 거치지 않고 드라이버가 스스로 VF 를 켜고 싶을 때
 * 쓰는 진입점이다. 실제 일은 전부 sriov_enable() 이 한다. 이 얇은 래퍼가
 * 하는 일은 두 가지 안전 장치뿐이다.
 * might_sleep() 의 의미: 이 함수는 안에서 msleep(100) 과 sysfs 조작으로
 * 반드시 잠든다. 인터럽트 문맥이나 스핀락 안에서 부르면 커널이 굳는다.
 * might_sleep() 은 그런 잘못된 호출을 디버그 커널에서 즉시 잡아내는 표식이다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 device_lock 을 쥐고 있어야
 * 안전하다(드라이버 probe 안이면 코어가 이미 쥐고 있다).
 *
 * 호출 체인:
 *   PF 드라이버 -> [pci_enable_sriov] -> sriov_enable()
 */
int pci_enable_sriov(struct pci_dev *dev, int nr_virtfn)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	might_sleep();	/* [한국어] 잠들 수 있는 함수임을 명시 — 인터럽트 문맥/아토믹 문맥에서 부르면 디버그 커널이 경고한다 */

	if (!dev->is_physfn)	/* [한국어] SR-IOV capability 초기화에 성공하지 못한 장치라면 */
		return -ENOSYS;	/* [한국어] 지원하지 않는 기능이라는 뜻의 -ENOSYS */

	return sriov_enable(dev, nr_virtfn);	/* [한국어] 실제 활성화는 전부 sriov_enable 이 처리한다 */
}
EXPORT_SYMBOL_GPL(pci_enable_sriov);	/* [한국어] PF 드라이버가 모듈로 빌드되므로 심볼 공개가 필요하다 */

/**
 * pci_disable_sriov - disable the SR-IOV capability
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pci_disable_sriov - PF 드라이버가 코드에서 직접 SR-IOV 를 끌 때 쓰는 공개 API
 *
 * @dev: PF 의 pci_dev.
 * @return: 없음.
 *
 * 왜 필요한가: pci_enable_sriov() 의 짝. PF 드라이버는 remove() 하기 전에
 * 반드시 이 함수를 불러 자기가 켠 VF 를 정리해야 한다. 그러지 않으면
 * pci_iov_remove() 가 "driver left SR-IOV enabled after remove" 경고를 낸다.
 * 이 함수는 모든 VF 드라이버의 remove() 가 끝날 때까지 돌아오지 않는다.
 * 그 성질 덕분에 pci_iov_get_pf_drvdata() 가 락 없이도 안전할 수 있다.
 * might_sleep(): 안에서 ssleep(1) 로 반드시 잠든다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PF 드라이버 -> [pci_disable_sriov] -> sriov_disable()
 */
void pci_disable_sriov(struct pci_dev *dev)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	might_sleep();	/* [한국어] 안에서 ssleep(1) 로 잠들므로 아토믹 문맥에서 부르면 안 된다 */

	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 */
		return;	/* [한국어] 끌 VF 가 없다 */

	sriov_disable(dev);	/* [한국어] 실제 비활성화는 전부 sriov_disable 이 처리한다 */
}
EXPORT_SYMBOL_GPL(pci_disable_sriov);	/* [한국어] PF 드라이버 모듈용 심볼 공개 */

/**
 * pci_num_vf - return number of VFs associated with a PF device_release_driver
 * @dev: the PCI device
 *
 * Returns number of VFs, or 0 if SR-IOV is not enabled.
 */
/*
 * [한국어]
 * pci_num_vf - 현재 켜져 있는 VF 개수를 돌려준다
 *
 * @dev: 조회할 pci_dev.
 * @return: 켜져 있는 VF 개수. PF 가 아니거나 SR-IOV 가 꺼져 있으면 0.
 *
 * 왜 필요한가: 드라이버가 "지금 VF 가 몇 개 켜져 있나"를 물어 자기 자원
 * 배분을 결정하거나, VF 가 켜진 상태에서는 금지된 동작을 막을 때 쓴다.
 * sysfs 의 sriov_numvfs 와 같은 값(iov->num_VFs)을 본다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 단일 정수 읽기라 락을 잡지 않는다 —
 * 호출자가 device_lock 을 쥐고 있지 않으면 값이 순간적으로 낡을 수 있다.
 *
 * 호출 체인:
 *   PF 드라이버 -> [pci_num_vf]
 */
int pci_num_vf(struct pci_dev *dev)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 dev->sriov 가 없다 */
		return 0;	/* [한국어] VF 가 0 개라는 뜻으로 0 을 돌려준다 */

	return dev->sriov->num_VFs;	/* [한국어] sriov_enable 이 성공했을 때만 갱신되는 값 */
}
EXPORT_SYMBOL_GPL(pci_num_vf);	/* [한국어] PF 드라이버 모듈용 심볼 공개 */

/**
 * pci_vfs_assigned - returns number of VFs are assigned to a guest
 * @dev: the PCI device
 *
 * Returns number of VFs belonging to this device that are assigned to a guest.
 * If device is not a physical function returns 0.
 */
/*
 * [한국어]
 * pci_vfs_assigned - 이 PF 의 VF 중 게스트(가상 머신)에 넘어간 것의 개수를 센다
 *
 * @dev: PF 의 pci_dev.
 * @return: 게스트에 배정된 VF 개수. PF 가 아니면 0.
 *
 * 왜 필요한가: VF 가 이미 게스트에 넘어가 있는데 호스트가 SR-IOV 설정을
 * 바꾸면 게스트가 쓰던 장치가 갑자기 사라진다. 그래서 설정을 바꾸기 전에
 * 이 함수로 확인하고, 하나라도 배정되어 있으면 거절한다
 * (pci_sriov_configure_simple 이 그렇게 한다).
 *
 * 어떻게 세는가: VF 의 Vendor ID 는 PF 와 같고 Device ID 는 capability 의
 * VF Device ID 이므로, 그 (Vendor, Device) 조합으로 시스템 전체 PCI 장치를
 * 훑는다. 같은 ID 를 가진 다른 PF 의 VF 도 걸리므로, physfn 이 정말 이 PF 인지
 * 다시 확인한다. 마지막으로 PCI_DEV_FLAGS_ASSIGNED 플래그(vfio 등이 세운다)로
 * 게스트 배정 여부를 판정한다.
 *
 * 참조 카운트 규약: pci_get_device() 는 찾은 장치의 참조를 올려서 주고,
 * 다음 호출에 이전 장치를 넘기면 그 참조를 대신 놓아 준다. 그래서 루프 안에서
 * 따로 pci_dev_put 을 부르지 않아도 균형이 맞는다.
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_get_device 가 내부에서 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_sriov_configure_simple() -> [pci_vfs_assigned] -> pci_get_device()
 */
int pci_vfs_assigned(struct pci_dev *dev)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	struct pci_dev *vfdev;	/* [한국어] 순회하며 검사할 VF 후보 */
	unsigned int vfs_assigned = 0;	/* [한국어] 배정된 VF 개수 누적 */
	unsigned short dev_id;	/* [한국어] 찾을 VF 의 Device ID */

	/* only search if we are a PF */
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 소유한 VF 자체가 없다 */
		return 0;	/* [한국어] 0 을 돌려준다 */

	/*
	 * determine the device ID for the VFs, the vendor ID will be the
	 * same as the PF so there is no need to check for that one
	 */
	dev_id = dev->sriov->vf_device;	/* [한국어] VF 의 Device ID 는 capability 의 VF Device ID 다. Vendor ID 는 PF 와 같으므로 따로 구할 필요가 없다 */

	/* loop through all the VFs to see if we own any that are assigned */
	vfdev = pci_get_device(dev->vendor, dev_id, NULL);	/* [한국어] (PF 의 Vendor ID, VF Device ID) 조합으로 첫 장치를 찾는다. 세 번째 인자 NULL 은 처음부터 찾으라는 뜻 */
	while (vfdev) {	/* [한국어] 더 이상 찾을 장치가 없을 때까지 */
		/*
		 * It is considered assigned if it is a virtual function with
		 * our dev as the physical function and the assigned bit is set
		 */
		if (vfdev->is_virtfn && (vfdev->physfn == dev) &&	/* [한국어] 진짜 VF 이고, 그 physfn 이 바로 이 PF 이며 */
			pci_is_dev_assigned(vfdev))	/* [한국어] PCI_DEV_FLAGS_ASSIGNED 가 세워져 있으면(vfio 가 게스트에 넘길 때 세운다) */
			vfs_assigned++;	/* [한국어] 배정된 것으로 센다 */

		vfdev = pci_get_device(dev->vendor, dev_id, vfdev);	/* [한국어] 다음 장치를 찾는다. 이전 장치를 넘기면 그 참조를 대신 놓아 주므로 누수가 없다 */
	}

	return vfs_assigned;	/* [한국어] 게스트에 넘어간 VF 개수를 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_vfs_assigned);	/* [한국어] PF 드라이버 모듈용 심볼 공개 */

/**
 * pci_sriov_set_totalvfs -- reduce the TotalVFs available
 * @dev: the PCI PF device
 * @numvfs: number that should be used for TotalVFs supported
 *
 * Should be called from PF driver's probe routine with
 * device's mutex held.
 *
 * Returns 0 if PF is an SRIOV-capable device and
 * value of numvfs valid. If not a PF return -ENOSYS;
 * if numvfs is invalid return -EINVAL;
 * if VFs already enabled, return -EBUSY.
 */
/*
 * [한국어]
 * pci_sriov_set_totalvfs - PF 드라이버가 VF 상한을 하드웨어 값보다 낮게 제한한다
 *
 * @dev: PF 의 pci_dev.
 * @numvfs: 새 상한. 하드웨어 TotalVFs 이하여야 한다.
 * @return: 0 성공, -ENOSYS PF 가 아님, -EINVAL 상한 초과,
 *          -EBUSY 이미 VF 가 켜져 있음.
 *
 * 왜 필요한가: 하드웨어는 VF 를 예를 들어 128개까지 지원하더라도, 드라이버가
 * 확보할 수 있는 자원(큐, 인터럽트, 펌웨어 컨텍스트)이 그보다 적을 수 있다.
 * 그러면 사용자가 128을 요청했을 때 실패하는 대신, 애초에 상한을 낮춰
 * sriov_totalvfs 에 낮춘 값이 보이게 하는 편이 낫다. 이 함수가 그 통로다.
 * 값은 iov->driver_max_VFs 에 들어가고 pci_sriov_get_totalvfs() 가 그것을 읽는다.
 * 드라이버가 언바인드되면 pci_iov_remove() 가 total_VFs 로 되돌린다.
 * 왜 VF 가 켜진 상태에서는 안 되는가: 이미 켜진 VF 수보다 낮은 상한을 걸면
 * 앞뒤가 맞지 않는 상태가 된다. 그래서 VF Enable 이 서 있으면 -EBUSY 로 거절한다.
 * 실행 컨텍스트: 원문 영어 주석대로 PF 드라이버의 probe 안에서
 * device_lock 을 쥔 채 부르는 것이 전제다. 그래서 자체 락이 없다.
 *
 * 호출 체인:
 *   PF 드라이버의 probe -> [pci_sriov_set_totalvfs]
 */
int pci_sriov_set_totalvfs(struct pci_dev *dev, u16 numvfs)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 제한할 대상이 없다 */
		return -ENOSYS;	/* [한국어] 이 장치는 SR-IOV 를 지원하지 않는다는 뜻 */

	if (numvfs > dev->sriov->total_VFs)	/* [한국어] 하드웨어 상한(TotalVFs)보다 큰 값은 의미가 없다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	/* Shouldn't change if VFs already enabled */
	if (dev->sriov->ctrl & PCI_SRIOV_CTRL_VFE)	/* [한국어] SR-IOV Control 사본의 VF Enable(bit0)이 서 있으면 이미 VF 가 돌고 있다 */
		return -EBUSY;	/* [한국어] 사용 중이므로 바꿀 수 없다 */

	dev->sriov->driver_max_VFs = numvfs;	/* [한국어] 드라이버가 정한 상한을 기록. 이후 sriov_totalvfs 와 sriov_numvfs 검사가 이 값을 쓴다 */
	return 0;	/* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(pci_sriov_set_totalvfs);	/* [한국어] PF 드라이버 모듈용 심볼 공개 */

/**
 * pci_sriov_get_totalvfs -- get total VFs supported on this device
 * @dev: the PCI PF device
 *
 * For a PCIe device with SRIOV support, return the PCIe
 * SRIOV capability value of TotalVFs or the value of driver_max_VFs
 * if the driver reduced it.  Otherwise 0.
 */
/*
 * [한국어]
 * pci_sriov_get_totalvfs - 이 PF 에 만들 수 있는 VF 의 최대 개수를 돌려준다
 *
 * @dev: PF 의 pci_dev.
 * @return: 상한값. PF 가 아니면 0.
 *
 * 왜 필요한가: 하드웨어의 TotalVFs 를 그대로 주는 것이 아니라, PF 드라이버가
 * pci_sriov_set_totalvfs() 로 낮췄다면 그 값을 준다. 즉 "지금 실제로 요청할 수
 * 있는 최대치" 다. sysfs 의 sriov_totalvfs 와 sriov_numvfs_store 의 범위 검사가
 * 모두 이 함수를 쓴다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 단일 정수 읽기라 락 없음.
 *
 * 호출 체인:
 *   sriov_totalvfs_show(), sriov_numvfs_store(), PF 드라이버 -> [pci_sriov_get_totalvfs]
 */
int pci_sriov_get_totalvfs(struct pci_dev *dev)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	if (!dev->is_physfn)	/* [한국어] PF 가 아니면 VF 를 만들 수 없다 */
		return 0;	/* [한국어] 상한 0 */

	return dev->sriov->driver_max_VFs;	/* [한국어] sriov_init 이 TotalVFs 로 초기화하고, 드라이버가 낮췄으면 그 값이 들어 있다 */
}
EXPORT_SYMBOL_GPL(pci_sriov_get_totalvfs);	/* [한국어] PF 드라이버 모듈용 심볼 공개 */

/**
 * pci_sriov_configure_simple - helper to configure SR-IOV
 * @dev: the PCI device
 * @nr_virtfn: number of virtual functions to enable, 0 to disable
 *
 * Enable or disable SR-IOV for devices that don't require any PF setup
 * before enabling SR-IOV.  Return value is negative on error, or number of
 * VFs allocated on success.
 */
/*
 * [한국어]
 * pci_sriov_configure_simple - PF 쪽 준비가 필요 없는 드라이버를 위한 기본 SR-IOV 설정 함수
 *
 * @dev: PF 의 pci_dev.
 * @nr_virtfn: 켤 VF 개수. 0 이면 전부 끈다.
 * @return: 성공 시 켠 VF 개수(끄는 경우 0), 실패 시 음수
 *          (-ENODEV PF 가 아님, -EPERM VF 가 게스트에 배정되어 있음,
 *           그 밖에는 sriov_enable() 의 오류).
 *          호출자 sriov_numvfs_store() 는 이 반환값이 요청 개수와 다르면 경고한다.
 *
 * 왜 필요한가: struct pci_driver 의 .sriov_configure 는 함수 포인터라 드라이버마다
 * 다른 준비 작업을 넣을 수 있게 되어 있다. 그런데 많은 장치는 VF 를 켜기 전에
 * 아무 준비도 필요 없다. 그런 드라이버가 똑같은 코드를 복사하지 않도록 PCI 코어가
 * 제공하는 기본 구현이 이 함수다. 하는 일은 안전 검사 세 가지와
 * sriov_enable()/sriov_disable() 호출이 전부다.
 *
 * NVMe 접점: drivers/nvme/host/pci.c 의 nvme_driver 가
 *   .sriov_configure = pci_sriov_configure_simple
 * 로 바로 이 함수를 등록한다(grep 으로 확인). 즉 NVMe 호스트 드라이버는
 * SR-IOV 설정을 커널 공통 구현에 그대로 위임하며, VF 를 켜기 전후로
 * NVMe 고유의 처리(큐 재배분, 관리 커맨드 전송 등)를 전혀 하지 않는다.
 * 사용자가 NVMe PF 의 sriov_numvfs 에 값을 쓰면 이 함수를 거쳐 sriov_enable() 이
 * 돌고, 만들어진 VF 는 보통의 PCI 장치로 열거되어 매칭되는 드라이버가 probe 한다.
 *
 * 왜 pci_vfs_assigned 검사가 필요한가: VF 가 이미 게스트에 넘어간 상태에서
 * 개수를 바꾸면 게스트가 쓰던 장치가 사라져 게스트가 죽는다. 그래서
 * 하나라도 배정되어 있으면 -EPERM 으로 거절한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자(sriov_numvfs_store)가
 * device_lock 과 pci_lock_rescan_remove 를 쥔 상태다. might_sleep() 이
 * 잠들 수 있음을 명시한다.
 *
 * 호출 체인:
 *   sriov_numvfs_store() -> pdev->driver->sriov_configure()
 *     -> [pci_sriov_configure_simple] -> sriov_enable() 또는 sriov_disable()
 */
int pci_sriov_configure_simple(struct pci_dev *dev, int nr_virtfn)	/* [한국어] include/linux/pci.h 에 선언된 공개 API. 드라이버는 이 함수 이름을 .sriov_configure 에 그대로 대입한다 */
{
	int rc;	/* [한국어] sriov_enable 의 반환값을 담을 변수 */

	might_sleep();	/* [한국어] 안에서 msleep/ssleep 으로 잠들므로 아토믹 문맥 호출을 디버그 커널이 잡아내게 한다 */

	if (!dev->is_physfn)	/* [한국어] SR-IOV capability 초기화에 성공하지 못한 장치라면 */
		return -ENODEV;	/* [한국어] 해당 장치가 없다는 뜻으로 -ENODEV */

	if (pci_vfs_assigned(dev)) {	/* [한국어] VF 가 하나라도 게스트(가상 머신)에 배정되어 있으면 */
		pci_warn(dev, "Cannot modify SR-IOV while VFs are assigned\n");	/* [한국어] 그 게스트의 장치를 빼앗게 되므로 안 된다고 알린다 */
		return -EPERM;	/* [한국어] 권한 없음(-EPERM)으로 거절한다 */
	}

	if (nr_virtfn == 0) {	/* [한국어] 0 을 요청했으면 전부 끄라는 뜻 */
		sriov_disable(dev);	/* [한국어] VF pci_dev 를 지우고 하드웨어의 VF Enable 을 끈다 */
		return 0;	/* [한국어] 끄기는 실패하지 않으므로 항상 0(켠 VF 수 0)을 돌려준다 */
	}

	rc = sriov_enable(dev, nr_virtfn);	/* [한국어] 요청한 개수만큼 VF 를 켠다 */
	if (rc < 0)	/* [한국어] 실패하면 */
		return rc;	/* [한국어] 오류 코드를 그대로 올려 보낸다 */

	return nr_virtfn;	/* [한국어] 성공 시 규약대로 "실제로 켠 VF 개수" 를 돌려준다. 이 구현은 요청 수 전부를 켜므로 nr_virtfn 이 그대로 답이다 */
}
EXPORT_SYMBOL_GPL(pci_sriov_configure_simple);	/* [한국어] NVMe 를 비롯한 여러 드라이버가 모듈이므로 심볼을 공개해야 한다 */

/**
 * pci_iov_vf_bar_set_size - set a new size for a VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 * @size: new size as defined in the spec (0=1MB, 31=128TB)
 *
 * Set the new size of a VF BAR that supports VF resizable BAR capability.
 * Unlike pci_resize_resource(), this does not cause the resource that
 * reserves the MMIO space (originally up to total_VFs) to be resized, which
 * means that following calls to pci_enable_sriov() can fail if the resources
 * no longer fit.
 *
 * Return: 0 on success, or negative on failure.
 */
/*
 * [한국어]
 * pci_iov_vf_bar_set_size - VF Resizable BAR 로 VF BAR 하나의 크기를 바꾼다
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF 의 resource[] 인덱스(VF BAR 구간이어야 한다).
 * @size: PCIe 가 정한 크기 인코딩(0 = 1MB, 31 = 128TB).
 * @return: 0 성공, -EINVAL IOV 리소스가 아니거나 지원하지 않는 크기,
 *          -EBUSY VF 메모리 디코딩이 켜져 있음, 그 밖에는 pci_rebar_set_size 의 오류.
 *
 * 왜 필요한가: VF BAR 의 크기는 원래 하드웨어가 고정해 놓는 값이지만,
 * VF Resizable BAR capability 가 있으면 소프트웨어가 바꿀 수 있다. VF 수와
 * VF 당 메모리를 맞바꾸는 조정에 쓴다 — 예를 들어 VF 를 많이 만들려면
 * VF 당 BAR 를 줄여야 한다.
 * 원문 영어 주석의 중요한 경고: 이 함수는 pci_resize_resource() 와 달리
 * PF 가 잡아 둔 MMIO 예약(원래 TotalVFs 배 크기) 자체를 다시 잡지 않는다.
 * 그래서 크기를 키우면 나중에 pci_enable_sriov() 가 "MMIO 가 모자란다"며
 * 실패할 수 있다(sriov_enable 의 nres 검사가 그것을 잡아낸다).
 * 안전 검사 순서: (1) 정말 VF BAR 인가, (2) 지금 VF 들이 그 BAR 로 주소를
 * 디코딩 중은 아닌가, (3) 하드웨어가 그 크기를 지원하는가.
 * 실행 컨텍스트: 프로세스 컨텍스트. 커널 트리 안에서는
 * drivers/gpu/drm/xe 가 이 API 를 쓴다(참조 커널 트리에서 grep 으로 확인).
 * 이 트리에는 그 드라이버가 없다.
 *
 * 호출 체인:
 *   PF 드라이버 -> [pci_iov_vf_bar_set_size]
 *     -> pci_iov_is_memory_decoding_enabled(), pci_rebar_size_supported(),
 *        pci_rebar_set_size() -> (내부에서) pci_iov_resource_set_size()
 */
int pci_iov_vf_bar_set_size(struct pci_dev *dev, int resno, int size)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	if (!pci_resource_is_iov(resno))	/* [한국어] resno 가 PCI_IOV_RESOURCES ~ PCI_IOV_RESOURCE_END 밖이면 VF BAR 가 아니다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	if (pci_iov_is_memory_decoding_enabled(dev))	/* [한국어] SR-IOV Control 의 MSE 가 켜져 있으면 VF 들이 이 BAR 로 주소를 디코딩 중이다 */
		return -EBUSY;	/* [한국어] 디코딩 중에 크기를 바꾸면 진행 중인 접근이 깨지므로 -EBUSY */

	if (!pci_rebar_size_supported(dev, resno, size))	/* [한국어] ReBAR Capability 레지스터의 지원 크기 비트맵에 이 크기가 있는지 확인한다 */
		return -EINVAL;	/* [한국어] 하드웨어가 지원하지 않는 크기는 거절 */

	return pci_rebar_set_size(dev, resno, size);	/* [한국어] 실제 ReBAR Control 레지스터를 쓰고, 내부에서 pci_iov_resource_set_size 로 barsz 캐시까지 갱신한다 */
}
EXPORT_SYMBOL_GPL(pci_iov_vf_bar_set_size);	/* [한국어] 외부 드라이버 모듈용 심볼 공개 */

/**
 * pci_iov_vf_bar_get_sizes - get VF BAR sizes allowing to create up to num_vfs
 * @dev: the PCI device
 * @resno: the resource number
 * @num_vfs: number of VFs
 *
 * Get the sizes of a VF resizable BAR that can accommodate @num_vfs within
 * the currently assigned size of the resource @resno.
 *
 * Return: A bitmask of sizes in format defined in the spec (bit 0=1MB,
 * bit 31=128TB).
 */
/*
 * [한국어]
 * pci_iov_vf_bar_get_sizes - num_vfs 개의 VF 를 담을 수 있는 VF BAR 크기 후보들을 구한다
 *
 * @dev: PF 의 pci_dev.
 * @resno: PF 의 resource[] 인덱스(VF BAR 구간).
 * @num_vfs: 만들고 싶은 VF 개수.
 * @return: 쓸 수 있는 크기들의 비트마스크(bit 0 = 1MB, bit 31 = 128TB).
 *          num_vfs 가 0 이면 0. 호출자는 보통 __fls() 로 가장 큰 후보를 고른다.
 *
 * 왜 필요한가: pci_iov_vf_bar_set_size() 로 크기를 바꾸기 전에 "지금 확보된
 * MMIO 안에서 VF 를 num_vfs 개 만들려면 VF 당 최대 몇 바이트까지 쓸 수 있나"를
 * 알아야 한다. 이미 배정된 resource 길이를 VF 수로 나누면 그 상한이 나온다.
 *
 * 계산 설명:
 *   vf_len = 현재 배정된 전체 길이 / num_vfs   -> VF 하나가 쓸 수 있는 최대 바이트
 *   roundup_pow_of_two(vf_len + 1) - 1        -> vf_len 이하의 모든 2의 거듭제곱
 *                                                자리에 1 이 서 있는 마스크
 *   >> ilog2(SZ_1M)                            -> 1MB 를 bit 0 으로 삼는 스펙 인코딩으로 이동
 * 예: vf_len 이 4MB 면 4M+1 을 올림해 8M, -1 하면 하위 23비트가 모두 1,
 * 20비트 시프트하면 0b111 즉 1MB/2MB/4MB 세 후보가 남는다.
 * 마지막으로 하드웨어가 실제 지원하는 크기 집합과 AND 해서 교집합을 낸다.
 * do_div 를 쓰는 이유: vf_len 이 u64 라 32비트 아키텍처에서 나눗셈 연산자를
 * 그대로 쓸 수 없기 때문이다(링크 오류가 난다).
 * 실행 컨텍스트: 프로세스 컨텍스트. config space 읽기(지원 크기 조회)를 포함한다.
 *
 * 호출 체인:
 *   PF 드라이버 -> [pci_iov_vf_bar_get_sizes] -> pci_rebar_get_possible_sizes()
 */
u32 pci_iov_vf_bar_get_sizes(struct pci_dev *dev, int resno, int num_vfs)	/* [한국어] include/linux/pci.h 에 선언된 공개 API */
{
	u64 vf_len = pci_resource_len(dev, resno);	/* [한국어] 현재 이 VF BAR 영역에 배정된 전체 길이(= VF BAR 크기 x TotalVFs 로 잡아 둔 값) */
	u64 sizes;	/* [한국어] 결과 비트마스크를 담을 변수 */

	if (!num_vfs)	/* [한국어] VF 를 0 개 만들겠다는 것은 나눗셈이 불가능하다 */
		return 0;	/* [한국어] 후보가 없다는 뜻으로 0 을 돌려준다 */

	do_div(vf_len, num_vfs);	/* [한국어] 전체 길이를 VF 수로 나눠 VF 하나가 쓸 수 있는 최대 바이트를 구한다. 결과는 vf_len 에 남는다 */
	sizes = (roundup_pow_of_two(vf_len + 1) - 1) >> ilog2(SZ_1M);	/* [한국어] vf_len 이하의 2의 거듭제곱들을 모두 1 로 만든 마스크를 만든 뒤, 1MB 를 bit 0 으로 삼는 스펙 인코딩으로 20비트 시프트한다 */

	return sizes & pci_rebar_get_possible_sizes(dev, resno);	/* [한국어] 하드웨어가 실제로 지원하는 크기 집합과 교집합을 내어 돌려준다 */
}
EXPORT_SYMBOL_GPL(pci_iov_vf_bar_get_sizes);	/* [한국어] 외부 드라이버 모듈용 심볼 공개 */
