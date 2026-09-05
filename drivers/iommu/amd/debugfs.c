// SPDX-License-Identifier: GPL-2.0
/*
 * AMD IOMMU driver
 *
 * Copyright (C) 2018 Advanced Micro Devices, Inc.
 *
 * Author: Gary R Hook <gary.hook@amd.com>
 */

/*
 * [한국어 설명] AMD IOMMU 의 하드웨어 상태를 들여다보는 debugfs (debugfs.c)
 *
 * === 파일의 역할 ===
 * 레지스터, 명령 버퍼, 장치 테이블(DTE), 인터럽트 재매핑 표를 사람이 읽을
 * 수 있게 노출한다. Intel 쪽 debugfs.c 와 목적은 같지만 인터페이스의 모양이
 * 다르다.
 *
 * 가장 큰 차이는 "먼저 대상을 지정한 뒤 읽는다"는 두 단계 방식이다.
 * devid 파일에 장치를 써 넣으면 그것이 전역 sbdf 에 기록되고, devtbl 과
 * irqtbl 은 그 장치의 상태만 보여 준다. mmio 와 capability 도 같은 방식으로
 * 오프셋을 먼저 쓰고 읽는다.
 *
 * 왜 그렇게 했는가: 장치 테이블은 세그먼트마다 65536개 항목이고 재매핑
 * 표는 장치마다 따로 있다. 전부 찍으면 수십만 줄이 나오므로, 보고 싶은
 * 것을 먼저 고르게 한 것이다.
 *
 * 그 대가로 상태가 전역 변수 하나(sbdf)에 담긴다 — 두 사용자가 동시에
 * 쓰면 서로의 선택을 덮어쓴다. 진단 도구라 그 정도는 감수한 설계이며,
 * 읽는 쪽이 sbdf_shadow 로 한 번 복사해 두는 것이 그나마의 방어다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버의 곁가지다. mmio/capability 파일이 레지스터를 읽는 것 외에는
 * 아무것도 바꾸지 않는다. 실행 컨텍스트는 사용자가 파일을 읽고 쓸 때의
 * 프로세스 문맥이다.
 *
 * 호출 체인:
 *   amd_iommu_init → amd_iommu_debugfs_setup() (파일 생성)
 *   사용자의 read/write → 각 show/write 콜백 → dump_dte()/dump_irte()
 *
 * === 타 모듈과의 연결 ===
 * amd_iommu.h 의 get_dev_table()/search_dev_data(), amd_iommu_types.h 의
 * DTE·IRTE 형식, 그리고 ../irq_remapping.h 의 irq_remapping_enabled.
 *
 * 데이터 흐름은 한 방향이다: 하드웨어와 메모리의 표 → seq_file → 사용자.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_mmio_write/show(): 오프셋을 지정해 임의의 MMIO 레지스터를 읽는다.
 * - iommu_capability_write/show(): 같은 방식으로 PCI 능력 공간을 읽는다.
 * - iommu_cmdbuf_show(): 명령 버퍼 전체와 머리/꼬리 위치를 덤프한다.
 * - devid_write/show(): 이후 조회의 대상 장치를 지정한다.
 * - dump_dte(): 그 장치의 장치 테이블 항목 256비트를 그대로 찍는다.
 * - dump_irte()/dump_128_irte()/dump_32_irte(): 재매핑 표를 형식에 맞게 덤프한다.
 * - amd_iommu_debugfs_setup(): 유닛별 디렉터리와 전역 파일을 만든다.
 */
#include <linux/debugfs.h>	/* [한국어] debugfs 파일 생성 */
#include <linux/pci.h>	/* [한국어] PCI 설정 공간 접근과 BDF 매크로 */

#include "amd_iommu.h"	/* [한국어] get_dev_table 등 드라이버 내부 함수 */
#include "../irq_remapping.h"	/* [한국어] irq_remapping_enabled — 재매핑이 실제로 켜져 있는지 */

static struct dentry *amd_iommu_debugfs;	/* [한국어] 이 드라이버의 debugfs 디렉터리. 모든 파일이 그 아래 만들어진다 */

#define	MAX_NAME_LEN	20	/* [한국어] 유닛 디렉터리 이름의 최대 길이 */
#define	OFS_IN_SZ	8	/* [한국어] 오프셋 입력의 최대 길이. 16진수 문자열 하나면 충분하다 */
#define	DEVID_IN_SZ	16	/* [한국어] 장치 지정 문자열의 최대 길이("0000:00:14.0"보다 넉넉하게) */

static int sbdf = -1;	/* [한국어] 지금 조회 대상인 장치. -1 은 "지정되지 않음"이다. devtbl/irqtbl 이 이 값을 본다 */

/*
 * [한국어]
 * iommu_mmio_write - 다음에 읽을 MMIO 레지스터의 오프셋을 지정한다
 *
 * @filp: 열린 파일.
 * @ubuf: 사용자가 쓴 오프셋 문자열.
 * @cnt: 그 길이.
 * @ppos: 파일 오프셋(쓰지 않는다).
 * @return: 받아들인 바이트 수, 또는 -EINVAL.
 *
 * 첫 줄의 이중 대입이 이 함수의 요령이다:
 *   int ret, dbg_mmio_offset = iommu->dbg_mmio_offset = -1;
 * 저장된 오프셋을 먼저 무효(-1)로 만들어 둔다. 그래서 이후 어느 단계에서
 * 실패하고 돌아가도 show 쪽은 "지정되지 않음"을 보게 되며, 옛 오프셋이
 * 남아 사용자를 헷갈리게 하지 않는다.
 *
 * 상한 검사가 mmio_phys_end - sizeof(u64) 인 이유: show 가 readq 로 8바이트를
 * 읽으므로, 끝에서 8바이트 안쪽이어야 영역을 넘지 않는다.
 *
 * 호출 체인:
 *   write(2) → [이 함수]
 */
static ssize_t iommu_mmio_write(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	struct seq_file *m = filp->private_data;	/* [한국어] seq_file 을 거쳐 유닛에 닿는다 */
	struct amd_iommu *iommu = m->private;	/* [한국어] 대상 유닛 */
	int ret, dbg_mmio_offset = iommu->dbg_mmio_offset = -1;	/* [한국어] 저장된 오프셋을 먼저 무효로 만든다 — 실패해도 옛 값이 남지 않게 */

	if (cnt > OFS_IN_SZ)	/* [한국어] 오프셋 하나보다 긴 입력은 무의미하다 */
		return -EINVAL;	/* [한국어] 거절 */

	ret = kstrtou32_from_user(ubuf, cnt, 0, &dbg_mmio_offset);	/* [한국어] 0 진법 = 0x 접두사를 알아서 해석한다 */
	if (ret)	/* [한국어] 숫자가 아니다 */
		return ret;	/* [한국어] 파싱 오류를 그대로 전달 */

	if (dbg_mmio_offset > iommu->mmio_phys_end - sizeof(u64))	/* [한국어] show 가 8바이트를 읽으므로 끝에서 8바이트 안쪽이어야 한다 */
		return -EINVAL;	/* [한국어] 영역을 넘는 접근을 막는다 */

	iommu->dbg_mmio_offset = dbg_mmio_offset;	/* [한국어] 검증을 통과한 값만 저장한다 */
	return cnt;	/* [한국어] 전부 받아들였다 */
}

/*
 * [한국어]
 * iommu_mmio_show - 지정된 오프셋의 MMIO 레지스터를 읽어 보여 준다
 *
 * @m: 출력 대상. private 에 유닛이 들어 있다.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * 범위를 다시 검사하는 이유: write 와 show 사이에 상태가 어긋날 수 있고,
 * 무엇보다 아직 아무것도 쓰지 않은 상태(-1)를 여기서 걸러 안내 메시지를
 * 내야 한다.
 *
 * 오류를 반환하지 않고 안내 문구를 출력하는 것이 debugfs 의 관례다 —
 * cat 이 오류로 끝나는 것보다 무엇을 해야 하는지 알려 주는 편이 낫다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → readq()
 */
static int iommu_mmio_show(struct seq_file *m, void *unused)
{
	struct amd_iommu *iommu = m->private;	/* [한국어] 대상 유닛 */
	u64 value;	/* [한국어] 읽어 온 레지스터 값 */
	int dbg_mmio_offset = iommu->dbg_mmio_offset;	/* [한국어] 지정된 오프셋 */

	if (dbg_mmio_offset < 0 || dbg_mmio_offset >	/* [한국어] 아직 지정하지 않았거나(-1) 범위를 벗어났다 */
			iommu->mmio_phys_end - sizeof(u64)) {	/* [한국어] write 이후 상태가 어긋났을 수도 있어 다시 본다 */
		seq_puts(m, "Please provide mmio register's offset\n");	/* [한국어] 오류 대신 무엇을 해야 하는지 알린다 — debugfs 의 관례다 */
		return 0;	/* [한국어] cat 이 실패로 끝나지 않게 */
	}

	value = readq(iommu->mmio_base + dbg_mmio_offset);	/* [한국어] 8바이트를 한 번에 읽는다 */
	seq_printf(m, "Offset:0x%x Value:0x%016llx\n", dbg_mmio_offset, value);	/* [한국어] 오프셋과 값을 함께 — 무엇을 읽은 것인지 분명해진다 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_STORE_ATTRIBUTE(iommu_mmio);	/* [한국어] 위 show/write 두 함수로 읽기+쓰기 file_operations 를 자동 생성한다 */

/*
 * [한국어]
 * iommu_capability_write - 다음에 읽을 PCI 능력 레지스터의 오프셋을 지정한다
 *
 * @filp: 열린 파일.
 * @ubuf: 사용자가 쓴 오프셋.
 * @cnt: 길이.
 * @ppos: 쓰지 않는다.
 * @return: 받아들인 바이트 수, 또는 -EINVAL.
 *
 * iommu_mmio_write 와 같은 구조이고 대상만 다르다. MMIO 가 아니라 PCI
 * 설정 공간의 IOMMU 능력 구조를 읽는다.
 *
 * 상한이 0x14 인 이유는 원 주석이 밝힌다 — 그것이 마지막 IOMMU 능력
 * 레지스터의 오프셋이다. 그 너머는 다른 능력 구조나 무의미한 값이다.
 *
 * 호출 체인:
 *   write(2) → [이 함수]
 */
static ssize_t iommu_capability_write(struct file *filp, const char __user *ubuf,
				      size_t cnt, loff_t *ppos)
{
	struct seq_file *m = filp->private_data;	/* [한국어] seq_file 을 거쳐 */
	struct amd_iommu *iommu = m->private;	/* [한국어] 대상 유닛 */
	int ret, dbg_cap_offset = iommu->dbg_cap_offset = -1;	/* [한국어] 같은 요령으로 먼저 무효화한다 */

	if (cnt > OFS_IN_SZ)	/* [한국어] 너무 긴 입력 */
		return -EINVAL;	/* [한국어] 거절 */

	ret = kstrtou32_from_user(ubuf, cnt, 0, &dbg_cap_offset);	/* [한국어] 문자열을 숫자로 */
	if (ret)	/* [한국어] 파싱 실패 */
		return ret;	/* [한국어] 그대로 전달 */

	/* Capability register at offset 0x14 is the last IOMMU capability register. */
	if (dbg_cap_offset > 0x14)	/* [한국어] (원 주석: 0x14 가 마지막 IOMMU 능력 레지스터다) */
		return -EINVAL;	/* [한국어] 그 너머는 다른 능력 구조라 의미가 없다 */

	iommu->dbg_cap_offset = dbg_cap_offset;	/* [한국어] 저장 */
	return cnt;	/* [한국어] 받아들였다 */
}

/*
 * [한국어]
 * iommu_capability_show - 지정된 PCI 능력 레지스터를 읽어 보여 준다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * cap_ptr 을 더하는 것이 핵심이다. 능력 구조는 설정 공간 안 어디에나 놓일
 * 수 있고 그 위치가 유닛마다 다르므로, 사용자가 준 오프셋은 능력 구조
 * 내부의 상대 위치로 해석한다.
 *
 * 읽기 실패도 오류로 반환하지 않고 문구로 알린다 — 진단 도구가 다른
 * 이유로 실패하는 것을 사용자가 구별할 수 있어야 하기 때문이다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → pci_read_config_dword()
 */
static int iommu_capability_show(struct seq_file *m, void *unused)
{
	struct amd_iommu *iommu = m->private;	/* [한국어] 대상 유닛 */
	u32 value;	/* [한국어] 읽어 온 값 */
	int err, dbg_cap_offset = iommu->dbg_cap_offset;	/* [한국어] 읽기 결과와 지정된 오프셋 */

	if (dbg_cap_offset < 0 || dbg_cap_offset > 0x14) {	/* [한국어] 지정되지 않았거나 범위를 벗어남 */
		seq_puts(m, "Please provide capability register's offset in the range [0x00 - 0x14]\n");	/* [한국어] 유효 범위까지 함께 알려 준다 */
		return 0;	/* [한국어] 안내만 하고 끝 */
	}

	err = pci_read_config_dword(iommu->dev, iommu->cap_ptr + dbg_cap_offset, &value);	/* [한국어] 능력 구조의 위치는 유닛마다 다르므로 cap_ptr 을 더한다 */
	if (err) {	/* [한국어] 설정 공간 읽기 실패 */
		seq_printf(m, "Not able to read capability register at 0x%x\n",	/* [한국어] 다른 실패와 구별할 수 있게 알린다 */
			   dbg_cap_offset);
		return 0;	/* [한국어] 오류로 반환하지 않는다 */
	}

	seq_printf(m, "Offset:0x%x Value:0x%08x\n", dbg_cap_offset, value);	/* [한국어] 오프셋과 값 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_STORE_ATTRIBUTE(iommu_capability);	/* [한국어] 같은 방식 */

/*
 * [한국어]
 * iommu_cmdbuf_show - 명령 버퍼 전체와 머리/꼬리 위치를 덤프한다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * IOMMU 가 멈춘 것처럼 보일 때 보는 파일이다. 머리(하드웨어가 처리한
 * 지점)와 꼬리(드라이버가 넣은 지점)가 벌어져 있으면 하드웨어가 명령을
 * 소화하지 못하고 있다는 뜻이고, 그것이 unmap 이 걸려 있는 이유인 경우가 많다.
 *
 * 512개 항목을 전부 찍는 이유: 링은 순환하므로 머리 바깥에도 옛 명령이
 * 그대로 남아 있고, 그 잔해가 "언제 무엇이 멈췄는가"를 알려 준다.
 *
 * 포인터를 >> 4 & 0x7fff 로 다루는 이유: 레지스터가 바이트 오프셋으로
 * 표현하므로, 항목 크기(16바이트)로 나눠야 사람이 읽을 수 있는 슬롯
 * 번호가 된다.
 *
 * iommu->lock 을 잡는 이유: 명령을 넣는 경로와 경쟁하면 꼬리 값과 버퍼
 * 내용이 서로 다른 시점의 것이 되어 덤프가 앞뒤가 맞지 않는다.
 *
 * 호출 체인:
 *   read(2) → [이 함수]
 */
static int iommu_cmdbuf_show(struct seq_file *m, void *unused)
{
	struct amd_iommu *iommu = m->private;	/* [한국어] 대상 유닛 */
	struct iommu_cmd *cmd;	/* [한국어] 현재 슬롯의 명령 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 저장용 */
	u32 head, tail;	/* [한국어] 하드웨어가 처리한 지점과 드라이버가 넣은 지점 */
	int i;	/* [한국어] 슬롯 인덱스 */

	raw_spin_lock_irqsave(&iommu->lock, flag);	/* [한국어] 명령을 넣는 경로와 경쟁하면 덤프가 앞뒤가 맞지 않는다 */
	head = readl(iommu->mmio_base + MMIO_CMD_HEAD_OFFSET);	/* [한국어] 머리 포인터 */
	tail = readl(iommu->mmio_base + MMIO_CMD_TAIL_OFFSET);	/* [한국어] 꼬리 포인터. 둘이 벌어져 있으면 하드웨어가 밀려 있다 */
	seq_printf(m, "CMD Buffer Head Offset:%d Tail Offset:%d\n",	/* [한국어] 두 위치를 먼저 알린다 */
		   (head >> 4) & 0x7fff, (tail >> 4) & 0x7fff);	/* [한국어] 바이트 오프셋을 항목 크기(16)로 나눠 슬롯 번호로 바꾼다 */
	for (i = 0; i < CMD_BUFFER_ENTRIES; i++) {	/* [한국어] 512개 전부 — 머리 바깥의 잔해도 진단 정보다 */
		cmd = (struct iommu_cmd *)(iommu->cmd_buf + i * sizeof(*cmd));	/* [한국어] 그 슬롯의 위치 */
		seq_printf(m, "%3d: %08x %08x %08x %08x\n", i, cmd->data[0],	/* [한국어] 네 워드를 원시값으로 */
			   cmd->data[1], cmd->data[2], cmd->data[3]);	/* [한국어] 해석하지 않는 것이 의도다 */
	}
	raw_spin_unlock_irqrestore(&iommu->lock, flag);	/* [한국어] 덤프 완료 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_ATTRIBUTE(iommu_cmdbuf);	/* [한국어] 쓰기가 없으므로 읽기 전용 버전 */

/*
 * [한국어]
 * devid_write - 이후 조회의 대상 장치를 지정한다
 *
 * @filp: 열린 파일.
 * @ubuf: "0000:00:14.0" 또는 "00:14.0" 형식의 문자열.
 * @cnt: 길이.
 * @ppos: 쓰지 않는다.
 * @return: 받아들인 바이트 수, 또는 -EINVAL/-ENODEV.
 *
 * devtbl 과 irqtbl 파일이 무엇을 보여 줄지를 정하는 관문이다. 전역 sbdf 에
 * 결과를 남기므로, 이 파일이 이 디렉터리의 상태를 쥐고 있는 셈이다.
 *
 * 맨 앞에서 sbdf 를 -1 로 되돌리는 이유: 잘못된 입력을 주었을 때 이전
 * 선택이 남아 있으면, 사용자는 자기가 방금 지정한 장치를 보고 있다고
 * 착각한다.
 *
 * 두 가지 형식을 받아들이는 것이 sscanf 두 번의 이유다. 세그먼트를 생략한
 * 짧은 형식은 세그먼트 0 으로 해석한다 — 대부분의 시스템이 세그먼트가
 * 하나뿐이라 그쪽이 훨씬 자주 쓰인다.
 *
 * 세 단계로 검증한다: 세그먼트가 존재하는가, 장치 id 가 그 세그먼트의
 * 범위 안인가, 그 장치를 담당하는 유닛이 있는가. 마지막 하나만 -ENODEV 로
 * 구별하는 것은 "형식은 맞지만 그런 장치가 없다"를 알리기 위해서다.
 *
 * 호출 체인:
 *   write(2) → [이 함수]
 */
static ssize_t devid_write(struct file *filp, const char __user *ubuf,
			   size_t cnt, loff_t *ppos)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트를 훑을 커서 */
	int seg, bus, slot, func;	/* [한국어] 파싱 결과 */
	struct amd_iommu *iommu;	/* [한국어] 담당 유닛(존재 확인용) */
	char *srcid_ptr;	/* [한국어] 사용자 문자열의 커널 사본 */
	u16 devid;	/* [한국어] 조립한 장치 id */
	int i;	/* [한국어] sscanf 가 채운 항목 수 */

	sbdf = -1;	/* [한국어] 잘못된 입력을 주었을 때 이전 선택이 남지 않게 먼저 지운다 */

	if (cnt >= DEVID_IN_SZ)	/* [한국어] "0000:00:14.0"보다 긴 입력은 형식이 아니다 */
		return -EINVAL;	/* [한국어] 거절 */

	srcid_ptr = memdup_user_nul(ubuf, cnt);	/* [한국어] 문자열로 다루기 위해 널 종료까지 붙여 복사한다 */
	if (IS_ERR(srcid_ptr))	/* [한국어] 복사 실패 */
		return PTR_ERR(srcid_ptr);	/* [한국어] 오류를 그대로 */

	i = sscanf(srcid_ptr, "%x:%x:%x.%x", &seg, &bus, &slot, &func);	/* [한국어] 세그먼트를 포함한 긴 형식 */
	if (i != 4) {	/* [한국어] 네 항목이 다 채워지지 않았다면 */
		i = sscanf(srcid_ptr, "%x:%x.%x", &bus, &slot, &func);	/* [한국어] 세그먼트를 생략한 짧은 형식으로 다시 시도 */
		if (i != 3) {	/* [한국어] 그것도 아니면 */
			kfree(srcid_ptr);	/* [한국어] 사본을 놓고 */
			return -EINVAL;	/* [한국어] 형식 오류 */
		}
		seg = 0;	/* [한국어] 대부분의 시스템이 세그먼트 하나뿐이라 그쪽을 기본으로 삼는다 */
	}

	devid = PCI_DEVID(bus, PCI_DEVFN(slot, func));	/* [한국어] 버스/슬롯/기능을 16비트 id 로 */

	/* Check if user device id input is a valid input */
	for_each_pci_segment(pci_seg) {	/* [한국어] (원 주석: 사용자 입력이 유효한지 확인) */
		if (pci_seg->id != seg)	/* [한국어] 다른 세그먼트는 건너뛴다 */
			continue;	/* [한국어] 다음 세그먼트 */
		if (devid > pci_seg->last_bdf) {	/* [한국어] 그 세그먼트에 존재할 수 있는 범위를 넘었다 */
			kfree(srcid_ptr);	/* [한국어] 사본을 놓고 */
			return -EINVAL;	/* [한국어] 거절 */
		}
		iommu = pci_seg->rlookup_table[devid];	/* [한국어] 그 장치를 담당하는 유닛이 있는가 */
		if (!iommu) {	/* [한국어] 없다면 */
			kfree(srcid_ptr);	/* [한국어] 사본을 놓고 */
			return -ENODEV;	/* [한국어] 형식은 맞지만 그런 장치가 없다는 뜻으로 다른 코드를 쓴다 */
		}
		break;	/* [한국어] 찾았으므로 순회 종료 */
	}

	if (pci_seg->id != seg) {	/* [한국어] 목록을 다 돌았는데 일치하는 세그먼트가 없었다 */
		kfree(srcid_ptr);	/* [한국어] 사본을 놓고 */
		return -EINVAL;	/* [한국어] 존재하지 않는 세그먼트 */
	}

	sbdf = PCI_SEG_DEVID_TO_SBDF(seg, devid);	/* [한국어] 이후 devtbl/irqtbl 이 볼 조회 키 */

	kfree(srcid_ptr);	/* [한국어] 사본을 놓는다 */

	return cnt;	/* [한국어] 전부 받아들였다 */
}

/*
 * [한국어]
 * devid_show - 지금 지정된 대상 장치를 보여 준다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * sbdf 를 지역 변수로 복사한 뒤 쓰는 것이 눈에 띈다. 전역이라 읽는 도중
 * 다른 스레드가 바꿀 수 있고, 그러면 세그먼트와 BDF 가 서로 다른 시점의
 * 값으로 조합되어 존재하지 않는 장치를 찍는다. 한 번 복사해 두면 적어도
 * 출력 한 줄 안에서는 일관된다.
 *
 * 호출 체인:
 *   read(2) → [이 함수]
 */
static int devid_show(struct seq_file *m, void *unused)
{
	u16 devid;	/* [한국어] 출력할 장치 id */
	int sbdf_shadow = sbdf;	/* [한국어] 전역을 한 번 복사한다 — 읽는 도중 바뀌면 세그먼트와 BDF 가 다른 시점의 조합이 된다 */

	if (sbdf_shadow >= 0) {	/* [한국어] 지정된 장치가 있다면 */
		devid = PCI_SBDF_TO_DEVID(sbdf_shadow);	/* [한국어] 합친 키에서 BDF 를 꺼내 */
		seq_printf(m, "%04x:%02x:%02x.%x\n", PCI_SBDF_TO_SEGID(sbdf_shadow),	/* [한국어] 사용자가 쓴 것과 같은 형식으로 되돌려 준다 */
			   PCI_BUS_NUM(devid), PCI_SLOT(devid), PCI_FUNC(devid));
	} else
		seq_puts(m, "No or Invalid input provided\n");	/* [한국어] 지정되지 않았거나 검증에 실패했다 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_STORE_ATTRIBUTE(devid);	/* [한국어] 대상 지정용이라 쓰기가 필요하다 */

/*
 * [한국어]
 * dump_dte - 그 장치의 장치 테이블 항목 256비트를 그대로 찍는다
 *
 * @m: 출력 대상.
 * @pci_seg: 그 장치가 속한 세그먼트.
 * @devid: 장치 id.
 *
 * 해석하지 않고 네 워드를 원시값으로 낸다. 이 파일이 쓰이는 상황이
 * "드라이버가 의도한 대로 DTE 가 써졌는가"를 의심할 때이므로, 드라이버의
 * 해석을 거치지 않은 값을 보여야 의미가 있다.
 *
 * QWORD[3]부터 역순으로 찍는 이유: 스펙의 비트 번호가 하위 워드부터
 * 매겨지므로, 왼쪽이 상위 비트가 되도록 놓아야 스펙 그림과 나란히 읽힌다.
 *
 * get_dev_table 을 쓰는 이유: kdump 커널에서는 물려받은 표의 사본을 보게
 * 되므로, 유닛이 실제로 쓰는 표를 그 함수가 돌려준다.
 *
 * 호출 체인:
 *   iommu_devtbl_show() → [이 함수]
 */
static void dump_dte(struct seq_file *m, struct amd_iommu_pci_seg *pci_seg, u16 devid)
{
	struct dev_table_entry *dev_table;	/* [한국어] 그 유닛이 실제로 쓰는 장치 테이블 */
	struct amd_iommu *iommu;	/* [한국어] 담당 유닛 */

	iommu = pci_seg->rlookup_table[devid];	/* [한국어] 장치 id 로 유닛을 찾는다 */
	if (!iommu)	/* [한국어] 담당 유닛이 없는 장치 */
		return;	/* [한국어] 찍을 것이 없다 */

	dev_table = get_dev_table(iommu);	/* [한국어] kdump 에서는 물려받은 사본을 돌려줄 수 있다 */
	if (!dev_table) {	/* [한국어] 표가 없다 */
		seq_puts(m, "Device table not found");	/* [한국어] 그 사실을 알리고 */
		return;	/* [한국어] 끝 */
	}

	seq_printf(m, "%-12s %16s %16s %16s %16s iommu\n", "DeviceId",	/* [한국어] 열 제목 */
		   "QWORD[3]", "QWORD[2]", "QWORD[1]", "QWORD[0]");	/* [한국어] 상위 워드가 왼쪽에 오도록 역순으로 이름 붙인다 */
	seq_printf(m, "%04x:%02x:%02x.%x ", pci_seg->id, PCI_BUS_NUM(devid),	/* [한국어] 어느 장치의 항목인지 */
		   PCI_SLOT(devid), PCI_FUNC(devid));
	for (int i = 3; i >= 0; --i)	/* [한국어] 역순 — 스펙 그림과 나란히 읽히도록 */
		seq_printf(m, "%016llx ", dev_table[devid].data[i]);	/* [한국어] 해석하지 않은 원시값. 드라이버의 해석을 의심할 때 쓰는 도구다 */
	seq_printf(m, "iommu%d\n", iommu->index);	/* [한국어] 어느 유닛의 표인지 */
}

/*
 * [한국어]
 * iommu_devtbl_show - 지정된 장치의 DTE 를 보여 준다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * devid 파일로 대상을 먼저 지정해야 한다는 두 단계 규약이 여기서 드러난다.
 * 지정되지 않았으면 무엇을 해야 하는지 안내한다.
 *
 * 세그먼트를 훑어 일치하는 것을 찾는 이유: sbdf 는 세그먼트+BDF 를 합친
 * 값이라, 세그먼트 구조체를 다시 찾아야 그 안의 표에 닿을 수 있다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → dump_dte()
 */
static int iommu_devtbl_show(struct seq_file *m, void *unused)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 커서 */
	u16 seg, devid;	/* [한국어] 대상 세그먼트와 장치 */
	int sbdf_shadow = sbdf;	/* [한국어] 전역을 한 번 복사 */

	if (sbdf_shadow < 0) {	/* [한국어] 대상이 지정되지 않았다 */
		seq_puts(m, "Enter a valid device ID to 'devid' file\n");	/* [한국어] 두 단계 규약을 안내한다 */
		return 0;	/* [한국어] 안내만 */
	}
	seg = PCI_SBDF_TO_SEGID(sbdf_shadow);	/* [한국어] 합친 키에서 세그먼트 */
	devid = PCI_SBDF_TO_DEVID(sbdf_shadow);	/* [한국어] 그리고 장치 id */

	for_each_pci_segment(pci_seg) {	/* [한국어] 세그먼트 구조체를 찾아야 그 안의 표에 닿을 수 있다 */
		if (pci_seg->id != seg)	/* [한국어] 다른 세그먼트 */
			continue;	/* [한국어] 건너뛴다 */
		dump_dte(m, pci_seg, devid);	/* [한국어] 찾았으면 덤프 */
		break;	/* [한국어] 더 볼 필요 없다 */
	}

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_ATTRIBUTE(iommu_devtbl);	/* [한국어] 읽기 전용 — 대상은 devid 파일이 정한다 */

/*
 * [한국어]
 * dump_128_irte - 128비트 형식의 재매핑 항목을 덤프한다
 *
 * @m: 출력 대상.
 * @table: 그 장치의 재매핑 표.
 * @int_tab_len: 항목 수.
 *
 * 유효 비트를 어디서 읽을지가 모드에 따라 달라지는 것이 이 함수의 요점이다.
 * 128비트 항목의 하위 64비트는 guest_mode 에 따라 두 가지로 해석되고, 그
 * 두 해석에서 유효 비트의 위치가 다르다. 그래서 시스템이 vAPIC 모드일 때는
 * fields_vapic 의, 아닐 때는 fields_remap 의 valid 를 본다.
 *
 * 유효하지 않은 항목을 건너뛰는 이유: 표는 항상 꽉 찬 크기로 잡히므로,
 * 전부 찍으면 의미 없는 0 이 수백 줄 나온다.
 *
 * 호출 체인:
 *   dump_irte() → [이 함수]
 */
static void dump_128_irte(struct seq_file *m, struct irq_remap_table *table, u16 int_tab_len)
{
	struct irte_ga *ptr, *irte;	/* [한국어] 128비트 항목 배열과 현재 항목 */
	int index;	/* [한국어] 항목 인덱스 */

	for (index = 0; index < int_tab_len; index++) {	/* [한국어] 표 전체를 훑는다 */
		ptr = (struct irte_ga *)table->table;	/* [한국어] 128비트 형식으로 해석 */
		irte = &ptr[index];	/* [한국어] 현재 항목 */

		if (AMD_IOMMU_GUEST_IR_VAPIC(amd_iommu_guest_ir) &&	/* [한국어] vAPIC 모드에서는 유효 비트가 게스트 쪽 해석에 있다 */
		    !irte->lo.fields_vapic.valid)	/* [한국어] 같은 64비트라도 해석에 따라 유효 비트의 자리가 다르다 */
			continue;	/* [한국어] 쓰이지 않는 항목 */
		else if (!irte->lo.fields_remap.valid)	/* [한국어] 그 밖에는 재매핑 해석의 유효 비트를 본다 */
			continue;	/* [한국어] 건너뛴다 — 전부 찍으면 0 이 수백 줄 나온다 */
		seq_printf(m, "IRT[%04d] %016llx %016llx\n", index, irte->hi.val, irte->lo.val);	/* [한국어] 두 워드를 원시값으로 */
	}
}

/*
 * [한국어]
 * dump_32_irte - 32비트(레거시) 형식의 재매핑 항목을 덤프한다
 *
 * @m: 출력 대상.
 * @table: 재매핑 표.
 * @int_tab_len: 항목 수.
 *
 * 128비트 쪽과 달리 해석이 하나뿐이라 유효 비트의 위치도 하나다. 항목이
 * 32비트이므로 한 워드만 찍는다.
 *
 * 호출 체인:
 *   dump_irte() → [이 함수]
 */
static void dump_32_irte(struct seq_file *m, struct irq_remap_table *table, u16 int_tab_len)
{
	union irte *ptr, *irte;	/* [한국어] 32비트 항목 배열과 현재 항목 */
	int index;	/* [한국어] 항목 인덱스 */

	for (index = 0; index < int_tab_len; index++) {	/* [한국어] 표 전체 */
		ptr = (union irte *)table->table;	/* [한국어] 32비트 형식으로 해석 */
		irte = &ptr[index];	/* [한국어] 현재 항목 */

		if (!irte->fields.valid)	/* [한국어] 해석이 하나뿐이라 유효 비트의 자리도 하나다 */
			continue;	/* [한국어] 쓰이지 않는 항목 */
		seq_printf(m, "IRT[%04d] %08x\n", index, irte->val);	/* [한국어] 한 워드만 찍는다 */
	}
}

/*
 * [한국어]
 * dump_irte - 그 장치의 인터럽트 재매핑 표를 형식에 맞게 덤프한다
 *
 * @m: 출력 대상.
 * @devid: 장치 id.
 * @pci_seg: 그 장치의 세그먼트.
 *
 * AMD 는 재매핑 표가 장치마다 있으므로, 먼저 그 장치의 표를 찾고 DTE 에서
 * 표의 크기를 읽어야 한다. 크기가 DTE 에 인코딩되어 있다는 점이 Intel 과
 * 다른 부분이다.
 *
 * BIT(int_tab_len >> 1) 로 항목 수를 구하는 이유: DTE 의 길이 필드는
 * 값이 1비트 왼쪽으로 밀린 채 저장되어 있고, 그 값 자체가 log2(항목 수)다.
 * 되밀어 지수를 얻은 뒤 2의 거듭제곱을 취한다.
 *
 * 길이가 512도 2048도 아니면 DTE 가 깨졌다는 뜻이라, 그 값을 믿고 순회하면
 * 표 밖을 읽는다. 그래서 덤프를 포기한다.
 *
 * table->lock 을 잡는 이유: 인터럽트 이동이 항목을 갈아 끼우는 중일 수 있다.
 *
 * 호출 체인:
 *   iommu_irqtbl_show() → [이 함수] → dump_128_irte()/dump_32_irte()
 */
static void dump_irte(struct seq_file *m, u16 devid, struct amd_iommu_pci_seg *pci_seg)
{
	struct dev_table_entry *dev_table;	/* [한국어] 표 크기를 읽을 장치 테이블 */
	struct irq_remap_table *table;	/* [한국어] 그 장치의 재매핑 표 */
	struct amd_iommu *iommu;	/* [한국어] 담당 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	u16 int_tab_len;	/* [한국어] DTE 에 인코딩된 표 길이 */

	table = pci_seg->irq_lookup_table[devid];	/* [한국어] AMD 는 표가 장치마다 있다 */
	if (!table) {	/* [한국어] 이 장치에는 재매핑 표가 없다 */
		seq_printf(m, "IRQ lookup table not set for %04x:%02x:%02x:%x\n",	/* [한국어] 인터럽트를 쓰지 않는 장치일 수 있다 */
			   pci_seg->id, PCI_BUS_NUM(devid), PCI_SLOT(devid), PCI_FUNC(devid));
		return;	/* [한국어] 찍을 것이 없다 */
	}

	iommu = pci_seg->rlookup_table[devid];	/* [한국어] 담당 유닛 */
	if (!iommu)	/* [한국어] 없으면 */
		return;	/* [한국어] 끝 */

	dev_table = get_dev_table(iommu);	/* [한국어] 표 크기가 DTE 에 들어 있다 */
	if (!dev_table) {	/* [한국어] 장치 테이블이 없다 */
		seq_puts(m, "Device table not found");	/* [한국어] 알리고 */
		return;	/* [한국어] 끝 */
	}

	int_tab_len = dev_table[devid].data[2] & DTE_INTTABLEN_MASK;	/* [한국어] DTE 에서 길이 필드를 꺼낸다 */
	if (int_tab_len != DTE_INTTABLEN_512 && int_tab_len != DTE_INTTABLEN_2K) {	/* [한국어] 512도 2048도 아니면 */
		seq_puts(m, "The device's DTE contains an invalid IRT length value.");	/* [한국어] DTE 가 깨졌다는 뜻이다 */
		return;	/* [한국어] 그 값을 믿고 순회하면 표 밖을 읽는다 */
	}

	seq_printf(m, "DeviceId %04x:%02x:%02x.%x\n", pci_seg->id, PCI_BUS_NUM(devid),	/* [한국어] 어느 장치의 표인지 */
		   PCI_SLOT(devid), PCI_FUNC(devid));

	raw_spin_lock_irqsave(&table->lock, flags);	/* [한국어] 인터럽트 이동이 항목을 갈아 끼우는 중일 수 있다 */
	if (AMD_IOMMU_GUEST_IR_GA(amd_iommu_guest_ir))	/* [한국어] 128비트 형식을 쓰는 모드인가 */
		dump_128_irte(m, table, BIT(int_tab_len >> 1));	/* [한국어] 길이 필드는 1비트 밀려 저장되고 그 값이 log2(항목 수)다 */
	else
		dump_32_irte(m, table, BIT(int_tab_len >> 1));	/* [한국어] 32비트 형식도 항목 수 계산은 같다 */
	seq_puts(m, "\n");	/* [한국어] 표 끝에 빈 줄 */
	raw_spin_unlock_irqrestore(&table->lock, flags);	/* [한국어] 덤프 완료 */
}

/*
 * [한국어]
 * iommu_irqtbl_show - 지정된 장치의 재매핑 표를 보여 준다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * irq_remapping_enabled 를 먼저 보는 이유: 재매핑이 꺼져 있으면 표가
 * 있더라도 하드웨어가 참조하지 않으므로 내용이 아무 의미가 없다. 표를
 * 찍어 사용자를 헷갈리게 하느니 그 사실을 알리는 편이 낫다.
 *
 * 나머지는 iommu_devtbl_show 와 같은 구조다 — 대상이 지정됐는지 보고,
 * 세그먼트를 찾아 덤프한다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → dump_irte()
 */
static int iommu_irqtbl_show(struct seq_file *m, void *unused)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 커서 */
	u16 devid, seg;	/* [한국어] 대상 장치와 세그먼트 */
	int sbdf_shadow = sbdf;	/* [한국어] 전역을 한 번 복사 */

	if (!irq_remapping_enabled) {	/* [한국어] 재매핑이 꺼져 있으면 */
		seq_puts(m, "Interrupt remapping is disabled\n");	/* [한국어] 표가 있어도 하드웨어가 참조하지 않아 내용이 무의미하다 */
		return 0;	/* [한국어] 그 사실만 알린다 */
	}

	if (sbdf_shadow < 0) {	/* [한국어] 대상이 지정되지 않았다 */
		seq_puts(m, "Enter a valid device ID to 'devid' file\n");	/* [한국어] 두 단계 규약을 안내 */
		return 0;	/* [한국어] 안내만 */
	}

	seg = PCI_SBDF_TO_SEGID(sbdf_shadow);	/* [한국어] 세그먼트 */
	devid = PCI_SBDF_TO_DEVID(sbdf_shadow);	/* [한국어] 장치 id */

	for_each_pci_segment(pci_seg) {	/* [한국어] 세그먼트 구조체를 찾는다 */
		if (pci_seg->id != seg)	/* [한국어] 다른 세그먼트 */
			continue;	/* [한국어] 건너뛴다 */
		dump_irte(m, devid, pci_seg);	/* [한국어] 찾았으면 덤프 */
		break;	/* [한국어] 종료 */
	}

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_ATTRIBUTE(iommu_irqtbl);	/* [한국어] 같은 이유로 읽기 전용 */

/*
 * [한국어]
 * amd_iommu_debugfs_setup - debugfs 파일들을 만든다
 *
 * 두 층으로 나뉜다.
 *  - 유닛별(iommu%02d/): mmio, capability, cmdbuf. 각 유닛의 하드웨어를
 *    직접 들여다보는 파일들이라 유닛마다 있어야 한다.
 *  - 전역: devid, devtbl, irqtbl. 장치를 지정해 조회하는 파일들로, 대상
 *    장치가 정해지면 어느 유닛인지는 표에서 찾아지므로 하나면 된다.
 *
 * dbg_mmio_offset 과 dbg_cap_offset 을 -1 로 초기화하는 것이 중요하다.
 * 0 으로 두면 사용자가 아무것도 쓰지 않았는데도 오프셋 0 을 읽어, 지정한
 * 적 없는 값을 보게 된다.
 *
 * 권한이 갈리는 이유: 쓰기로 대상을 지정하는 파일만 0644 이고, 읽기만
 * 하는 파일은 0444 다.
 *
 * 반환값을 확인하지 않는 것은 debugfs 의 관례다 — 실패해도 진단 기능이
 * 없어질 뿐 드라이버 동작에는 영향이 없다.
 *
 * 호출 체인:
 *   amd_iommu_init → [이 함수] → debugfs_create_file()
 */
void amd_iommu_debugfs_setup(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */
	char name[MAX_NAME_LEN + 1];	/* [한국어] 유닛 디렉터리 이름을 만들 버퍼 */

	amd_iommu_debugfs = debugfs_create_dir("amd", iommu_debugfs_dir);	/* [한국어] IOMMU 공통 디렉터리 아래 AMD 전용 디렉터리 */

	for_each_iommu(iommu) {	/* [한국어] 유닛마다 하드웨어를 직접 보는 파일들을 만든다 */
		iommu->dbg_mmio_offset = -1;	/* [한국어] 0 으로 두면 지정한 적 없는 오프셋 0 을 읽게 된다 */
		iommu->dbg_cap_offset = -1;	/* [한국어] 같은 이유 */

		snprintf(name, MAX_NAME_LEN, "iommu%02d", iommu->index);	/* [한국어] 유닛 번호로 디렉터리 이름을 */
		iommu->debugfs = debugfs_create_dir(name, amd_iommu_debugfs);	/* [한국어] 유닛별 디렉터리 */

		debugfs_create_file("mmio", 0644, iommu->debugfs, iommu,	/* [한국어] 쓰기로 오프셋을 지정하므로 0644 */
				    &iommu_mmio_fops);
		debugfs_create_file("capability", 0644, iommu->debugfs, iommu,	/* [한국어] 같은 방식 */
				    &iommu_capability_fops);
		debugfs_create_file("cmdbuf", 0444, iommu->debugfs, iommu,	/* [한국어] 읽기 전용 */
				    &iommu_cmdbuf_fops);
	}

	debugfs_create_file("devid", 0644, amd_iommu_debugfs, NULL,	/* [한국어] 전역 — 대상 장치를 지정하는 관문 */
			    &devid_fops);
	debugfs_create_file("devtbl", 0444, amd_iommu_debugfs, NULL,	/* [한국어] 지정된 장치의 DTE. 어느 유닛인지는 표에서 찾아지므로 전역이면 된다 */
			    &iommu_devtbl_fops);
	debugfs_create_file("irqtbl", 0444, amd_iommu_debugfs, NULL,	/* [한국어] 같은 이유로 전역 */
			    &iommu_irqtbl_fops);
}
