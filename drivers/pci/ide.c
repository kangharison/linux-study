// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024-2025 Intel Corporation. All rights reserved. */

/* PCIe r7.0 section 6.33 Integrity & Data Encryption (IDE) */

/*
 * [한국어 설명] PCIe 링크 자체를 암호화하는 IDE (ide.c)
 *
 * === 파일의 역할 ===
 * IDE(Integrity & Data Encryption)는 PCIe 규격 r7.0 §6.33 에 정의된 기능으로,
 * 링크를 오가는 TLP(Transaction Layer Packet)를 암호화하고 무결성을 검증한다.
 * 이 파일은 그 하드웨어를 발견하고, 자원을 배분하고, 레지스터를 채우는 일을 한다.
 *
 * 왜 링크를 암호화하는가. 위협 모델이 "물리적으로 접근할 수 있는 공격자"이기
 * 때문이다. 클라우드에 맡긴 가상 머신을 생각해 보자. CPU 안의 메모리는
 * 기밀 컴퓨팅 기술로 암호화할 수 있지만, CPU 와 장치 사이의 PCIe 배선에
 * 계측 장비를 붙이면 오가는 데이터가 그대로 보인다. IDE 는 그 구간을 막는다.
 *
 * 그래서 IDE 는 혼자 쓰이지 않는다. TDISP(TEE Device Interface Security
 * Protocol)와 함께 쓰여, 장치를 기밀 가상 머신에 직접 할당하면서도 그
 * 경로 전체를 보호하는 것이 목표다. 커널에서 그 상위 계층이 tsm.c 다.
 *
 * --- IDE 의 두 가지 방식 ---
 * Link IDE      : 그 링크의 모든 트래픽을 암호화한다. 단순하지만 전부 아니면
 *                 전무라, 한 장치가 여러 주인을 섬기는 구성에는 맞지 않는다.
 * Selective IDE : 특정 Requester ID 범위와 특정 주소 범위만 골라 암호화한다.
 *                 SR-IOV 로 나뉜 VF 하나만 기밀 VM 에 주고 나머지는 평문으로
 *                 두는 것이 가능해진다. 이 파일이 주로 다루는 것이 이쪽이다.
 *
 * --- 헷갈리기 쉬운 두 가지 번호 ---
 * Stream ID    : 8비트. 호스트 브리지 범위에서 유일해야 하며, 링크 위를
 *                실제로 흐르는 패킷에 실려 "이 패킷이 어느 스트림의 것인가" 를
 *                나타낸다. 그래서 hb->ide_stream_ids_ida 로 관리한다.
 * Stream Index : 레지스터 블록의 번호. 장치마다 Selective IDE 레지스터
 *                블록이 여러 개 있고 그중 몇 번째를 쓸지를 가리킨다.
 *                장치마다 독립적이라 pdev->ide_stream_ida 로 관리한다.
 * 둘은 전혀 다른 것이다. 스트림 하나를 세우려면 Stream ID 하나와
 * Stream Index 셋(엔드포인트, 루트 포트, 호스트 브리지)이 필요하다.
 *
 * --- 파트너 포트라는 개념 ---
 * IDE 는 링크의 양 끝이 짝을 이뤄야 성립한다. 엔드포인트와 루트 포트가
 * 각자 자기 레지스터 블록을 갖고, 거기에 상대방의 정보를 적는다.
 * 엔드포인트의 블록에는 루트 포트의 RID 를, 루트 포트의 블록에는
 * 엔드포인트의 RID 를 적는 식이다. 이 파일에서 PCI_IDE_EP 와 PCI_IDE_RP
 * 두 항목을 나란히 다루는 이유다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거 단계:
 *   pci_scan_child_bus() -> pci_device_add() [probe.c:6499]
 *     -> [이 파일] pci_ide_init()
 *        - IDE capability 를 찾고 능력을 읽어 pdev 에 기록
 *        - 펌웨어가 이미 켜 둔 스트림이 있으면 그 번호를 선점
 *        - 쓰지 않는 블록에는 예약 Stream ID 를 넣어 표시
 *   pci_alloc_host_bridge() 경로 [probe.c:1782]
 *     -> [이 파일] pci_ide_init_host_bridge() 로 브리지의 스트림 풀 준비
 *   제거 시 [remove.c:157] -> pci_ide_destroy()
 *
 * 스트림 수립 단계 (상위 계층이 부른다):
 *   pci_ide_stream_alloc()   자원 확보
 *     -> pci_ide_stream_register()  Stream ID 확정 + sysfs 노출
 *        -> pci_ide_stream_setup()  레지스터에 설정 기록 (양쪽 포트에 각각)
 *           -> [여기서 키 교환이 일어난다 — 이 파일 밖의 일이다]
 *              -> pci_ide_stream_enable()  실제 활성화
 * 해제는 pci_ide_stream_release() 가 역순으로 되감는다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. config 접근과 메모리 할당이
 * 있어 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거 중 초기화), remove.c(정리), 그리고 스트림 API 를 쓸
 *   상위 계층. 다만 확인해 보면 pci_ide_stream_* 계열을 이 트리 안에서
 *   부르는 곳은 아직 없다 — 전부 EXPORT_SYMBOL_GPL 로 내보내기만 하고,
 *   실제 사용자는 앞으로 들어올 TSM/TDISP 코드와 플랫폼별 모듈이다.
 * 아래쪽: access.c 의 config 접근, ida(정수 ID 할당자), sysfs.
 * 옆쪽: tsm.c — 기밀 컴퓨팅 쪽 상위 계층이며 IDE 를 그 구성 요소로 쓴다.
 * 공유 상태: struct pci_dev 의 ide_cap / nr_link_ide / nr_sel_ide /
 *   nr_ide_mem / ide_stream_ida, 그리고 struct pci_host_bridge 의
 *   nr_ide_streams / ide_stream_ida / ide_stream_ids_ida.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리 전수
 * 확인 — pci_ide_* 호출 0건, tsm/tdisp 관련 심볼도 0건).
 *
 * 그럼에도 이 파일이 NVMe 를 공부하는 사람에게 의미가 있는 이유가 있다.
 * 기밀 컴퓨팅 환경에서 NVMe 를 쓰려면 데이터가 지나는 모든 구간이
 * 보호되어야 하는데, 그 구간이 셋이다.
 *   1) 드라이브 안의 데이터 — SED/OPAL 자체 암호화가 담당한다.
 *      (drivers/nvme/host/ 의 opal 관련 코드)
 *   2) 호스트 메모리 — CPU 의 기밀 컴퓨팅 기술이 담당한다.
 *   3) 그 사이의 PCIe 링크 — 여기가 비어 있었고, IDE 가 그것을 메운다.
 * 즉 IDE 는 NVMe 보안 그림의 마지막 조각이다.
 *
 * 또 하나. NVMe 는 SR-IOV 로 VF 를 만들 수 있는데, Selective IDE 가
 * RID 범위를 다루는 이유가 바로 그런 구성 때문이다. 아래
 * pci_ide_stream_alloc() 이 pci_num_vf() 로 VF 개수를 확인해 RID 범위를
 * 넓히는 코드가 그 대비다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_ide_init()            : 열거 중 IDE 능력을 발견하고 기록한다.
 *                             펌웨어가 켜 둔 스트림을 선점하는 일도 한다.
 * pci_ide_init_host_bridge(): 브리지의 스트림 풀(기본 256개)을 준비한다.
 * pci_ide_stream_alloc()    : 스트림 하나에 필요한 세 자리(EP/RP/HB)를
 *                             확보하고 RID·주소 범위를 계산해 담는다.
 * pci_ide_stream_register() : Stream ID 를 확정하고 sysfs 에 링크를 만든다.
 * pci_ide_stream_setup()    : 계산해 둔 설정을 실제 레지스터에 쓴다.
 *                             엔드포인트와 루트 포트에 각각 한 번씩 부른다.
 * pci_ide_stream_enable()   : 활성화하고 secure 상태가 됐는지 확인한다.
 * pci_ide_stream_disable() / _teardown() / _unregister() / _free() :
 *                             역순 해제 단계들.
 * pci_ide_stream_release()  : 위 해제들을 진행 상태에 맞게 알아서 되감는다.
 * pci_ide_to_settings()     : pdev 가 EP 인지 RP 인지 보고 해당 파트너
 *                             설정을 골라 준다. 이 파일 곳곳의 진입점.
 * sel_ide_offset()          : Selective IDE 레지스터 블록의 위치 계산.
 * struct pci_ide            : 스트림 하나의 전체 상태(파트너 둘 + HB 자리).
 * struct pci_ide_partner    : 한쪽 포트의 설정(RID 범위, 주소 범위, 인덱스).
 */

/* [한국어] 이 파일의 모든 dev_ 계열, pci_ 계열 로그 앞에 "PCI/IDE: " 를 붙인다.
 * dmesg 에서 IDE 관련 메시지만 골라 보기 쉽게 하려는 것으로,
 * 반드시 아래 헤더들보다 먼저 정의되어야 dev_printk 계열이 이 정의를 본다. */
#define dev_fmt(fmt) "PCI/IDE: " fmt
/* [한국어] FIELD_GET / FIELD_PREP — 레지스터의 특정 비트 구간을 읽고 쓴다.
 * 이 파일은 거의 모든 줄에서 이 매크로를 쓴다. 시프트와 마스크를 손으로
 * 쓰는 대신 마스크 상수만 주면 되므로 실수가 줄고, 값이 구간을 넘으면
 * 컴파일 시점에 잡힌다. */
#include <linux/bitfield.h>
/* [한국어] GENMASK 등 비트 범위 매크로. 아래 SEL_ADDR1_LOWER 정의에 쓴다. */
#include <linux/bitops.h>
/* [한국어] struct pci_dev, pci_read_config_dword 등 PCI 코어 API. */
#include <linux/pci.h>
/* [한국어] 이 파일이 구현하는 IDE API 의 선언과 struct pci_ide,
 * struct pci_ide_partner 정의. 상위 계층이 include 하는 공개 헤더다. */
#include <linux/pci-ide.h>
/* [한국어] PCI_IDE_* 레지스터 오프셋과 비트 마스크. PCIe 규격 §6.33 의
 * 레지스터 정의가 그대로 들어 있다. */
#include <linux/pci_regs.h>
/* [한국어] kzalloc_obj / kfree — struct pci_ide 할당에 쓴다. */
#include <linux/slab.h>
/* [한국어] sysfs_create_link 등. 스트림을 호스트 브리지 아래에 심볼릭
 * 링크로 노출해 사용자 공간이 볼 수 있게 한다. */
#include <linux/sysfs.h>

/* [한국어] PCI 코어 내부 헤더. pci_ide_attr_group 같은 내부 심볼을
 * probe.c 등과 공유하기 위해 필요하다. */
#include "pci.h"

/* [한국어]
 * __sel_ide_offset - Selective IDE 스트림 레지스터 블록의 config 오프셋 계산
 *
 * @ide_cap: 이 장치의 IDE 확장 capability 시작 오프셋.
 * @nr_link_ide: 이 장치가 가진 Link IDE 스트림 블록의 개수.
 * @stream_index: 몇 번째 Selective 스트림 블록을 원하는지.
 * @nr_ide_mem: 스트림 블록 하나가 갖는 주소 연관(address association) 블록 수.
 * @return: config space 안의 절대 오프셋.
 *
 * IDE capability 안의 레지스터 배치가 가변이라 이 계산이 필요하다.
 * 구조는 이렇다.
 *
 *   ide_cap ─┬─ IDE Capability / Control 레지스터 (고정 크기)
 *            ├─ Link IDE 블록 0 ─┐
 *            ├─ Link IDE 블록 1  │ nr_link_ide 개, 각각 고정 크기
 *            ├─ ...             ─┘
 *            ├─ Selective 블록 0 ─┐
 *            ├─ Selective 블록 1  │ 각각 (고정부 + 주소연관 nr_ide_mem 개)
 *            └─ ...              ─┘
 *
 * Link 블록은 크기가 고정이지만 Selective 블록은 그 안에 든 주소 연관
 * 블록 수만큼 커진다. 그래서 Selective 블록의 위치를 알려면 앞의 Link
 * 블록들을 건너뛴 뒤, 블록 하나의 크기에 인덱스를 곱해야 한다.
 *
 * 상류 주석이 "일정하다고 가정한다" 고 밝힌 부분이 중요하다. 규격상으로는
 * 스트림마다 주소 연관 블록 수가 다를 수 있지만, 그러면 이런 단순한
 * 곱셈으로 위치를 구할 수 없다. 그래서 커널은 그런 장치를 지원하지 않고,
 * pci_ide_init() 에서 첫 스트림과 개수가 다른 스트림을 만나면 거기서
 * 멈춘다(아래 "SKIP the rest" 로그).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산이라 부수효과가 없다.
 *
 * 호출 체인:
 *   pci_ide_init() / sel_ide_offset() → [이 함수]
 */
static int __sel_ide_offset(u16 ide_cap, u8 nr_link_ide, u8 stream_index,
			    u8 nr_ide_mem)
{
	/* [한국어] 먼저 Link IDE 블록들을 건너뛴다. PCI_IDE_LINK_STREAM_0 이
	 * capability 헤더 다음 첫 Link 블록의 상대 위치이고, 거기서 블록
	 * 크기 × 개수만큼 더하면 Selective 영역의 시작이다. */
	u32 offset = ide_cap + PCI_IDE_LINK_STREAM_0 +
		     nr_link_ide * PCI_IDE_LINK_BLOCK_SIZE;

	/*
	 * Assume a constant number of address association resources per stream
	 * index
	 */
	/* [한국어] PCI_IDE_SEL_BLOCK_SIZE(nr_ide_mem) 가 Selective 블록 하나의
	 * 크기다. 주소 연관 블록 수에 따라 달라지므로 인자로 받는다.
	 * 위 상류 주석대로 모든 스트림이 같은 개수를 갖는다고 전제하기에
	 * 단순 곱셈이 성립한다. */
	return offset + stream_index * PCI_IDE_SEL_BLOCK_SIZE(nr_ide_mem);
}

/* [한국어]
 * sel_ide_offset - 위 계산의 편의 래퍼
 *
 * @pdev: 대상 장치. 필요한 값 셋(ide_cap, nr_link_ide, nr_ide_mem)이 여기 있다.
 * @settings: 이 포트의 파트너 설정. 여기서 stream_index 를 가져온다.
 * @return: Selective IDE 스트림 블록의 config 오프셋.
 *
 * 인자 넷을 매번 늘어놓는 대신 pdev 와 settings 에서 꺼내 오게 한 것뿐이다.
 * pci_ide_init() 만 __sel_ide_offset() 을 직접 부르는데, 그때는 아직
 * pdev 에 값이 기록되기 전이라 지역 변수를 넘겨야 하기 때문이다.
 * 그 외 모든 곳은 이 래퍼를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산.
 *
 * 호출 체인:
 *   pci_ide_stream_setup() / _teardown() / _enable() / _disable()
 *     → [이 함수] → __sel_ide_offset()
 */
static int sel_ide_offset(struct pci_dev *pdev,
			  struct pci_ide_partner *settings)
{
	/* [한국어] pdev 에 기록해 둔 IDE 구성값과 settings 의 스트림 인덱스를
	 * 조합해 실제 계산 함수로 넘긴다. */
	return __sel_ide_offset(pdev->ide_cap, pdev->nr_link_ide,
				settings->stream_index, pdev->nr_ide_mem);
}

/* [한국어]
 * reserve_stream_index - 특정 스트림 인덱스를 콕 집어 예약한다
 *
 * @pdev: 대상 장치.
 * @idx: 예약할 스트림 인덱스(레지스터 블록 번호).
 * @return: 예약에 성공하면 true, 이미 쓰이고 있으면 false.
 *
 * 보통의 할당은 "빈 것 아무거나" 를 요청하지만(아래 alloc_stream_index),
 * 이 함수는 반대로 지정한 번호만 원한다. 그런 요구가 생기는 상황이 하나
 * 있다 — 부팅 시 펌웨어가 이미 어떤 스트림 블록을 켜 둔 경우다.
 * 그 번호는 커널이 고를 수 없고 하드웨어가 이미 정해 놓은 것이므로,
 * 나중에 커널이 같은 번호를 다른 용도로 배정하지 않도록 미리 막아야 한다.
 *
 * ida_alloc_range() 에 최소와 최대를 같은 값으로 주면 그 번호만 시도한다.
 * 이미 할당되어 있으면 -ENOSPC 를 돌려주므로 음수 여부로 판단한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_ide_init() → claim_stream() → [이 함수] → ida_alloc_range()
 */
static bool reserve_stream_index(struct pci_dev *pdev, u8 idx)
{
	int ret;

	/* [한국어] min 과 max 를 모두 idx 로 주어 "이 번호가 아니면 실패" 를
	 * 요청한다. ida 는 성공 시 할당된 번호를, 실패 시 음수 오류를 반환한다. */
	ret = ida_alloc_range(&pdev->ide_stream_ida, idx, idx, GFP_KERNEL);
	/* [한국어] 반환값이 음수가 아니면(= 요청한 번호를 받았으면) 성공.
	 * 값 자체는 idx 와 같으므로 버리고 성패만 알린다. */
	return ret >= 0;
}

/* [한국어]
 * reserve_stream_id - 특정 Stream ID 를 호스트 브리지 범위에서 예약한다
 *
 * @hb: 호스트 브리지. Stream ID 의 유일성이 보장되어야 하는 범위다.
 * @id: 예약할 8비트 Stream ID.
 * @return: 성공하면 true, 이미 쓰이고 있으면 false.
 *
 * 바로 위 reserve_stream_index() 와 형태는 같지만 대상이 다르다.
 * 파일 상단에서 구분한 두 번호 중 이쪽이 Stream ID — 링크 위를 실제로
 * 흐르는 패킷에 실리는 값이다.
 *
 * 왜 호스트 브리지 단위인가. 같은 브리지 아래 두 장치가 같은 Stream ID 를
 * 쓰면 패킷이 어느 스트림의 것인지 구분할 수 없기 때문이다. 그래서
 * ida 를 장치가 아니라 브리지에 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_ide_init() → claim_stream() → [이 함수]
 *   pci_ide_init_host_bridge() → [이 함수]   (예약 ID 를 미리 잡아 둔다)
 *   pci_ide_stream_register() → request_stream_id() → [이 함수]
 */
static bool reserve_stream_id(struct pci_host_bridge *hb, u8 id)
{
	int ret;

	/* [한국어] 브리지의 Stream ID ida 에서 그 번호 하나만 요청한다.
	 * 위 함수와 달리 ida 가 pdev 가 아니라 hb 에 붙어 있다는 점이 핵심이다. */
	ret = ida_alloc_range(&hb->ide_stream_ids_ida, id, id, GFP_KERNEL);
	/* [한국어] 음수가 아니면 성공. */
	return ret >= 0;
}

/* [한국어]
 * claim_stream - 펌웨어가 이미 켜 둔 스트림의 번호들을 선점한다
 *
 * @hb: 호스트 브리지.
 * @stream_id: 그 스트림이 쓰고 있는 Stream ID.
 * @pdev: Selective IDE 라면 대상 장치, Link IDE 라면 NULL.
 * @stream_idx: Selective IDE 라면 스트림 인덱스, Link IDE 라면 무시된다.
 * @return: 두 예약이 모두 성공하면 true.
 *
 * 커널이 부팅되기 전에 펌웨어가 IDE 스트림을 켜 두는 경우가 있다.
 * 그러면 커널은 그 사실을 존중해야 한다 — 이미 동작 중인 스트림의
 * 번호를 커널이 나중에 다른 용도로 배정하면 그 스트림이 깨지기 때문이다.
 *
 * 그래서 열거 중에 켜져 있는 스트림을 발견하면 그 번호들을 ida 에
 * 미리 잡아 둔다. 커널이 쓰려고 잡는 것이 아니라, 쓰지 못하게 막으려고
 * 잡는 것이다.
 *
 * 두 종류의 IDE 에서 잡아야 할 것이 다르다.
 *   Link IDE      : Stream ID 만 있으면 된다. 레지스터 블록은 링크마다
 *                   고정이라 인덱스라는 개념이 없다.
 *   Selective IDE : Stream ID 와 스트림 인덱스 둘 다.
 * pdev 가 NULL 인지로 그 둘을 구분한다.
 *
 * 실패하면 호출자(pci_ide_init)가 그 장치의 IDE 초기화를 중단한다.
 * 상태를 정확히 파악할 수 없는 장치를 어설프게 다루느니 IDE 를 아예
 * 쓰지 않는 편이 안전하다는 판단이다.
 *
 * 실행 컨텍스트: 열거 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_device_add() [probe.c] → pci_ide_init() → [이 함수]
 *     → reserve_stream_id() / reserve_stream_index()
 */
static bool claim_stream(struct pci_host_bridge *hb, u8 stream_id,
			 struct pci_dev *pdev, u8 stream_idx)
{
	/* [한국어] 부팅 시점에 이미 켜져 있는 스트림을 발견했음을 알린다.
	 * 흔한 일이 아니므로 dev_dbg 가 아니라 dev_info 로 남긴다 — 나중에
	 * 스트림 자원이 부족할 때 원인을 찾는 실마리가 된다. */
	dev_info(&hb->dev, "Stream ID %d active at init\n", stream_id);
	/* [한국어] 먼저 Stream ID 를 브리지 범위에서 잡는다. */
	if (!reserve_stream_id(hb, stream_id)) {
		/* [한국어] 실패했다는 것은 그 번호가 이미 예약되어 있다는 뜻이다.
		 * 두 가지 경우가 있어 메시지에서 구분한다.
		 *   reserved — PCI_IDE_RESERVED_STREAM_ID 는 브리지 초기화 때
		 *     커널이 일부러 잡아 둔 번호다(pci_ide_init_host_bridge 참고).
		 *     펌웨어가 하필 그 번호를 썼다면 충돌한다.
		 *   active — 다른 장치가 이미 같은 번호를 쓰고 있다. Stream ID 는
		 *     브리지 범위에서 유일해야 하므로 이것은 펌웨어의 오류다. */
		dev_info(&hb->dev, "Failed to claim %s Stream ID %d\n",
			 stream_id == PCI_IDE_RESERVED_STREAM_ID ? "reserved" :
								   "active",
			 stream_id);
		return false;
	}

	/* No stream index to reserve in the Link IDE case */
	/* [한국어] 상류 주석대로 Link IDE 는 여기서 끝이다. pdev 가 NULL 로
	 * 넘어온 것이 곧 "Link IDE 경로" 라는 신호다. */
	if (!pdev)
		return true;

	/* [한국어] Selective IDE 라면 레지스터 블록 번호도 잡아야 한다.
	 * 이쪽 ida 는 장치마다 따로 있다. */
	if (!reserve_stream_index(pdev, stream_idx)) {
		/* [한국어] 여기 실패하는 것은 같은 장치의 두 스트림이 같은
		 * 블록을 가리킨다는 뜻이라 정상적으로는 일어날 수 없다.
		 * 앞서 잡은 Stream ID 를 되돌리지 않는데, 호출자가 초기화를
		 * 중단하고 이 장치의 IDE 를 통째로 쓰지 않을 것이므로
		 * 그 번호를 계속 막아 두는 편이 오히려 안전하기 때문이다. */
		pci_info(pdev, "Failed to claim active Selective Stream %d\n",
			 stream_idx);
		return false;
	}

	return true;
}

/* [한국어]
 * pci_ide_init - 열거 중 장치의 IDE 능력을 발견하고 초기 상태를 정리한다
 *
 * @pdev: 방금 열거된 PCI 장치.
 * @return: 없음. IDE 를 못 쓰는 장치라면 조용히 물러난다 — 대부분의 장치가
 *   그러하며 오류가 아니다.
 *
 * 이 함수는 세 가지 일을 순서대로 한다.
 *   1) 능력 파악 — IDE capability 가 있는지, Selective IDE 를 지원하는지,
 *      Link IDE 블록과 Selective 블록이 각각 몇 개인지 읽어 pdev 에 기록한다.
 *   2) 선점 — 펌웨어가 이미 켜 둔 스트림이 있으면 그 번호들을 ida 에 잡아
 *      커널이 나중에 같은 번호를 배정하지 못하게 한다.
 *   3) 표시 — 쓰지 않는 블록의 Stream ID 필드에 예약 번호를 넣어 둔다.
 *
 * 3번이 왜 필요한지가 이 함수에서 가장 덜 자명한 부분이다. 켜지지 않은
 * 블록의 Stream ID 필드는 값이 무엇이든 동작에 영향이 없지만, 하드웨어가
 * 리셋 직후 남긴 값이 우연히 다른 활성 스트림과 같으면 진단할 때 혼란스럽다.
 * 그래서 "이 블록은 아무도 쓰지 않는다" 는 뜻의 약속된 번호를 넣어 둔다.
 *
 * 엔드포인트에 대해 루트 포트의 IDE 능력을 함께 요구하는 이유도 분명하다.
 * IDE 는 링크 양 끝이 짝을 이뤄야 성립하므로(파일 상단의 파트너 포트 설명),
 * 한쪽만 능력이 있으면 스트림을 세울 수 없다. 그런 장치는 아예 IDE 가
 * 없는 것으로 취급해 나중에 헛되이 시도하지 않게 한다.
 *
 * 실행 컨텍스트: 열거 중 프로세스 컨텍스트. config 접근과 GFP_KERNEL 할당이
 * 있어 잠들 수 있다. 한 장치에 대해 한 번만 불리므로 재진입 걱정은 없다.
 *
 * 에러 경로: 중간에 실패하면 pdev->ide_cap 을 설정하지 않은 채 돌아간다.
 * 그 값이 0 이면 이후 모든 IDE 함수가 이 장치를 건너뛰므로, 별도의
 * 되돌리기 없이 "IDE 없는 장치" 상태가 된다. ida_init 만 무조건 해 두는
 * 이유가 이것이다 — 아래 첫 주석이 말하는 일관성이 그 뜻이다.
 *
 * 호출 체인:
 *   pci_scan_child_bus() → pci_device_add() [probe.c:6499] → [이 함수]
 *     → pci_find_ext_capability() [pci.c]
 *     → claim_stream() → reserve_stream_id() / reserve_stream_index()
 */
void pci_ide_init(struct pci_dev *pdev)
{
	/* [한국어] Stream ID 는 호스트 브리지 범위에서 유일해야 하므로,
	 * 이 장치가 속한 브리지를 먼저 찾아 둔다. 아래 claim_stream 이 쓴다. */
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);
	/* [한국어] nr_link_ide: Link IDE 블록 수 — Selective 블록의 시작 위치를
	 *   계산하려면 앞의 Link 블록들을 건너뛰어야 하므로 필요하다.
	 * nr_ide_mem: Selective 블록 하나에 든 주소 연관 블록 수.
	 * nr_streams: Selective 스트림 블록 수. */
	u16 nr_link_ide, nr_ide_mem, nr_streams;
	/* [한국어] IDE 확장 capability 의 시작 오프셋. 성공했을 때만
	 * 마지막에 pdev->ide_cap 으로 옮긴다. */
	u16 ide_cap;
	/* [한국어] config 읽기용 임시 변수. 여러 레지스터에 재사용한다. */
	u32 val;

	/*
	 * Unconditionally init so that ida idle state is consistent with
	 * pdev->ide_cap.
	 */
	/* [한국어] 상류 주석대로 조건 없이 초기화한다. IDE 를 못 쓰는 장치라도
	 * ida 는 초기화되어 있어야 하는데, 장치가 제거될 때 pci_ide_destroy()
	 * 가 ida_destroy() 를 무조건 부르기 때문이다. 초기화되지 않은 ida 를
	 * 파괴하려 들면 문제가 된다. */
	ida_init(&pdev->ide_stream_ida);

	/* [한국어] IDE 는 PCIe 규격의 기능이므로 구형 PCI 장치에는 없다. */
	if (!pci_is_pcie(pdev))
		return;

	/* [한국어] 확장 capability 목록에서 IDE 를 찾는다. 확장 capability 는
	 * config space 의 0x100 이후에 연결 리스트로 놓여 있다. 없으면 0. */
	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	if (!ide_cap)
		return;

	/* [한국어] IDE Capability 레지스터를 읽는다. 이 장치가 IDE 로 무엇을
	 * 할 수 있는지가 전부 이 한 워드에 들어 있다. */
	pci_read_config_dword(pdev, ide_cap + PCI_IDE_CAP, &val);
	/* [한국어] Selective IDE 를 지원하지 않으면 이 커널 코드가 다룰 것이
	 * 없다. Link IDE 만 있는 장치는 세밀한 제어가 불가능해 기밀 컴퓨팅
	 * 용도로 쓰기 어렵기 때문에 지원 대상에서 뺀다. */
	if ((val & PCI_IDE_CAP_SELECTIVE) == 0)
		return;

	/*
	 * Require endpoint IDE capability to be paired with IDE Root Port IDE
	 * capability.
	 */
	/* [한국어] 상류 주석대로 엔드포인트는 짝이 되는 루트 포트도 IDE 능력이
	 * 있어야 한다. IDE 는 링크 양 끝이 함께 설정해야 성립하기 때문이다. */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT) {
		/* [한국어] 이 엔드포인트가 매달린 루트 포트를 거슬러 올라가 찾는다. */
		struct pci_dev *rp = pcie_find_root_port(pdev);

		/* [한국어] 루트 포트에 ide_cap 이 없다는 것은 그쪽에서 이 함수가
		 * 성공하지 못했다는 뜻이다. 열거 순서상 루트 포트가 먼저 처리되므로
		 * 이 시점에 그 값을 믿을 수 있다. */
		if (!rp->ide_cap)
			return;
	}

	/* [한국어] Selective IDE 스트림이 config 요청(TLP)까지 보호할 수 있는지.
	 * 나중에 Control 레지스터의 CFG_EN 비트로 그대로 쓴다. */
	pdev->ide_cfg = FIELD_GET(PCI_IDE_CAP_SEL_CFG, val);
	/* [한국어] TEE-Limited: 이 스트림을 신뢰 실행 환경(TEE)에 속한 요청으로만
	 * 제한할 수 있는지. 기밀 컴퓨팅에서 중요한 비트로, 역시 나중에
	 * Control 레지스터에 그대로 실린다. */
	pdev->ide_tee_limit = FIELD_GET(PCI_IDE_CAP_TEE_LIMITED, val);

	/* [한국어] Link IDE 를 지원하면 그 블록이 몇 개인지 센다. 규격이 값을
	 * 0-기반으로 인코딩하므로 1 을 더한다 — 0 이 "1개" 를 뜻한다.
	 * 블록 수는 지원하는 트래픽 클래스(TC) 수와 같다. */
	if (val & PCI_IDE_CAP_LINK)
		nr_link_ide = 1 + FIELD_GET(PCI_IDE_CAP_LINK_TC_NUM, val);
	else
		/* [한국어] Link IDE 가 없으면 0 개. Selective 블록이 곧바로 시작된다. */
		nr_link_ide = 0;

	/* [한국어] 첫 스트림의 주소 연관 블록 수를 아직 모르므로 0 에서 시작한다.
	 * 첫 회차에서 __sel_ide_offset(..., 0) 을 부르는 것이 맞는 이유는,
	 * i=0 일 때 곱해지는 stream_index 가 0 이라 블록 크기가 결과에
	 * 영향을 주지 않기 때문이다. 두 번째 회차부터는 아래에서 채운
	 * 실제 값이 쓰인다. */
	nr_ide_mem = 0;
	/* [한국어] Selective 스트림 블록 개수. 역시 0-기반 인코딩이라 1 을 더한다. */
	nr_streams = 1 + FIELD_GET(PCI_IDE_CAP_SEL_NUM, val);
	/* [한국어] 첫 번째 순회 — 각 Selective 블록의 능력을 읽고, 이미 켜져
	 * 있는 스트림이 있으면 그 번호를 선점한다. */
	for (u16 i = 0; i < nr_streams; i++) {
		/* [한국어] i 번째 블록의 config 오프셋. 바깥 pdev 에는 아직 값이
		 * 기록되기 전이라 지역 변수를 넘기는 __sel_ide_offset 을 쓴다. */
		int pos = __sel_ide_offset(ide_cap, nr_link_ide, i, nr_ide_mem);
		int nr_assoc;
		/* [한국어] 바깥 val 을 가린다. 여기서는 이 블록의 값만 다루므로
		 * 바깥 것을 덮어쓰지 않으려는 의도다. */
		u32 val;
		u8 id;

		/* [한국어] 이 스트림 블록의 Capability 레지스터를 읽는다. */
		pci_read_config_dword(pdev, pos + PCI_IDE_SEL_CAP, &val);

		/*
		 * Let's not entertain streams that do not have a constant
		 * number of address association blocks
		 */
		/* [한국어] 이 블록이 가진 주소 연관 블록 수. */
		nr_assoc = FIELD_GET(PCI_IDE_SEL_CAP_ASSOC_NUM, val);
		/* [한국어] 상류 주석대로, 스트림마다 개수가 다르면 지원하지 않는다.
		 * __sel_ide_offset() 의 단순 곱셈이 성립하지 않게 되기 때문이다.
		 * i 가 0 이 아닐 때만 비교하는 이유는 첫 회차에는 비교 대상인
		 * nr_ide_mem 이 아직 0(초기값)이기 때문이다. */
		if (i && (nr_assoc != nr_ide_mem)) {
			/* [한국어] 여기까지만 쓰겠다고 알린다. 발견한 것을 버리는 것이
			 * 아니라, 앞의 i 개는 그대로 쓰고 나머지만 포기한다. */
			pci_info(pdev, "Unsupported Selective Stream %d capability, SKIP the rest\n", i);
			/* [한국어] 사용 가능한 스트림 수를 i 로 줄인다. 이 값이 아래
			 * 두 번째 순회와 pdev->nr_sel_ide 에 그대로 쓰인다. */
			nr_streams = i;
			break;
		}

		/* [한국어] 첫 회차에서는 실제 값을 채우고, 이후 회차에서는 같은
		 * 값을 다시 넣는다(위 검사를 통과했으므로 같음이 보장된다). */
		nr_ide_mem = nr_assoc;

		/*
		 * Claim Stream IDs and Selective Stream blocks that are already
		 * active on the device
		 */
		/* [한국어] 이 블록의 Control 레지스터를 읽어 현재 상태를 본다. */
		pci_read_config_dword(pdev, pos + PCI_IDE_SEL_CTL, &val);
		/* [한국어] 켜져 있다면 어떤 Stream ID 로 동작 중인지 꺼낸다. */
		id = FIELD_GET(PCI_IDE_SEL_CTL_ID, val);
		/* [한국어] Enable 비트가 서 있으면 펌웨어가 켜 둔 스트림이다.
		 * 그 번호들을 선점하되, 실패하면 이 장치의 IDE 초기화를 중단한다 —
		 * 상태를 정확히 알 수 없는 채로 진행하느니 안 쓰는 편이 낫다.
		 * 중단해도 pdev->ide_cap 이 설정되지 않아 자연히 "IDE 없음" 이 된다. */
		if ((val & PCI_IDE_SEL_CTL_EN) &&
		    !claim_stream(hb, id, pdev, i))
			return;
	}

	/* Reserve link stream-ids that are already active on the device */
	/* [한국어] 두 번째 순회 — Link IDE 쪽도 같은 이유로 확인한다.
	 * Selective 를 지원하는 장치라도 Link IDE 블록을 함께 가질 수 있고,
	 * 그쪽이 켜져 있으면 그 Stream ID 역시 브리지 범위에서 막아야 한다. */
	for (u16 i = 0; i < nr_link_ide; ++i) {
		/* [한국어] Link 블록은 크기가 고정이라 계산이 단순하다.
		 * Selective 와 달리 주소 연관 블록이 없기 때문이다. */
		int pos = ide_cap + PCI_IDE_LINK_STREAM_0 + i * PCI_IDE_LINK_BLOCK_SIZE;
		u8 id;

		/* [한국어] Link IDE 의 Control 레지스터를 읽는다. */
		pci_read_config_dword(pdev, pos + PCI_IDE_LINK_CTL_0, &val);
		/* [한국어] 동작 중인 Stream ID 를 꺼낸다. */
		id = FIELD_GET(PCI_IDE_LINK_CTL_ID, val);
		/* [한국어] pdev 자리에 NULL, 인덱스 자리에 -1 을 넘기는 것이
		 * claim_stream() 에 "이건 Link IDE 다" 를 알리는 방식이다.
		 * Link IDE 에는 선점할 레지스터 블록 번호가 없으므로 -1 은
		 * 실제로 쓰이지 않는다. */
		if ((val & PCI_IDE_LINK_CTL_EN) &&
		    !claim_stream(hb, id, NULL, -1))
			return;
	}

	/* [한국어] 세 번째 순회 — 쓰지 않는 Selective 블록에 예약 Stream ID 를
	 * 적어 둔다. 켜지지 않은 블록의 ID 필드는 동작에 영향이 없지만,
	 * 리셋 직후 남은 값이 우연히 활성 스트림과 같으면 레지스터를 덤프해
	 * 진단할 때 혼란스럽다. 그래서 "아무도 쓰지 않음" 을 뜻하는 약속된
	 * 번호로 통일해 둔다. */
	for (u16 i = 0; i < nr_streams; i++) {
		/* [한국어] 이제 nr_ide_mem 이 실제 값으로 채워졌으므로 오프셋
		 * 계산이 정확하다. 위 첫 순회에서는 0 이었다. */
		int pos = __sel_ide_offset(ide_cap, nr_link_ide, i, nr_ide_mem);

		/* [한국어] 현재 Control 레지스터 값을 읽는다. 다른 비트를
		 * 보존해야 하므로 통째로 덮어쓰지 않고 읽어서 고친다. */
		pci_read_config_dword(pdev, pos + PCI_IDE_SEL_CTL, &val);
		/* [한국어] 이미 켜져 있는 블록은 건드리지 않는다. 앞에서
		 * claim_stream() 으로 그 ID 를 선점해 두었고, 동작 중인 스트림의
		 * ID 를 바꾸면 그 스트림이 깨진다. */
		if (val & PCI_IDE_SEL_CTL_EN)
			continue;
		/* [한국어] ID 필드만 지운다. 나머지 비트는 그대로 둔다. */
		val &= ~PCI_IDE_SEL_CTL_ID;
		/* [한국어] 그 자리에 예약 번호를 넣는다. */
		val |= FIELD_PREP(PCI_IDE_SEL_CTL_ID, PCI_IDE_RESERVED_STREAM_ID);
		/* [한국어] Enable 비트는 건드리지 않았으므로 스트림이 켜지지는
		 * 않는다. 표시만 남기는 쓰기다. */
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, val);
	}

	/* [한국어] 네 번째 순회 — Link IDE 블록에도 같은 표시를 한다. */
	for (u16 i = 0; i < nr_link_ide; ++i) {
		/* [한국어] Link 블록은 크기가 고정이라 계산이 단순하다. */
		int pos = ide_cap + PCI_IDE_LINK_STREAM_0 +
			  i * PCI_IDE_LINK_BLOCK_SIZE;

		/* [한국어] Link 블록은 Control 레지스터가 블록의 맨 앞이라
		 * 오프셋을 더하지 않고 pos 를 그대로 쓴다. 위 Selective 쪽이
		 * pos + PCI_IDE_SEL_CTL 인 것과 다른 점이다. */
		pci_read_config_dword(pdev, pos, &val);
		/* [한국어] 켜져 있으면 건드리지 않는다. */
		if (val & PCI_IDE_LINK_CTL_EN)
			continue;
		/* [한국어] ID 필드를 지우고 예약 번호로 채운다. */
		val &= ~PCI_IDE_LINK_CTL_ID;
		val |= FIELD_PREP(PCI_IDE_LINK_CTL_ID, PCI_IDE_RESERVED_STREAM_ID);
		pci_write_config_dword(pdev, pos, val);
	}

	/* [한국어] 여기까지 왔다는 것은 모든 확인과 선점이 성공했다는 뜻이다.
	 * 이제야 pdev 에 값을 기록한다. 순서가 중요한데, 중간에 실패해 일찍
	 * 돌아간 경우 ide_cap 이 0 으로 남아 이후 모든 IDE 코드가 이 장치를
	 * 자연스럽게 건너뛰기 때문이다. 별도의 되돌리기 코드가 필요 없는
	 * 이유가 이 배치에 있다. */
	pdev->ide_cap = ide_cap;
	/* [한국어] Link 블록 수. Selective 블록의 위치 계산에 계속 쓰인다. */
	pdev->nr_link_ide = nr_link_ide;
	/* [한국어] 쓸 수 있는 Selective 스트림 수. 위에서 "SKIP the rest" 로
	 * 줄어들었을 수 있으므로 원래 능력값이 아니라 이 값을 기록한다. */
	pdev->nr_sel_ide = nr_streams;
	/* [한국어] 블록 하나당 주소 연관 블록 수. 오프셋 계산과, 나중에
	 * 몇 개의 주소 범위를 등록할 수 있는지 판단하는 데 쓰인다. */
	pdev->nr_ide_mem = nr_ide_mem;
}

/* [한국어]
 * struct stream_index - 할당한 스트림 인덱스를 되돌릴 수 있게 묶어 둔 것
 *
 * 이 구조체 자체가 목적이 아니라, 아래 DEFINE_FREE 와 짝을 이루기 위한
 * 그릇이다. pci_ide_stream_alloc() 은 세 곳(엔드포인트, 루트 포트,
 * 호스트 브리지)에서 인덱스를 할당하는데, 두 번째가 실패하면 첫 번째를
 * 되돌려야 하고 세 번째가 실패하면 앞의 둘을 되돌려야 한다. 그런 계단식
 * 정리를 goto 로 쓰면 길고 틀리기 쉬워서, 커널의 __free() 정리 기능을
 * 쓴다. 그러려면 "어느 ida 에서 몇 번을 빌렸는지" 를 한 덩어리로 들고
 * 있어야 하고, 그것이 이 구조체다.
 */
struct stream_index {
	struct ida *ida;
	/* [한국어] 이 인덱스를 빌려 온 ida 를 가리킨다.
	 * 되돌려줄 때 어디로 반납해야 하는지 알아야 하기 때문에 함께 들고 있다.
	 * 설정자: alloc_stream_index() 가 할당에 성공한 직후 채운다.
	 * 읽는 자: free_stream_index() 가 반납할 때.
	 * 값 범위: pdev->ide_stream_ida 또는 hb->ide_stream_ida 중 하나.
	 *   어느 쪽인지는 호출자가 정한다.
	 * 동기화: ida 자체가 내부 잠금을 갖는다. 이 포인터는 지역 변수의
	 *   수명 동안만 쓰이므로 별도 보호가 필요 없다. */

	u8 stream_index;
	/* [한국어] 실제로 할당받은 번호.
	 * 설정자: alloc_stream_index() 가 ida_alloc_max() 의 반환값을 넣는다.
	 * 읽는 자: free_stream_index() 가 반납할 때, 그리고
	 *   pci_ide_stream_alloc() 이 struct pci_ide 로 옮겨 담을 때.
	 * 값 범위: 0 부터 max-1 까지. max 는 그 장치의 Selective 블록 수
	 *   또는 브리지의 스트림 풀 크기다.
	 * 동기화: 위와 같다. */
};

/* [한국어]
 * free_stream_index - 빌린 스트림 인덱스를 ida 에 반납한다
 *
 * @stream: 반납할 인덱스와 그것을 빌려 준 ida.
 * @return: 없음.
 *
 * 한 줄짜리 함수지만 따로 있는 이유는 아래 DEFINE_FREE 에 이름으로
 * 넘겨야 하기 때문이다. 매크로 인자 안에 ida_free() 호출을 직접 쓰는 것보다
 * 이름을 하나 두는 편이 읽기 쉽다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 정리 경로에서 자동으로 불린다.
 *
 * 호출 체인:
 *   (컴파일러가 스코프를 벗어날 때) → DEFINE_FREE(free_stream) → [이 함수]
 */
static void free_stream_index(struct stream_index *stream)
{
	/* [한국어] ida 에 번호를 돌려준다. 이후 다른 스트림이 그 번호를 쓸 수 있다. */
	ida_free(stream->ida, stream->stream_index);
}

/* [한국어] __free(free_stream) 로 선언된 변수가 스코프를 벗어날 때
 * 자동으로 free_stream_index() 를 부르게 한다.
 * _T 가 NULL 인지 확인하는 이유는, 할당에 실패해 NULL 인 채로 스코프를
 * 벗어나는 경우와, no_free_ptr() 로 소유권을 넘겨 일부러 NULL 로 만든
 * 경우가 있기 때문이다. 후자가 이 파일에서 실제로 쓰이는 방식이다 —
 * 성공했을 때는 소유권을 struct pci_ide 로 넘기고 자동 정리를 끈다. */
DEFINE_FREE(free_stream, struct stream_index *, if (_T) free_stream_index(_T))
/* [한국어]
 * alloc_stream_index - 빈 스트림 인덱스를 하나 받아 온다
 *
 * @ida: 어느 풀에서 받을지. 장치의 것이거나 호스트 브리지의 것이다.
 * @max: 그 풀의 크기. 0 이면 이 자원이 아예 없다는 뜻이다.
 * @stream: 결과를 채워 넣을 그릇. 호출자의 스택에 있는 배열의 한 칸이다.
 * @return: 성공하면 @stream 을 그대로 돌려주고, 실패하면 NULL.
 *
 * 앞의 reserve_stream_index() 와 반대다. 그쪽은 "이 번호를 달라" 였고
 * 이쪽은 "빈 것 아무거나 달라" 다. 평소의 스트림 수립은 이쪽을 쓴다.
 *
 * 결과를 새로 할당하지 않고 호출자가 준 그릇에 채우는 이유는, 이 값들이
 * pci_ide_stream_alloc() 함수 안에서만 살면 되기 때문이다. 세 개를
 * 스택 배열로 잡아 두고 그 칸을 하나씩 넘긴다 — 힙 할당과 그 실패
 * 처리를 피하는 방법이다.
 *
 * 반환값이 인자와 같은 포인터라는 점이 처음에는 이상해 보이지만,
 * __free() 와 함께 쓰기 위한 것이다. 실패 시 NULL 이 들어가야 자동 정리가
 * 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_ide_stream_alloc() → [이 함수] → ida_alloc_max()
 */
static struct stream_index *alloc_stream_index(struct ida *ida, u16 max,
					       struct stream_index *stream)
{
	int id;

	/* [한국어] 풀 크기가 0 이면 이 장치나 브리지에 Selective 스트림
	 * 자원이 없다는 뜻이다. ida 에 물어볼 것도 없이 실패다. */
	if (!max)
		return NULL;

	/* [한국어] 0 부터 max-1 사이에서 빈 번호를 받는다. ida_alloc_max 의
	 * 두 번째 인자가 포함 상한이라 max 가 아니라 max-1 을 넘긴다.
	 * 자원이 다 찼으면 -ENOSPC 를 돌려준다. */
	id = ida_alloc_max(ida, max - 1, GFP_KERNEL);
	if (id < 0)
		return NULL;

	/* [한국어] 받은 번호와 그것을 준 ida 를 함께 기록한다. 나중에
	 * 자동 정리가 이 둘만 보고 반납할 수 있게 하려는 것이다. */
	*stream = (struct stream_index) {
		.ida = ida,
		.stream_index = id,
	};
	/* [한국어] 호출자가 준 포인터를 그대로 돌려준다. 호출자는 이 값을
	 * __free(free_stream) 변수에 담아 실패 시 자동 반납되게 한다. */
	return stream;
}

/**
 * pci_ide_stream_alloc() - Reserve stream indices and probe for settings
 * @pdev: IDE capable PCIe Endpoint Physical Function
 *
 * Retrieve the Requester ID range of @pdev for programming its Root
 * Port IDE RID Association registers, and conversely retrieve the
 * Requester ID of the Root Port for programming @pdev's IDE RID
 * Association registers.
 *
 * Allocate a Selective IDE Stream Register Block instance per port.
 *
 * Allocate a platform stream resource from the associated host bridge.
 * Retrieve stream association parameters for Requester ID range and
 * address range restrictions for the stream.
 */
/* [한국어]
 * pci_ide_stream_alloc - 스트림 하나에 필요한 자원을 확보하고 설정을 계산한다
 *
 * @pdev: IDE 능력이 있는 PCIe 엔드포인트의 Physical Function.
 * @return: 준비된 스트림 상태 구조체. 자원이 부족하거나 조건이 맞지 않으면 NULL.
 *   호출자는 pci_ide_stream_free() 또는 pci_ide_stream_release() 로 해제해야 한다.
 *
 * 스트림 수립의 첫 단계다. 하드웨어를 건드리지는 않고, 필요한 자리를
 * 잡아 두고 무엇을 쓸지 계산해 둔다. 실제 레지스터 쓰기는 setup 이 한다.
 *
 * 확보하는 자리가 셋이다(파일 상단의 두 번호 설명 참고).
 *   호스트 브리지의 스트림 자원 — 가장 희소하므로 먼저 잡는다
 *   루트 포트의 Selective 레지스터 블록
 *   엔드포인트의 Selective 레지스터 블록
 * 셋 중 하나라도 실패하면 앞의 것들이 __free() 로 자동 반납된다.
 * goto 로 된 정리 코드가 없는 이유가 이것이다.
 *
 * 계산하는 것은 상류 주석이 밝힌 두 가지다.
 *   RID 범위 — 엔드포인트 쪽 레지스터에는 루트 포트의 RID 를, 루트 포트
 *     쪽에는 엔드포인트의 RID 범위를 적는다. 서로의 것을 적는 것이
 *     파트너 포트 구조의 핵심이다. SR-IOV 를 켠 장치라면 VF 들까지
 *     덮도록 범위를 넓힌다.
 *   주소 범위 — 다운스트림 방향만 거른다. 업스트림(장치가 내는 DMA)은
 *     주소를 미리 알 수 없어 조건을 두지 않는다.
 *
 * Stream ID 는 여기서 정하지 않고 -1(미정)로 둔다. 어떤 번호를 쓸지가
 * 플랫폼의 보안 정책에 달린 문제라 이 계층이 고를 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 할당으로 잠들 수 있다.
 *
 * 에러 경로: 전부 NULL 반환이다. 어느 단계에서 실패했는지는 구분해 주지
 *   않는데, 호출자가 할 수 있는 일이 "포기" 하나뿐이라 구분할 실익이 없다.
 *
 * 호출 체인:
 *   (상위 TSM/TDISP 계층 — 확인해 보면 이 트리에는 아직 호출자가 없다)
 *     → [이 함수] → alloc_stream_index() → ida_alloc_max()
 */
struct pci_ide *pci_ide_stream_alloc(struct pci_dev *pdev)
{
	/* EP, RP, + HB Stream allocation */
	/* [한국어] 세 개의 인덱스 할당 결과를 담을 스택 배열.
	 * 힙이 아니라 스택인 이유는 이 값들이 함수 안에서만 살면 되기
	 * 때문이다. 성공하면 아래에서 struct pci_ide 로 값만 옮겨 담고,
	 * 실패하면 __free 정리가 반납한 뒤 배열은 그냥 사라진다.
	 * 크기가 PCI_IDE_HB + 1 인 것은 그 열거값이 가장 크기 때문이다
	 * (PCI_IDE_EP, PCI_IDE_RP, PCI_IDE_HB 순). */
	struct stream_index __stream[PCI_IDE_HB + 1];
	/* [한국어] prefetchable 메모리 창의 버스 주소 범위. { 0, -1 } 로
	 * 초기화하는 것은 "전 범위" 를 뜻하며, 아래에서 실제 창을 찾지 못하면
	 * 이 값이 그대로 쓰인다. */
	struct pci_bus_region pref_assoc = { 0, -1 };
	/* [한국어] 일반 메모리 창의 버스 주소 범위. 위와 같다. */
	struct pci_bus_region mem_assoc = { 0, -1 };
	/* [한국어] 상위 브리지에서 가져올 두 자원 창. */
	struct resource *mem, *pref;
	/* [한국어] Stream ID 풀을 가진 호스트 브리지. */
	struct pci_host_bridge *hb;
	/* [한국어] rp: 짝이 될 루트 포트. br: 바로 위 브리지.
	 * 둘은 다를 수 있다 — 사이에 스위치가 있으면 br 은 스위치의
	 * 다운스트림 포트이고 rp 는 그보다 위의 루트 포트다. */
	struct pci_dev *rp, *br;
	/* [한국어] SR-IOV VF 개수와, RID 범위의 끝. */
	int num_vf, rid_end;

	/* [한국어] IDE 는 PCIe 기능이다. */
	if (!pci_is_pcie(pdev))
		return NULL;

	/* [한국어] 이 함수는 엔드포인트를 기준으로 스트림을 구성한다.
	 * 루트 포트 쪽 설정도 여기서 함께 계산하므로, 인자로는 엔드포인트만
	 * 받는다. 다른 타입이 들어오면 호출자의 오해이므로 거절한다. */
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return NULL;

	/* [한국어] pci_ide_init() 이 성공했어야 이 값이 채워져 있다.
	 * 0 이면 IDE 를 쓸 수 없는 장치다. */
	if (!pdev->ide_cap)
		return NULL;

	/* [한국어] 스트림 상태를 담을 구조체. __free(kfree) 를 붙였으므로
	 * 아래 어느 지점에서 return 하든 자동으로 해제된다. 성공 경로에서만
	 * return_ptr() 로 소유권을 넘긴다. */
	struct pci_ide *ide __free(kfree) = kzalloc_obj(*ide);
	if (!ide)
		return NULL;

	/* [한국어] Stream ID 와 브리지 스트림 자원의 주인을 찾는다. */
	hb = pci_find_host_bridge(pdev->bus);
	/* [한국어] 첫 번째 할당 — 호스트 브리지의 스트림 자원.
	 * 이것을 가장 먼저 잡는 이유는 가장 희소한 자원이기 때문이다.
	 * 플랫폼이 지원하는 총 스트림 수(기본 256, pci_ide_set_nr_streams 로
	 * 줄일 수 있음)가 상한이고, 그 아래 모든 장치가 나눠 쓴다.
	 * 실패하면 여기서 물러나므로 뒤의 두 할당을 헛되이 하지 않는다. */
	struct stream_index *hb_stream __free(free_stream) = alloc_stream_index(
		&hb->ide_stream_ida, hb->nr_ide_streams, &__stream[PCI_IDE_HB]);
	if (!hb_stream)
		return NULL;

	/* [한국어] 짝이 될 루트 포트를 거슬러 올라가 찾는다. */
	rp = pcie_find_root_port(pdev);
	/* [한국어] 두 번째 할당 — 루트 포트의 레지스터 블록.
	 * 실패하면 위 hb_stream 이 __free 로 자동 반납된다. goto 정리 코드가
	 * 없는 이유가 이것이다. */
	struct stream_index *rp_stream __free(free_stream) = alloc_stream_index(
		&rp->ide_stream_ida, rp->nr_sel_ide, &__stream[PCI_IDE_RP]);
	if (!rp_stream)
		return NULL;

	/* [한국어] 세 번째 할당 — 엔드포인트의 레지스터 블록.
	 * 실패하면 앞의 둘이 모두 자동 반납된다. */
	struct stream_index *ep_stream __free(free_stream) = alloc_stream_index(
		&pdev->ide_stream_ida, pdev->nr_sel_ide, &__stream[PCI_IDE_EP]);
	if (!ep_stream)
		return NULL;

	/* for SR-IOV case, cover all VFs */
	/* [한국어] 상류 주석대로 SR-IOV 를 고려한다. 이 부분이 Selective IDE 의
	 * 실용적 의미를 잘 보여 준다.
	 *
	 * 루트 포트 쪽 레지스터에는 "어느 Requester ID 에서 온 요청을 이
	 * 스트림으로 볼 것인가" 를 범위로 적는다. 그런데 SR-IOV 를 켠 장치는
	 * PF 하나가 아니라 VF 여럿이 각자의 RID 로 요청을 낸다. 그 VF 들의
	 * 트래픽도 같은 스트림으로 보호하려면 범위를 그만큼 넓혀야 한다. */
	num_vf = pci_num_vf(pdev);
	if (num_vf)
		/* [한국어] VF 의 RID 는 PF 다음부터 규칙적으로 배치되므로,
		 * 마지막 VF 의 버스·devfn 을 구해 범위의 끝으로 삼는다.
		 * num_vf - 1 이 마지막 VF 의 인덱스다. */
		rid_end = PCI_DEVID(pci_iov_virtfn_bus(pdev, num_vf - 1),
				    pci_iov_virtfn_devfn(pdev, num_vf - 1));
	else
		/* [한국어] VF 가 없으면 범위는 이 장치 하나다. 시작과 끝이 같다. */
		rid_end = pci_dev_id(pdev);

	/* [한국어] 바로 위 브리지를 찾는다. 아래에서 주소 창을 가져올 곳이다.
	 * 루트 포트(rp)가 아니라 직속 상위(br)를 쓰는 이유는, 이 장치에 실제로
	 * 배정된 주소 범위를 가장 좁게 알려 주는 것이 직속 브리지의 창이기
	 * 때문이다. 스위치가 끼어 있으면 루트 포트의 창은 그보다 넓다. */
	br = pci_upstream_bridge(pdev);
	if (!br)
		return NULL;

	/*
	 * Check if the device consumes memory and/or prefetch-memory. Setup
	 * downstream address association ranges for each.
	 */
	/* [한국어] 브리지의 두 메모리 창을 가져온다. PCI 브리지는 아래로
	 * 내려보낼 주소 범위를 창으로 정의하는데, 일반 메모리와 prefetchable
	 * 메모리가 따로다. prefetchable 은 읽어도 부작용이 없는 영역이라
	 * 캐시나 미리 읽기가 허용되며, 64비트 주소를 쓸 수 있다. */
	mem = pci_resource_n(br, PCI_BRIDGE_MEM_WINDOW);
	pref = pci_resource_n(br, PCI_BRIDGE_PREF_MEM_WINDOW);
	/* [한국어] 창에 실제로 주소가 배정되어 있을 때만 변환한다.
	 * 브리지가 그 종류의 메모리를 안 쓰면 창이 비어 있고, 그때는 위에서
	 * 초기화한 { 0, -1 }(전 범위)이 그대로 남는다. */
	if (resource_assigned(mem))
		/* [한국어] CPU 물리 주소를 PCI 버스 주소로 바꾼다. IDE 레지스터에
		 * 적는 것은 버스 주소이기 때문이다. 두 주소가 같은 아키텍처도
		 * 있지만 다른 곳도 있어(예: 오프셋이 붙는 임베디드 시스템)
		 * 반드시 변환을 거쳐야 한다. */
		pcibios_resource_to_bus(br->bus, &mem_assoc, mem);
	if (resource_assigned(pref))
		pcibios_resource_to_bus(br->bus, &pref_assoc, pref);

	/* [한국어] 여기까지 모든 자원 확보와 계산이 끝났다. 이제 결과를
	 * 한 번에 채운다. 지정 초기화자로 통째로 대입하는 방식이라
	 * 빠뜨린 필드는 0 이 되어, kzalloc 과 함께 초기화 누락을 막는다. */
	*ide = (struct pci_ide) {
		.pdev = pdev,
		.partner = {
			/* [한국어] 엔드포인트 쪽 레지스터에 적을 내용.
			 * 여기서 IDE 의 파트너 구조가 드러난다 — 엔드포인트의
			 * 레지스터에는 상대방인 루트 포트의 RID 를 적는다. */
			[PCI_IDE_EP] = {
				/* [한국어] 엔드포인트가 보기에 이 스트림의 상대는
				 * 루트 포트 하나뿐이므로 시작과 끝이 같다. */
				.rid_start = pci_dev_id(rp),
				.rid_end = pci_dev_id(rp),
				/* [한국어] no_free_ptr() 로 소유권을 가져온다.
				 * 이 호출이 ep_stream 을 NULL 로 만들어, 함수를
				 * 벗어날 때 __free 정리가 아무 일도 하지 않게 한다.
				 * 즉 "이제 이 인덱스는 ide 가 책임진다" 는 선언이며,
				 * 이후 반납은 pci_ide_stream_free() 가 맡는다. */
				.stream_index = no_free_ptr(ep_stream)->stream_index,
				/* Disable upstream address association */
				/* [한국어] 상류 주석대로 업스트림 방향은 주소로
				 * 거르지 않는다. 엔드포인트가 호스트 메모리로
				 * 보내는 DMA 는 어느 주소로 갈지 미리 알 수 없기
				 * 때문이다 — 드라이버가 그때그때 매핑한 주소를
				 * 쓰므로 범위를 못 박을 수 없다.
				 * { 0, -1 } 은 전 범위, 즉 주소 조건을 두지 않는다는 뜻. */
				.mem_assoc = { 0, -1 },
				.pref_assoc = { 0, -1 },
			},
			/* [한국어] 루트 포트 쪽 레지스터에 적을 내용.
			 * 반대로 여기에는 엔드포인트의 RID 를 적는다. */
			[PCI_IDE_RP] = {
				/* [한국어] 범위의 시작은 이 엔드포인트 자신. */
				.rid_start = pci_dev_id(pdev),
				/* [한국어] 끝은 위에서 계산한 값 — VF 가 있으면
				 * 마지막 VF 까지, 없으면 자기 자신. */
				.rid_end = rid_end,
				.stream_index = no_free_ptr(rp_stream)->stream_index,
				/* [한국어] 다운스트림 방향은 주소로 거른다.
				 * 호스트가 장치의 BAR 로 보내는 접근은 그 장치에
				 * 배정된 주소 범위 안이라는 것이 확실하므로,
				 * 그 범위를 적어 두면 다른 주소로 위장한 요청을
				 * 스트림에서 배제할 수 있다. */
				.mem_assoc = mem_assoc,
				.pref_assoc = pref_assoc,
			},
		},
		/* [한국어] 브리지 쪽 자원도 소유권을 넘겨받는다. */
		.host_bridge_stream = no_free_ptr(hb_stream)->stream_index,
		/* [한국어] Stream ID 는 아직 정하지 않았다. -1 은 "미정" 을 뜻하며,
		 * 나중에 상위 계층이 값을 넣고 pci_ide_stream_register() 를 부른다.
		 * 이 파일이 ID 를 스스로 고르지 않는 이유는, 어떤 번호를 쓸지가
		 * 플랫폼의 보안 정책과 펌웨어 협의에 달린 문제이기 때문이다.
		 * pci_ide_stream_register() 가 0..U8_MAX 범위를 검사하므로
		 * -1 인 채로 등록하려 하면 -ENXIO 로 걸린다. */
		.stream_id = -1,
	};

	/* [한국어] 성공했으므로 ide 의 소유권을 호출자에게 넘긴다.
	 * return_ptr() 이 __free(kfree) 를 무력화하고 포인터를 반환한다.
	 * 이제 해제 책임은 호출자에게 있으며, pci_ide_stream_free() 또는
	 * pci_ide_stream_release() 를 불러야 한다. */
	return_ptr(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_alloc);

/**
 * pci_ide_stream_free() - unwind pci_ide_stream_alloc()
 * @ide: idle IDE settings descriptor
 *
 * Free all of the stream index (register block) allocations acquired by
 * pci_ide_stream_alloc(). The stream represented by @ide is assumed to
 * be unregistered and not instantiated in any device.
 */
/* [한국어]
 * pci_ide_stream_free - pci_ide_stream_alloc() 이 잡은 자리 셋을 반납한다
 *
 * @ide: 놀고 있는 스트림 상태. 등록도 설정도 되어 있지 않아야 한다.
 * @return: 없음.
 *
 * alloc 의 정확한 반대다. 인덱스 셋을 각자의 ida 에 돌려주고 구조체를
 * 해제한다. 하드웨어는 건드리지 않는다.
 *
 * 상류 주석의 전제가 중요하다 — 이 스트림은 등록 해제되어 있고 어떤
 * 장치에도 설정되어 있지 않아야 한다. 그렇지 않은 상태에서 부르면
 * 하드웨어는 여전히 켜져 있는데 커널은 그 사실을 잊게 된다.
 * 진행 상태를 모르겠으면 이 함수 대신 pci_ide_stream_release() 를 쓰면
 * 되며, 그쪽이 알아서 필요한 만큼만 되감는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_ide_stream_release() → [이 함수] → ida_free() / kfree()
 */
void pci_ide_stream_free(struct pci_ide *ide)
{
	/* [한국어] 세 자원의 주인을 다시 찾는다. alloc 때 찾았던 것과 같은
	 * 세 곳이며, 그 사이에 토폴로지가 바뀌지 않았음을 전제한다. */
	struct pci_dev *pdev = ide->pdev;
	struct pci_dev *rp = pcie_find_root_port(pdev);
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);

	/* [한국어] 할당의 역순으로 반납한다. ida 반납은 순서에 무관하지만,
	 * 코드를 읽는 사람이 대응 관계를 알아보기 쉽도록 맞춰 두었다. */
	ida_free(&pdev->ide_stream_ida, ide->partner[PCI_IDE_EP].stream_index);
	ida_free(&rp->ide_stream_ida, ide->partner[PCI_IDE_RP].stream_index);
	ida_free(&hb->ide_stream_ida, ide->host_bridge_stream);
	/* [한국어] 마지막으로 구조체 자체를 해제한다. 상류 주석이 밝히듯
	 * 이 시점에 스트림은 등록 해제되어 있고 어떤 장치에도 설정되어 있지
	 * 않아야 한다 — 그렇지 않으면 하드웨어는 여전히 켜져 있는데
	 * 커널은 그 사실을 잊어버리게 된다. */
	kfree(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_free);

/**
 * pci_ide_stream_release() - unwind and release an @ide context
 * @ide: partially or fully registered IDE settings descriptor
 *
 * In support of automatic cleanup of IDE setup routines perform IDE
 * teardown in expected reverse order of setup and with respect to which
 * aspects of IDE setup have successfully completed.
 *
 * Be careful that setup order mirrors this shutdown order. Otherwise,
 * open code releasing the IDE context.
 */
/* [한국어]
 * pci_ide_stream_release - 진행된 만큼만 알아서 되감아 정리한다
 *
 * @ide: 일부만 설정됐거나 완전히 설정된 스트림 상태.
 * @return: 없음.
 *
 * 스트림 수립은 여러 단계라 어디서든 실패할 수 있다. 단계마다 다른
 * 정리 코드를 두는 대신, 이 함수 하나가 구조체 안의 플래그를 보고
 * 필요한 것만 역순으로 되감는다.
 *
 * 판단에 쓰는 표시가 셋이다.
 *   partner[].enable — pci_ide_stream_enable() 이 세운다
 *   partner[].setup  — pci_ide_stream_setup() 이 세운다
 *   ide->name        — pci_ide_stream_register() 가 채운다
 *
 * 상류 주석의 경고를 새겨야 한다. 설정 순서가 이 해제 순서를 거울처럼
 * 반영해야 하며, 그렇지 않으면 이 자동 되감기가 틀린다. 순서가 다른
 * 흐름을 만들었다면 이 함수를 쓰지 말고 직접 풀어야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 접근이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   (상위 TSM/TDISP 계층) → [이 함수]
 *     → pci_ide_stream_disable() → _teardown() → _unregister() → _free()
 */
void pci_ide_stream_release(struct pci_ide *ide)
{
	/* [한국어] 양쪽 파트너를 각각 정리해야 하므로 둘 다 찾아 둔다. */
	struct pci_dev *pdev = ide->pdev;
	struct pci_dev *rp = pcie_find_root_port(pdev);

	/* [한국어] 이 함수의 요령은 "어디까지 진행됐는지" 를 구조체 안의
	 * 플래그로 알아내는 것이다. 설정 과정이 여러 단계라 중간에 실패할 수
	 * 있는데, 그때마다 다른 정리 코드를 쓰는 대신 이 함수 하나가
	 * 플래그를 보고 필요한 것만 되감는다.
	 *
	 * 그래서 상류 주석이 "설정 순서가 이 해제 순서를 거울처럼 반영하는지
	 * 주의하라" 고 경고한다. 순서가 어긋나면 이 자동 되감기가 틀린다.
	 *
	 * 먼저 활성화를 끈다. 링크 양쪽 중 루트 포트를 먼저 끄는 것이
	 * 안전하다 — 호스트 쪽에서 먼저 스트림을 닫으면 그 뒤 엔드포인트를
	 * 만지는 접근이 보호되지 않은 채로 나가지 않는다. */
	if (ide->partner[PCI_IDE_RP].enable)
		pci_ide_stream_disable(rp, ide);

	/* [한국어] 이어서 엔드포인트 쪽 활성화를 끈다. */
	if (ide->partner[PCI_IDE_EP].enable)
		pci_ide_stream_disable(pdev, ide);

	/* [한국어] 그다음 레지스터 내용을 지운다. disable 은 Control 만 끄고
	 * 나머지 설정은 남겨 두는 반면, teardown 은 RID·주소 연관까지 전부
	 * 0 으로 되돌린다. 둘을 나눈 이유는 잠시 껐다 켜는 경우
	 * (예: 링크 재트레이닝) 설정을 다시 쓰지 않아도 되게 하려는 것이다. */
	if (ide->partner[PCI_IDE_RP].setup)
		pci_ide_stream_teardown(rp, ide);

	if (ide->partner[PCI_IDE_EP].setup)
		pci_ide_stream_teardown(pdev, ide);

	/* [한국어] sysfs 노출과 Stream ID 예약을 되돌린다.
	 * name 이 NULL 이 아니라는 것이 곧 "등록까지 성공했다" 는 표시다.
	 * 별도의 불리언 대신 이미 있는 포인터를 그 신호로 쓴다. */
	if (ide->name)
		pci_ide_stream_unregister(ide);

	/* [한국어] 마지막으로 인덱스 셋을 반납하고 구조체를 해제한다.
	 * 이것만은 조건이 없다 — alloc 이 성공했다면 반드시 셋 다 잡혀 있다. */
	pci_ide_stream_free(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_release);

/* [한국어]
 * struct pci_ide_stream_id - 예약한 Stream ID 를 되돌릴 수 있게 묶은 것
 *
 * 앞의 struct stream_index 와 같은 발상이다. 아래 DEFINE_FREE 와 짝을
 * 이뤄, pci_ide_stream_register() 가 중간에 실패하면 예약한 Stream ID 가
 * 자동으로 반납되게 한다.
 */
struct pci_ide_stream_id {
	struct pci_host_bridge *hb;
	/* [한국어] 이 Stream ID 를 빌려 준 호스트 브리지.
	 * 반납할 ida 가 hb->ide_stream_ids_ida 이므로 브리지를 들고 있어야 한다.
	 * 설정자: request_stream_id() 가 예약 성공 직후 채운다.
	 * 읽는 자: DEFINE_FREE(free_stream_id) 의 자동 정리 코드.
	 * 값 범위: 유효한 호스트 브리지 포인터.
	 * 동기화: ida 가 내부 잠금을 갖는다. 이 포인터는 함수 안에서만 쓰인다. */

	u8 stream_id;
	/* [한국어] 예약한 8비트 Stream ID.
	 * 설정자: request_stream_id().
	 * 읽는 자: 자동 정리 코드가 반납할 때.
	 * 값 범위: 0~255. 다만 PCI_IDE_RESERVED_STREAM_ID 는 브리지 초기화 때
	 *   미리 잡혀 있어 여기로 들어올 수 없다.
	 * 동기화: 위와 같다. */
};

/* [한국어]
 * request_stream_id - Stream ID 를 예약하고 되돌릴 수 있는 형태로 감싼다
 *
 * @hb: 예약 대상 호스트 브리지.
 * @stream_id: 상위 계층이 정한 Stream ID.
 * @sid: 결과를 채울 그릇(호출자의 스택).
 * @return: 성공하면 @sid, 이미 쓰이는 번호면 NULL.
 *
 * reserve_stream_id() 를 그대로 부르되, 결과를 __free() 가 알아볼 수 있는
 * 모양으로 담아 준다. 앞의 alloc_stream_index() 와 같은 패턴이다.
 *
 * 차이가 하나 있다. 인덱스 쪽은 "빈 것 아무거나" 지만 Stream ID 는
 * 상위 계층이 지정한 번호여야 한다. 어떤 번호를 쓸지가 플랫폼의 보안
 * 정책에 달린 문제라 커널이 임의로 고를 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_ide_stream_register() → [이 함수] → reserve_stream_id()
 */
static struct pci_ide_stream_id *
request_stream_id(struct pci_host_bridge *hb, u8 stream_id,
		  struct pci_ide_stream_id *sid)
{
	/* [한국어] 그 번호를 브리지 범위에서 잡아 본다. 이미 쓰이고 있으면 실패. */
	if (!reserve_stream_id(hb, stream_id))
		return NULL;

	/* [한국어] 반납에 필요한 두 가지를 함께 기록한다. */
	*sid = (struct pci_ide_stream_id) {
		.hb = hb,
		.stream_id = stream_id,
	};

	/* [한국어] 호출자가 준 포인터를 그대로 돌려준다. 호출자는 이것을
	 * __free(free_stream_id) 변수에 담아 실패 시 자동 반납되게 한다. */
	return sid;
}
/* [한국어] __free(free_stream_id) 변수가 스코프를 벗어날 때 Stream ID 를
 * 반납하게 한다. 위 struct stream_index 쪽과 달리 별도의 헬퍼 함수 없이
 * 매크로 안에서 ida_free() 를 직접 부른다 — 반납할 ida 가
 * _T->hb->ide_stream_ids_ida 하나로 정해져 있어 한 줄로 표현되기 때문이다.
 * _T 가 NULL 인지 보는 이유는 예약 실패로 NULL 인 경우와,
 * retain_and_null_ptr() 로 소유권을 넘겨 일부러 NULL 로 만든 경우 때문이다. */
DEFINE_FREE(free_stream_id, struct pci_ide_stream_id *,
	    if (_T) ida_free(&_T->hb->ide_stream_ids_ida, _T->stream_id))

/**
 * pci_ide_stream_register() - Prepare to activate an IDE Stream
 * @ide: IDE settings descriptor
 *
 * After a Stream ID has been acquired for @ide, record the presence of
 * the stream in sysfs. The expectation is that @ide is immutable while
 * registered.
 */
int pci_ide_stream_register(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	/* [한국어] Stream ID 풀과 sysfs 를 걸 대상이 되는 브리지. */
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);
	/* [한국어] Stream ID 예약 결과를 담을 그릇. 스택에 두는 이유는
	 * 앞의 alloc 함수와 같다 — 함수 안에서만 살면 되기 때문이다. */
	struct pci_ide_stream_id __sid;
	/* [한국어] sysfs 이름을 만들 때 쓸 두 인덱스. */
	u8 ep_stream, rp_stream;
	int rc;

	/* [한국어] Stream ID 가 유효한 범위인지 먼저 본다.
	 * pci_ide_stream_alloc() 이 -1 로 두고 나가므로, 상위 계층이 값을
	 * 넣지 않고 이 함수를 부르면 여기서 걸린다. 그것이 이 검사의 주된
	 * 목적이다 — 미정 상태로 등록되는 것을 막는 것.
	 * ide->stream_id 가 부호 있는 정수라 위아래를 모두 확인해야 한다. */
	if (ide->stream_id < 0 || ide->stream_id > U8_MAX) {
		pci_err(pdev, "Setup fail: Invalid Stream ID: %d\n", ide->stream_id);
		return -ENXIO;
	}

	/* [한국어] 그 번호를 브리지 범위에서 예약한다. __free 를 붙였으므로
	 * 아래에서 실패해 돌아가면 자동으로 반납된다. */
	struct pci_ide_stream_id *sid __free(free_stream_id) =
		request_stream_id(hb, ide->stream_id, &__sid);
	if (!sid) {
		/* [한국어] 이미 다른 스트림이 쓰는 번호다. Stream ID 는 브리지
		 * 범위에서 유일해야 하므로 겹치면 안 된다. */
		pci_err(pdev, "Setup fail: Stream ID %d in use\n", ide->stream_id);
		return -EBUSY;
	}

	/* [한국어] sysfs 이름에 넣을 두 인덱스를 꺼낸다. */
	ep_stream = ide->partner[PCI_IDE_EP].stream_index;
	rp_stream = ide->partner[PCI_IDE_RP].stream_index;
	/* [한국어] "stream<브리지>.<루트포트>.<엔드포인트>" 형태의 이름을 만든다.
	 * 세 인덱스를 모두 넣는 이유는 이것이 스트림 하나를 유일하게
	 * 가리키는 조합이기 때문이다. Stream ID 를 이름에 쓰지 않는 것이
	 * 눈에 띄는데, ID 는 나중에 바뀔 수 있는 정책적 값인 반면 이 세
	 * 인덱스는 스트림의 물리적 자리를 나타내기 때문이다.
	 * __free(kfree) 라 실패 시 자동 해제된다. */
	const char *name __free(kfree) = kasprintf(GFP_KERNEL, "stream%d.%d.%d",
						   ide->host_bridge_stream,
						   rp_stream, ep_stream);
	if (!name)
		return -ENOMEM;

	/* [한국어] 호스트 브리지 디렉터리 아래에 이 장치를 가리키는 심볼릭
	 * 링크를 만든다. 사용자 공간에서 브리지 아래를 훑으면 어떤 스트림이
	 * 어느 장치에 걸려 있는지 알 수 있다. 실패하면 sid 와 name 이
	 * 자동 정리된다. */
	rc = sysfs_create_link(&hb->dev.kobj, &pdev->dev.kobj, name);
	if (rc)
		return rc;

	/* [한국어] 이름의 소유권을 ide 로 넘긴다. 이후 해제는
	 * pci_ide_stream_unregister() 가 한다. 동시에 이 필드가 NULL 이
	 * 아니게 되는 것이 "등록 완료" 의 표시가 된다 —
	 * pci_ide_stream_release() 가 그것을 보고 판단한다. */
	ide->name = no_free_ptr(name);

	/* Stream ID reservation recorded in @ide is now successfully registered */
	/* [한국어] 상류 주석대로, 이제 Stream ID 예약의 책임이 ide 로 넘어갔다.
	 * retain_and_null_ptr() 은 자동 정리를 끄되(sid 를 NULL 로 만들되)
	 * 예약 자체는 유지한다. no_free_ptr() 과 달리 값을 쓰지 않고 버리는
	 * 경우에 쓰는 것으로, 여기서는 예약 정보가 이미 ide->stream_id 에
	 * 들어 있어 sid 를 더 볼 필요가 없기 때문이다.
	 * 반납은 pci_ide_stream_unregister() 가 맡는다. */
	retain_and_null_ptr(sid);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_register);

/**
 * pci_ide_stream_unregister() - unwind pci_ide_stream_register()
 * @ide: idle IDE settings descriptor
 *
 * In preparation for freeing @ide, remove sysfs enumeration for the
 * stream.
 */
/* [한국어]
 * pci_ide_stream_unregister - pci_ide_stream_register() 를 되돌린다
 *
 * @ide: 등록된 스트림 상태.
 * @return: 없음.
 *
 * sysfs 링크를 없애고, 이름을 해제하고, Stream ID 를 브리지 풀에 반납한다.
 * 세 가지가 한 덩어리인 이유는 register 가 그 셋을 함께 잡았기 때문이다.
 *
 * 순서에 이유가 있다. sysfs 링크를 먼저 지워야 그 뒤 이름을 해제할 때
 * sysfs 가 해제된 문자열을 참조하지 않는다. 마지막에 name 을 NULL 로
 * 되돌리는 것도 중요한데, 그것이 "등록되지 않음" 의 표시라
 * pci_ide_stream_release() 가 두 번 부르지 않게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_ide_stream_release() → [이 함수] → sysfs_remove_link() / ida_free()
 */
void pci_ide_stream_unregister(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);

	/* [한국어] sysfs 링크를 먼저 없앤다. 사용자 공간이 더는 이 스트림을
	 * 볼 수 없게 하는 것이 우선이다 — 아래에서 이름을 해제하기 전에
	 * 링크를 지워야 sysfs 가 해제된 문자열을 참조하지 않는다. */
	sysfs_remove_link(&hb->dev.kobj, ide->name);
	/* [한국어] register 에서 kasprintf 로 만든 이름을 해제한다. */
	kfree(ide->name);
	/* [한국어] Stream ID 를 브리지 풀에 반납한다. 이제 다른 스트림이
	 * 그 번호를 쓸 수 있다. */
	ida_free(&hb->ide_stream_ids_ida, ide->stream_id);
	/* [한국어] name 을 NULL 로 되돌린다. 이것이 "등록되지 않음" 의 표시라,
	 * pci_ide_stream_release() 가 두 번 부르는 일이 없게 된다.
	 * 해제 후 NULL 로 만드는 순서가 중요하다 — 위 kfree 뒤에 남겨 두면
	 * 해제된 포인터가 남아 use-after-free 의 빌미가 된다. */
	ide->name = NULL;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_unregister);

/* [한국어]
 * pci_ide_domain - RID 연관 레지스터에 적을 세그먼트 번호를 정한다
 *
 * @pdev: 대상 장치. 루트 포트이거나 엔드포인트다.
 * @return: 세그먼트를 포착하는 장치면 이 장치의 PCI 도메인 번호, 아니면 0.
 *
 * IDE 의 RID 연관 레지스터에는 Requester ID 와 함께 세그먼트(도메인)
 * 번호를 적는 자리가 있다. 그런데 그 자리를 언제 쓸 수 있는지가 조건부다.
 *
 * 판단 근거는 pdev->fm_enabled 인데, 그 값이 어디서 오는지 확인해 보면
 * probe.c:5515 의
 *     pdev->fm_enabled = !!(val & PCI_DEV3_STA_SEGMENT);
 * 이다. 즉 Device 3 Status 레지스터의 Segment Captured 비트이며,
 * include/linux/pci.h 의 필드 주석은 이것을 "Flit Mode (segment captured)"
 * 라고 적고 있다. 요컨대 그 장치가 요청의 세그먼트 번호를 포착해
 * 비교에 쓸 수 있느냐를 나타낸다.
 *
 * 포착하지 못하는 장치에 0 이 아닌 값을 적으면 어떤 요청과도 맞지 않아
 * 스트림이 아무것도 보호하지 못한 채 조용히 놀게 된다. 그래서 0 을 쓴다.
 * 여러 도메인을 가진 시스템에서 이 구분이 필요해진다.
 *
 * 참고: 이 트리는 부분 체크아웃이라 include/linux/pci_regs.h 가 없어
 * PCI_DEV3_STA_SEGMENT 의 실제 비트 위치는 확인하지 못했다. 다만
 * probe.c 의 기존 주석은 bit3(0x8)이라고 적고 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 조회.
 *
 * 호출 체인:
 *   pci_ide_stream_setup() → pci_ide_stream_to_regs() → [이 함수]
 */
static int pci_ide_domain(struct pci_dev *pdev)
{
	/* [한국어] Device 3 Status 의 Segment Captured 비트가 서 있는 장치.
	 * 세그먼트를 비교에 쓸 수 있으므로 실제 도메인 번호를 적는다. */
	if (pdev->fm_enabled)
		return pci_domain_nr(pdev->bus);
	/* [한국어] 그렇지 않으면 0. 실제 도메인 번호를 적으면 들어오는 요청과
	 * 영영 맞지 않아 스트림이 아무것도 보호하지 못한다. */
	return 0;
}

/* [한국어]
 * pci_ide_to_settings - pdev 가 링크의 어느 쪽인지 보고 해당 설정을 골라 준다
 *
 * @pdev: 설정을 적용할 대상. 엔드포인트이거나 그 짝인 루트 포트다.
 * @ide: 스트림 상태. 두 파트너의 설정이 모두 들어 있다.
 * @return: 그 포트에 해당하는 파트너 설정. 짝이 맞지 않으면 NULL.
 *
 * 이 파일의 설정·해제 함수들은 전부 (pdev, ide) 한 쌍을 받는다.
 * 엔드포인트와 루트 포트 양쪽에 같은 함수를 한 번씩 부르는 방식이라,
 * 함수 안에서 "지금 어느 쪽을 다루는 중인가" 를 판단해야 한다.
 * 그 판단을 한곳에 모은 것이 이 함수다.
 *
 * 그냥 타입만 보고 고르지 않고 실제로 그 스트림의 짝이 맞는지까지
 * 확인하는 점이 중요하다. 엔드포인트라면 ide->pdev 와 같아야 하고,
 * 루트 포트라면 ide->pdev 의 루트 포트와 같아야 한다. 다른 장치를
 * 넘기면 엉뚱한 하드웨어에 설정을 쓰게 되므로 그 전에 막는다.
 *
 * 경고에 pci_warn_once 를 쓰는 이유는 이런 실수가 호출자의 코드 오류라
 * 한 번 나면 계속 나기 때문이다. 로그를 채우지 않으면서도 알려 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 조회라 부수효과가 없다.
 *
 * 에러 경로: NULL 을 돌려주면 호출자들은 하나같이 조용히 물러난다.
 *   void 함수가 많아 오류를 전할 방법이 없기 때문이며, 대신 여기서
 *   경고를 남겨 원인을 알 수 있게 한다.
 *
 * 호출 체인:
 *   pci_ide_stream_setup() / _teardown() / _enable() / _disable() /
 *   pci_ide_stream_to_regs() → [이 함수]
 */
struct pci_ide_partner *pci_ide_to_settings(struct pci_dev *pdev, struct pci_ide *ide)
{
	/* [한국어] IDE 는 PCIe 기능이므로 구형 PCI 장치가 오면 잘못된 호출이다. */
	if (!pci_is_pcie(pdev)) {
		pci_warn_once(pdev, "not a PCIe device\n");
		return NULL;
	}

	/* [한국어] PCIe 장치 타입으로 어느 쪽인지 가른다. */
	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
		/* [한국어] 엔드포인트라면 이 스트림의 주인공과 같아야 한다.
		 * 다르면 다른 장치의 스트림 설정을 이 장치에 쓰려는 것이다. */
		if (pdev != ide->pdev) {
			pci_warn_once(pdev, "setup expected Endpoint: %s\n", pci_name(ide->pdev));
			return NULL;
		}
		/* [한국어] 엔드포인트 쪽 설정. 상대인 루트 포트의 RID 가 들어 있다. */
		return &ide->partner[PCI_IDE_EP];
	case PCI_EXP_TYPE_ROOT_PORT: {
		/* [한국어] 이 스트림의 짝이 되는 루트 포트를 다시 계산해 본다. */
		struct pci_dev *rp = pcie_find_root_port(ide->pdev);

		/* [한국어] 넘어온 루트 포트가 그 짝이 아니면 거절한다.
		 * 시스템에 루트 포트가 여럿이므로 실수할 여지가 있다. */
		if (pdev != rp) {
			pci_warn_once(pdev, "setup expected Root Port: %s\n",
				      pci_name(rp));
			return NULL;
		}
		/* [한국어] 루트 포트 쪽 설정. 상대인 엔드포인트의 RID 범위와
		 * 다운스트림 주소 범위가 들어 있다. */
		return &ide->partner[PCI_IDE_RP];
	}
	default:
		/* [한국어] 스위치 포트나 브리지 등은 IDE 스트림의 끝점이 될 수 없다.
		 * IDE 는 링크 양 끝의 두 포트가 짝을 이루는 구조이기 때문이다. */
		pci_warn_once(pdev, "invalid device type\n");
		return NULL;
	}
}
EXPORT_SYMBOL_GPL(pci_ide_to_settings);

/* [한국어]
 * set_ide_sel_ctl - Selective IDE Stream Control 레지스터를 한 번에 구성한다
 *
 * @pdev: 대상 포트.
 * @ide: 스트림 상태. Stream ID 를 여기서 가져온다.
 * @settings: 이 포트의 파트너 설정. default_stream 플래그를 가져온다.
 * @pos: 이 스트림 블록의 config 오프셋.
 * @enable: 활성화 비트를 세울지 여부.
 *
 * Control 레지스터의 다섯 필드를 한 번의 쓰기로 정한다. 읽어서 고치지
 * 않고 통째로 덮어쓰는데, 이 레지스터의 모든 필드를 이 함수가 다 알고
 * 있어서 남길 것이 없기 때문이다.
 *
 * 다섯 필드의 뜻:
 *   ID          — 이 스트림의 Stream ID. 링크 위 패킷에 실리는 값이다.
 *   DEFAULT     — 이 스트림을 기본 스트림으로 삼을지. 어느 스트림에도
 *                 해당하지 않는 트래픽을 여기로 보낸다.
 *   CFG_EN      — config 요청까지 이 스트림으로 보호할지.
 *                 장치가 지원할 때만(pdev->ide_cfg) 켤 수 있다.
 *   TEE_LIMITED — 신뢰 실행 환경에 속한 요청만 허용할지.
 *                 기밀 컴퓨팅에서 중요한 비트다.
 *   EN          — 스트림 활성화.
 *
 * CFG_EN 과 TEE_LIMITED 를 settings 가 아니라 pdev 에서 읽는 점이
 * 눈에 띈다. 이 둘은 스트림별 정책이 아니라 장치의 능력이라,
 * pci_ide_init() 이 capability 에서 읽어 pdev 에 적어 둔 값을 그대로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 쓰기가 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_ide_stream_setup()  → [이 함수]  (enable=false 로 설정만)
 *   pci_ide_stream_enable() → [이 함수]  (enable=true 로 활성화)
 */
static void set_ide_sel_ctl(struct pci_dev *pdev, struct pci_ide *ide,
			    struct pci_ide_partner *settings, int pos,
			    bool enable)
{
	/* [한국어] 다섯 필드를 각자의 비트 자리에 넣어 하나의 워드로 합친다.
	 * FIELD_PREP 이 마스크에 맞춰 시프트해 주므로 비트 위치를 손으로
	 * 계산하지 않아도 되고, 값이 자리를 넘으면 컴파일 시점에 걸린다. */
	u32 val = FIELD_PREP(PCI_IDE_SEL_CTL_ID, ide->stream_id) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_DEFAULT, settings->default_stream) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_CFG_EN, pdev->ide_cfg) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_TEE_LIMITED, pdev->ide_tee_limit) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_EN, enable);

	/* [한국어] 한 번의 쓰기로 반영한다. 활성화 비트가 다른 필드와 함께
	 * 들어가므로, 켜는 순간 나머지 설정이 확실히 자리 잡은 상태가 된다 —
	 * 켜 놓고 나중에 ID 를 바꾸는 식의 위험한 중간 상태가 없다. */
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, val);
}

/* [한국어] 주소의 하위 절반(비트 31..20)만 뽑는 마스크.
 * 왜 20비트부터인가 — IDE 의 주소 연관은 1MB 단위로만 지정할 수 있기
 * 때문이다. 2^20 = 1MB 이므로 그보다 아래 비트는 레지스터에 자리가 없고,
 * 범위는 항상 1MB 경계에 맞춰진다. PCI 브리지의 메모리 창도 같은 단위라
 * 잘 맞는다. */
#define SEL_ADDR1_LOWER GENMASK(31, 20)
/* [한국어] 주소의 상위 32비트를 뽑는 마스크. 64비트 주소를 레지스터
 * 셋에 나눠 담기 때문에 필요하다. GENMASK_ULL 을 쓰는 것은 32비트
 * 아키텍처에서도 64비트 연산이 되게 하려는 것이다. */
#define SEL_ADDR_UPPER GENMASK_ULL(63, 32)
/* [한국어] 주소 연관 레지스터 1번을 만드는 매크로.
 * 이 레지스터 하나에 세 가지가 함께 들어간다.
 *   VALID     — 이 주소 연관 블록이 유효함을 표시. 항상 1 로 쓴다.
 *   BASE_LOW  — 시작 주소의 비트 31..20
 *   LIMIT_LOW — 끝 주소의 비트 31..20
 * 시작과 끝의 상위 32비트는 각각 레지스터 3번과 2번에 따로 들어간다
 * (아래 mem_assoc_to_regs 참고). 하나의 범위가 레지스터 셋에 흩어지는
 * 셈인데, 레지스터 폭이 32비트인데 64비트 주소 두 개를 담아야 해서다.
 *
 * 매크로 안에서 FIELD_GET 으로 뽑아 FIELD_PREP 으로 다시 넣는 것이
 * 돌아가는 것처럼 보이지만, 원래 주소에서 필요한 비트만 꺼내(GET)
 * 레지스터의 다른 자리에 넣는(PREP) 것이라 두 단계가 다 필요하다. */
#define PREP_PCI_IDE_SEL_ADDR1(base, limit)			\
	(FIELD_PREP(PCI_IDE_SEL_ADDR_1_VALID, 1) |		\
	 FIELD_PREP(PCI_IDE_SEL_ADDR_1_BASE_LOW,		\
		    FIELD_GET(SEL_ADDR1_LOWER, (base))) |	\
	 FIELD_PREP(PCI_IDE_SEL_ADDR_1_LIMIT_LOW,		\
		    FIELD_GET(SEL_ADDR1_LOWER, (limit))))

/* [한국어]
 * mem_assoc_to_regs - 주소 범위 하나를 레지스터 세 개의 값으로 변환한다
 *
 * @region: 변환할 버스 주소 범위.
 * @regs: 결과를 담을 곳.
 * @idx: 몇 번째 주소 연관 블록인지.
 * @return: 없음.
 *
 * 64비트 주소 두 개(시작과 끝)를 32비트 레지스터 셋에 나눠 담는다.
 * 배치가 조금 특이하다.
 *   assoc1 — 시작과 끝의 하위 부분(비트 31..20)을 함께, 그리고 VALID 비트
 *   assoc2 — 끝 주소의 상위 32비트
 *   assoc3 — 시작 주소의 상위 32비트
 * 끝이 시작보다 앞에 오는 것은 규격이 그렇게 정해서다.
 *
 * 이 함수는 값을 계산만 하고 레지스터에 쓰지는 않는다. 실제 쓰기는
 * pci_ide_stream_setup() 이 한다. 계산과 쓰기를 나눈 덕에 설정값을
 * 검증하거나 시험하기 쉬워진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산.
 *
 * 호출 체인:
 *   pci_ide_stream_setup() → pci_ide_stream_to_regs() → [이 함수]
 */
static void mem_assoc_to_regs(struct pci_bus_region *region,
			      struct pci_ide_regs *regs, int idx)
{
	/* convert to u64 range for bitfield size checks */
	/* [한국어] 상류 주석대로 struct range 로 옮겨 담는다. pci_bus_region 의
	 * 필드는 아키텍처에 따라 32비트일 수 있는데, 아래 FIELD_GET 이
	 * 64비트 마스크(SEL_ADDR_UPPER)를 쓰므로 폭이 맞아야 컴파일 시점의
	 * 크기 검사를 통과한다. */
	struct range r = { region->start, region->end };

	/* [한국어] 시작과 끝의 하위 부분을 VALID 비트와 함께 한 워드로 만든다. */
	regs->addr[idx].assoc1 = PREP_PCI_IDE_SEL_ADDR1(r.start, r.end);
	/* [한국어] 끝 주소의 상위 32비트. */
	regs->addr[idx].assoc2 = FIELD_GET(SEL_ADDR_UPPER, r.end);
	/* [한국어] 시작 주소의 상위 32비트. */
	regs->addr[idx].assoc3 = FIELD_GET(SEL_ADDR_UPPER, r.start);
}

/**
 * pci_ide_stream_to_regs() - convert IDE settings to association register values
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered IDE settings descriptor
 * @regs: output register values
 */
/* [한국어]
 * pci_ide_stream_to_regs - 스트림 설정을 레지스터에 쓸 값으로 변환한다
 *
 * @pdev: 루트 포트이거나 엔드포인트. 어느 쪽이냐에 따라 다른 설정이 쓰인다.
 * @ide: 등록된 스트림 상태.
 * @regs: 계산 결과를 담을 곳. 호출자의 스택에 있다.
 * @return: 없음. 결과는 @regs 에 담긴다.
 *
 * 계산과 하드웨어 쓰기를 나눈 것이 이 함수의 존재 이유다. 값을 다 구해
 * 놓고 나서 한꺼번에 쓰면, 쓰는 도중에 실패할 여지가 없고 설정값을
 * 따로 검증하거나 시험하기도 쉽다.
 *
 * 채우는 것이 둘이다.
 *   RID 연관 — 어느 Requester ID 범위의 요청을 이 스트림으로 볼지.
 *   주소 연관 — 어느 주소 범위를 이 스트림으로 볼지. 다운스트림만 해당하며,
 *     장치가 가진 블록 수만큼만 채울 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산이라 부수효과가 없다.
 *
 * 호출 체인:
 *   pci_ide_stream_setup() → [이 함수]
 *     → pci_ide_to_settings() / pci_ide_domain() / mem_assoc_to_regs()
 */
static void pci_ide_stream_to_regs(struct pci_dev *pdev, struct pci_ide *ide,
				   struct pci_ide_regs *regs)
{
	/* [한국어] 이 포트가 링크의 어느 쪽인지 보고 해당 설정을 고른다. */
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	/* [한국어] 다음에 채울 주소 연관 블록의 번호. 두 창(일반/prefetch)이
	 * 있지만 둘 다 있으리라는 보장이 없어, 실제로 채운 만큼만 세어 나간다. */
	int assoc_idx = 0;

	/* [한국어] 전체를 0 으로 밀고 시작한다. 아래에서 채우지 않은 필드가
	 * 쓰레기 값으로 남으면 안 되기 때문이다. 특히 settings 가 NULL 이라
	 * 곧바로 돌아가는 경우, 호출자는 이 구조체를 그대로 쓰게 되므로
	 * 0 으로 채워져 있어야 안전하다(nr_addr = 0 이라 아무것도 쓰지 않는다). */
	memset(regs, 0, sizeof(*regs));

	/* [한국어] 짝이 맞지 않는 장치였다면 경고는 이미 나갔다. 여기서는
	 * 조용히 돌아간다 — void 함수라 오류를 전할 방법이 없다. */
	if (!settings)
		return;

	/* [한국어] RID 연관 레지스터 1번: 범위의 끝. */
	regs->rid1 = FIELD_PREP(PCI_IDE_SEL_RID_1_LIMIT, settings->rid_end);

	/* [한국어] RID 연관 레지스터 2번: 유효 표시, 범위의 시작, 세그먼트.
	 * VALID 를 1 로 두어야 이 RID 조건이 실제로 적용된다.
	 * 세그먼트는 pci_ide_domain() 이 정한다 — 장치가 세그먼트를 포착하지
	 * 못하면 0 이 들어간다(그 함수 주석 참고). */
	regs->rid2 = FIELD_PREP(PCI_IDE_SEL_RID_2_VALID, 1) |
		     FIELD_PREP(PCI_IDE_SEL_RID_2_BASE, settings->rid_start) |
		     FIELD_PREP(PCI_IDE_SEL_RID_2_SEG, pci_ide_domain(pdev));

	/* [한국어] 일반 메모리 창을 첫 번째 주소 연관 블록에 넣는다.
	 * 두 조건을 함께 보는 이유:
	 *   nr_ide_mem — 이 장치에 주소 연관 블록이 하나라도 있는가.
	 *     없는 장치도 있으며, 그때는 RID 로만 거른다.
	 *   size       — 그 창에 실제로 범위가 있는가. 엔드포인트 쪽 설정은
	 *     { 0, -1 } 이라 크기가 0 으로 계산되어 여기서 걸러진다.
	 *     업스트림 방향은 주소로 거르지 않기 때문이다. */
	if (pdev->nr_ide_mem && pci_bus_region_size(&settings->mem_assoc)) {
		mem_assoc_to_regs(&settings->mem_assoc, regs, assoc_idx);
		assoc_idx++;
	}

	/* [한국어] prefetchable 창을 다음 블록에 넣는다.
	 * nr_ide_mem > assoc_idx 로 비교하는 이유는 블록이 남아 있는지
	 * 확인하기 위해서다. 장치가 블록을 하나만 가졌다면 위에서 이미 다
	 * 썼으므로 prefetch 창은 보호하지 못한 채 넘어간다 —
	 * 하드웨어 자원의 한계이며 오류는 아니다. */
	if (pdev->nr_ide_mem > assoc_idx &&
	    pci_bus_region_size(&settings->pref_assoc)) {
		mem_assoc_to_regs(&settings->pref_assoc, regs, assoc_idx);
		assoc_idx++;
	}

	/* [한국어] 실제로 채운 블록 수를 알린다. 호출자는 이 수만큼만 쓰고,
	 * 나머지 블록은 0 으로 지운다. */
	regs->nr_addr = assoc_idx;
}

/**
 * pci_ide_stream_setup() - program settings to Selective IDE Stream registers
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered IDE settings descriptor
 *
 * When @pdev is a PCI_EXP_TYPE_ENDPOINT then the PCI_IDE_EP partner
 * settings are written to @pdev's Selective IDE Stream register block,
 * and when @pdev is a PCI_EXP_TYPE_ROOT_PORT, the PCI_IDE_RP settings
 * are selected.
 */
/* [한국어]
 * pci_ide_stream_setup - 계산한 설정을 실제 레지스터에 기록한다
 *
 * @pdev: 루트 포트이거나 엔드포인트.
 * @ide: 등록된 스트림 상태.
 * @return: 없음. 실패를 알릴 방법이 없으므로 조건이 맞지 않으면 조용히 물러난다.
 *
 * 상류 주석대로 pdev 가 엔드포인트면 PCI_IDE_EP 설정이, 루트 포트면
 * PCI_IDE_RP 설정이 쓰인다. 즉 이 함수는 스트림 하나당 두 번 불러야
 * 한다 — 링크의 양 끝을 각각 설정해야 하기 때문이다.
 *
 * 쓰는 순서가 RID → 주소 연관 → 남는 블록 지우기 → Control 이다.
 * Control 을 마지막에 두되 enable=false 로 두는 것이 요점인데,
 * 그 이유는 함수 안의 해당 주석에 적었다(키 교환 전에 Stream ID 가
 * 레지스터에 있기를 기대하는 장치가 있다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 쓰기가 여러 번 있어 잠들 수 있다.
 *
 * 에러 경로: 짝이 맞지 않는 pdev 면 pci_ide_to_settings() 가 경고를 남기고
 *   NULL 을 주며, 이 함수는 아무것도 하지 않고 돌아간다. settings->setup 이
 *   1 이 되지 않으므로 release 도 teardown 을 건너뛴다.
 *
 * 호출 체인:
 *   (상위 TSM/TDISP 계층) → [이 함수]
 *     → pci_ide_stream_to_regs() → set_ide_sel_ctl()
 */
void pci_ide_stream_setup(struct pci_dev *pdev, struct pci_ide *ide)
{
	/* [한국어] 이 포트가 엔드포인트인지 루트 포트인지 보고 해당 설정을 고른다. */
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	/* [한국어] 레지스터에 쓸 값을 담아 둘 임시 구조체. 스택에 두고
	 * 계산이 끝난 뒤 한꺼번에 쓴다. */
	struct pci_ide_regs regs;
	/* [한국어] 이 스트림의 레지스터 블록 오프셋. */
	int pos;

	/* [한국어] 짝이 맞지 않으면 조용히 물러난다. 경고는 이미 나갔다. */
	if (!settings)
		return;

	/* [한국어] 먼저 쓸 값을 전부 계산해 둔다. 계산과 쓰기를 나눈 덕에
	 * 중간에 실패할 여지가 없다 — 여기까지 오면 남은 것은 쓰기뿐이다. */
	pci_ide_stream_to_regs(pdev, ide, &regs);

	/* [한국어] 이 포트에서 이 스트림이 쓸 레지스터 블록의 위치. */
	pos = sel_ide_offset(pdev, settings);

	/* [한국어] RID 연관 두 워드를 쓴다. 어느 Requester ID 범위의 요청을
	 * 이 스트림으로 볼지가 정해진다. */
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, regs.rid1);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, regs.rid2);

	/* [한국어] 채운 만큼의 주소 연관 블록을 쓴다. 블록 하나가 레지스터
	 * 셋이라 세 번씩 쓴다. PCI_IDE_SEL_ADDR_n(i) 매크로가 i 번째 블록의
	 * 상대 오프셋을 계산해 준다. */
	for (int i = 0; i < regs.nr_addr; i++) {
		/* [한국어] 1번: VALID 비트 + 시작·끝의 하위 부분(비트 31..20). */
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_1(i),
				       regs.addr[i].assoc1);
		/* [한국어] 2번: 끝 주소의 상위 32비트. */
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_2(i),
				       regs.addr[i].assoc2);
		/* [한국어] 3번: 시작 주소의 상위 32비트. 끝이 시작보다 앞 번호에
		 * 오는 것은 규격이 정한 배치다. */
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_3(i),
				       regs.addr[i].assoc3);
	}

	/* clear extra unused address association blocks */
	/* [한국어] 상류 주석대로 남는 블록을 0 으로 지운다. 이것을 빠뜨리면
	 * 이전에 이 블록을 쓰던 스트림의 주소 범위가 남아, 의도하지 않은
	 * 영역까지 이 스트림에 딸려 들어간다. 보안 기능이므로 지우는 것이
	 * 특히 중요하다.
	 * assoc1 의 VALID 비트가 0 이 되므로 그 블록은 무시된다. */
	for (int i = regs.nr_addr; i < pdev->nr_ide_mem; i++) {
		/* [한국어] 세 워드를 모두 0 으로. 1번의 VALID 가 0 이 되므로
		 * 하드웨어는 이 블록을 무시한다. */
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_1(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_2(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_3(i), 0);
	}

	/*
	 * Setup control register early for devices that expect
	 * stream_id is set during key programming.
	 */
	/* [한국어] 상류 주석이 밝힌 이유가 중요하다. Control 레지스터를
	 * 여기서 미리 쓰되 enable=false 로 둔다.
	 *
	 * 왜 미리 쓰는가 — 설정과 활성화 사이에 암호 키 교환 단계가 있는데
	 * (이 파일 밖의 일이다), 일부 장치는 그 과정에서 Stream ID 가 이미
	 * 레지스터에 들어 있기를 기대한다. 어느 스트림의 키인지 알아야 하기
	 * 때문이다. 그래서 활성화는 미루되 ID 를 포함한 나머지 설정은 먼저
	 * 넣어 둔다.
	 *
	 * enable 만 false 인 것이 요점이다. 키가 준비되기 전에 스트림을 켜면
	 * 암호화 없이 스트림으로 표시된 트래픽이 흐를 수 있다. */
	set_ide_sel_ctl(pdev, ide, settings, pos, false);
	/* [한국어] 설정 완료 표시. pci_ide_stream_release() 가 이 값을 보고
	 * teardown 이 필요한지 판단한다. */
	settings->setup = 1;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_setup);

/**
 * pci_ide_stream_teardown() - disable the stream and clear all settings
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered IDE settings descriptor
 *
 * For stream destruction, zero all registers that may have been written
 * by pci_ide_stream_setup(). Consider pci_ide_stream_disable() to leave
 * settings in place while temporarily disabling the stream.
 */
/* [한국어]
 * pci_ide_stream_teardown - 스트림을 끄고 레지스터를 전부 지운다
 *
 * @pdev: 루트 포트이거나 엔드포인트.
 * @ide: 등록된 스트림 상태.
 * @return: 없음.
 *
 * setup 의 반대다. 상류 주석이 밝히듯 setup 이 썼을 수 있는 모든
 * 레지스터를 0 으로 만든다.
 *
 * pci_ide_stream_disable() 과의 차이가 중요하다. disable 은 Control 만
 * 끄고 RID 와 주소 범위는 남겨 두어, 잠시 껐다 켤 때 설정을 다시 쓰지
 * 않아도 되게 한다. teardown 은 스트림을 없앨 때 쓰는 것으로 전부 지운다.
 * 보안 기능이라 남은 설정이 다음 스트림에 딸려 들어가면 안 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_ide_stream_release() → [이 함수]
 */
void pci_ide_stream_teardown(struct pci_dev *pdev, struct pci_ide *ide)
{
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	int pos, i;

	/* [한국어] 짝이 맞지 않으면 조용히 물러난다. void 함수라 오류를
	 * 전할 방법이 없고, 경고는 이미 pci_ide_to_settings 가 남겼다. */
	if (!settings)
		return;

	pos = sel_ide_offset(pdev, settings);

	/* [한국어] Control 을 먼저 0 으로 만들어 스트림을 끈다. 순서가
	 * 중요하다 — 켜져 있는 상태에서 RID 나 주소 범위를 지우면 그 사이에
	 * 잘못된 범위로 동작하는 순간이 생긴다. 끄고 나서 지운다. */
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);

	/* [한국어] 주소 연관 블록을 전부 지운다. regs.nr_addr 이 아니라
	 * pdev->nr_ide_mem 까지 도는 이유는, setup 이 몇 개를 썼는지와
	 * 무관하게 이 블록 전체를 깨끗이 비우려는 것이다. */
	for (i = 0; i < pdev->nr_ide_mem; i++) {
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_1(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_2(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_3(i), 0);
	}

	/* [한국어] RID 연관도 지운다. 2번(VALID 비트가 있는 쪽)을 먼저 지워
	 * 조건을 무효화한 뒤 1번을 지우는 순서다. */
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, 0);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, 0);
	/* [한국어] 설정 없음으로 표시. pci_ide_stream_release() 가 두 번
	 * teardown 하지 않게 된다. */
	settings->setup = 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_teardown);

/**
 * pci_ide_stream_enable() - enable a Selective IDE Stream
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered and setup IDE settings descriptor
 *
 * Activate the stream by writing to the Selective IDE Stream Control
 * Register.
 *
 * Return: 0 if the stream successfully entered the "secure" state, and -EINVAL
 * if @ide is invalid, and -ENXIO if the stream fails to enter the secure state.
 *
 * Note that the state may go "insecure" at any point after returning 0, but
 * those events are equivalent to a "link down" event and handled via
 * asynchronous error reporting.
 *
 * Caller is responsible to clear the enable bit in the -ENXIO case.
 */
/* [한국어]
 * pci_ide_stream_enable - 스트림을 활성화하고 보안 상태를 확인한다
 *
 * @pdev: 루트 포트이거나 엔드포인트.
 * @ide: 등록되고 setup 까지 마친 스트림 상태.
 * @return: 0 이면 스트림이 secure 상태로 들어갔다. @ide 가 잘못됐으면
 *   -EINVAL, 켰는데 secure 로 가지 못했으면 -ENXIO.
 *
 * setup 이 enable=false 로 써 둔 Control 을 이번에는 true 로 다시 쓴다.
 * 그 사이에 암호 키 교환이 끝나 있어야 한다(이 파일 밖의 일이다).
 *
 * 켰다고 끝이 아니라 Status 를 읽어 확인하는 점이 중요하다. 양쪽 설정이
 * 어긋났거나 키가 제대로 들어가지 않았으면 하드웨어가 secure 로 가지
 * 못한다. 그런데 그때도 enable 비트는 이미 서 있으므로, 상류 주석대로
 * 호출자가 그 비트를 지울 책임을 진다(pci_ide_stream_disable 을 부르면 된다).
 *
 * 또 하나 상류 주석의 단서: 0 을 돌려준 뒤에도 언제든 insecure 로
 * 떨어질 수 있으며, 그런 사건은 링크 다운과 같은 취급으로 비동기 오류
 * 보고를 통해 온다. 즉 이 반환값은 "지금 이 순간" 의 상태일 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (상위 TSM/TDISP 계층) → [이 함수] → set_ide_sel_ctl()
 */
int pci_ide_stream_enable(struct pci_dev *pdev, struct pci_ide *ide)
{
	/* [한국어] 이 포트에 해당하는 파트너 설정을 고른다. */
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	/* [한국어] 이 스트림의 레지스터 블록 오프셋. */
	int pos;
	/* [한국어] Status 레지스터를 읽어 담을 임시 변수. */
	u32 val;

	/* [한국어] 이 함수는 int 를 돌려주므로 오류를 전할 수 있다.
	 * 위의 void 함수들과 달리 호출자가 실패를 알 수 있다. */
	if (!settings)
		return -EINVAL;

	pos = sel_ide_offset(pdev, settings);

	/* [한국어] setup 때 enable=false 로 써 두었던 Control 을 이번에는
	 * true 로 다시 쓴다. 나머지 필드는 같은 값이 다시 들어간다. */
	set_ide_sel_ctl(pdev, ide, settings, pos, true);
	/* [한국어] 활성화 표시를 먼저 남긴다. 아래 상태 확인이 실패하더라도
	 * 하드웨어의 enable 비트는 이미 서 있으므로, 호출자가 정리할 때
	 * disable 을 부를 수 있어야 하기 때문이다.
	 * 상류 주석의 "Caller is responsible to clear the enable bit in the
	 * -ENXIO case" 가 이것을 말한다. */
	settings->enable = 1;

	/* [한국어] Status 레지스터를 읽어 스트림이 정말 보안 상태로
	 * 들어갔는지 확인한다. 켜라고 썼다고 켜지는 것이 아니다 — 키가
	 * 제대로 설정되지 않았거나 양쪽 설정이 어긋나면 secure 로 가지 못한다. */
	pci_read_config_dword(pdev, pos + PCI_IDE_SEL_STS, &val);
	/* [한국어] 상태가 SECURE 가 아니면 실패다. 상류 주석이 밝히듯
	 * 0 을 돌려준 뒤에도 언제든 insecure 로 떨어질 수 있으며, 그런
	 * 사건은 링크 다운과 같은 취급으로 비동기 오류 보고를 통해 온다.
	 * 즉 이 확인은 "지금 이 순간" 의 상태일 뿐이다. */
	if (FIELD_GET(PCI_IDE_SEL_STS_STATE, val) !=
	    PCI_IDE_SEL_STS_STATE_SECURE)
		return -ENXIO;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_enable);

/**
 * pci_ide_stream_disable() - disable a Selective IDE Stream
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered and setup IDE settings descriptor
 *
 * Clear the Selective IDE Stream Control Register, but leave all other
 * registers untouched.
 */
/* [한국어]
 * pci_ide_stream_disable - 설정은 남긴 채 스트림만 끈다
 *
 * @pdev: 루트 포트이거나 엔드포인트.
 * @ide: 등록되고 setup 까지 마친 스트림 상태.
 * @return: 없음.
 *
 * 상류 주석대로 Control 레지스터만 지우고 나머지는 그대로 둔다.
 * teardown 과 나눠 둔 이유는 잠시 껐다 켜는 경우 때문이다 — 설정을
 * 다시 쓰지 않아도 되므로 config 접근이 크게 줄어든다.
 *
 * pci_ide_stream_enable() 이 -ENXIO 를 돌려줬을 때 enable 비트를 지우는
 * 데도 이 함수를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_ide_stream_release() → [이 함수]
 *   (또는 enable 실패 후 호출자가 직접)
 */
void pci_ide_stream_disable(struct pci_dev *pdev, struct pci_ide *ide)
{
	/* [한국어] 이 포트에 해당하는 파트너 설정을 고른다. */
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	/* [한국어] 이 스트림의 레지스터 블록 오프셋. */
	int pos;

	/* [한국어] 짝이 맞지 않으면 조용히 물러난다. */
	if (!settings)
		return;

	pos = sel_ide_offset(pdev, settings);

	/* [한국어] Control 만 0 으로 만든다. RID 와 주소 연관은 그대로 남는다.
	 * teardown 과의 차이가 여기 있다 — 잠시 껐다 켤 때 설정을 다시 쓰지
	 * 않아도 되게 하려는 것이다.
	 * 다만 0 을 쓰면 Stream ID 필드도 함께 0 이 된다는 점은 알아 두어야
	 * 한다. 다시 켤 때는 set_ide_sel_ctl() 이 ID 를 포함해 전부 다시
	 * 쓰므로 문제가 되지 않는다. */
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);
	/* [한국어] 비활성 표시. release 가 두 번 끄지 않게 된다. */
	settings->enable = 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_disable);

/* [한국어]
 * pci_ide_init_host_bridge - 브리지의 IDE 스트림 자원 풀을 준비한다
 *
 * @hb: 막 만들어진 호스트 브리지.
 * @return: 없음.
 *
 * 브리지 하나가 IDE 스트림 자원의 관리 단위다. 그 아래 모든 장치가
 * 여기서 Stream ID 를 받아 가며, 번호가 겹치면 안 되기 때문이다.
 *
 * 기본값 256 은 Stream ID 가 8비트라 그 이상은 표현할 수 없어서다.
 * 실제 플랫폼이 그보다 적게 지원하면 pci_ide_set_nr_streams() 로 줄인다.
 *
 * 예약 ID 를 미리 잡아 두는 것이 이 함수의 요점이다. pci_ide_init() 이
 * 쓰지 않는 레지스터 블록에 그 번호를 적어 "비어 있음" 을 표시하는데,
 * 만약 어떤 스트림이 실제로 그 번호를 할당받으면 표시와 실제가 구분되지
 * 않게 된다. 그래서 아무도 못 쓰게 처음부터 막아 둔다.
 *
 * 실행 컨텍스트: 브리지 생성 중 프로세스 컨텍스트.
 *
 * 에러 경로: reserve_stream_id 의 반환값을 보지 않는다. 방금 초기화한
 *   ida 라 실패할 수 있는 경우가 메모리 부족뿐이고, 그때는 어차피
 *   그 브리지에서 IDE 를 쓸 수 없게 된다.
 *
 * 호출 체인:
 *   pci_alloc_host_bridge() 경로 [probe.c:1782] → [이 함수]
 */
void pci_ide_init_host_bridge(struct pci_host_bridge *hb)
{
	/* [한국어] Stream ID 가 8비트이므로 256 이 이론적 상한이다.
	 * 플랫폼이 더 적게 지원하면 pci_ide_set_nr_streams() 로 줄인다. */
	hb->nr_ide_streams = 256;
	/* [한국어] 브리지 수준의 스트림 자원 풀. pci_ide_stream_alloc() 이
	 * 여기서 첫 번째 할당을 받아 간다. */
	ida_init(&hb->ide_stream_ida);
	/* [한국어] Stream ID 풀. 위와 다른 것이다 — 이쪽이 링크 위 패킷에
	 * 실리는 번호를 관리한다. */
	ida_init(&hb->ide_stream_ids_ida);
	/* [한국어] 예약 번호를 즉시 잡아 아무도 쓰지 못하게 한다.
	 * 이 번호는 "이 레지스터 블록은 비어 있다" 는 표시 전용이다. */
	reserve_stream_id(hb, PCI_IDE_RESERVED_STREAM_ID);
}

/* [한국어]
 * available_secure_streams_show - 남은 스트림 자원 수를 sysfs 로 보여 준다
 *
 * @dev: 호스트 브리지의 device. to_pci_host_bridge 로 되돌린다.
 * @attr: 어느 속성인지. 이 브리지에 속성이 하나뿐이라 쓰지 않는다.
 * @buf: 출력 버퍼. PAGE_SIZE 크기가 보장된다.
 * @return: 쓴 바이트 수, 또는 이 브리지가 IDE 를 지원하지 않으면 -ENXIO.
 *
 * 기밀 컴퓨팅 환경을 운영할 때 알아야 할 것이 "이 브리지에 보안 스트림을
 * 몇 개 더 만들 수 있는가" 다. 장치 하나를 기밀 VM 에 붙일 때마다
 * 스트림을 하나 쓰므로, 남은 수가 곧 더 붙일 수 있는 장치 수다.
 *
 * 계산 방식이 소박하다. ida 에 0부터 nr-1 까지 하나씩 물어보며 이미
 * 쓰이는 것을 뺀다. 상류 주석이 스스로 "비효율적이고 경쟁 조건도 있다"
 * 고 인정하면서도 괜찮다고 하는 이유가 둘이다.
 *   - 이 파일을 읽는 일이 자주 있지 않다. 운영자가 가끔 조사할 뿐이다.
 *   - 최악이라도 256번 반복이라 시간이 뻔하다.
 * 경쟁 조건이란, 세는 동안 다른 CPU 가 스트림을 만들거나 없앨 수 있어
 * 결과가 그 순간의 정확한 값이 아닐 수 있다는 뜻이다. 대략의 수치로
 * 쓰는 값이라 락을 걸어 가며 정확히 셀 이유가 없다.
 *
 * 실행 컨텍스트: sysfs 읽기 — 사용자 프로세스의 컨텍스트.
 *
 * 호출 체인:
 *   사용자가 /sys/.../available_secure_streams 를 읽음
 *     → sysfs 계층 → [이 함수] → ida_exists()
 */
static ssize_t available_secure_streams_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	/* [한국어] 임베디드된 device 에서 바깥 호스트 브리지를 되찾는다. */
	struct pci_host_bridge *hb = to_pci_host_bridge(dev);
	/* [한국어] READ_ONCE 로 읽는 이유는 pci_ide_set_nr_streams() 가
	 * 동시에 이 값을 바꿀 수 있기 때문이다. 컴파일러가 이 값을 여러 번
	 * 다시 읽어 루프 도중에 달라지는 일을 막는다 — 아래 루프의 상한과
	 * avail 의 초기값이 반드시 같은 값이어야 계산이 말이 된다. */
	int nr = READ_ONCE(hb->nr_ide_streams);
	/* [한국어] 전체에서 시작해 쓰이는 것을 빼 나간다. */
	int avail = nr;

	/* [한국어] 0 이면 이 브리지에 IDE 스트림 자원이 아예 없다.
	 * 아래 is_visible 이 그런 경우 이 파일을 숨기므로 정상적으로는
	 * 여기 오지 않지만, 파일을 연 뒤에 값이 0 으로 바뀌었을 수 있다. */
	if (!nr)
		return -ENXIO;

	/*
	 * Yes, this is inefficient and racy, but it is only for occasional
	 * platform resource surveys. Worst case is bounded to 256 streams.
	 */
	/* [한국어] 하나씩 물어보며 쓰이는 것을 뺀다. ida 에 "몇 개 쓰였나" 를
	 * 한 번에 묻는 API 가 없어 이렇게 센다. */
	for (int i = 0; i < nr; i++)
		if (ida_exists(&hb->ide_stream_ida, i))
			avail--;
	/* [한국어] sysfs_emit 은 버퍼 넘침을 막아 주는 sysfs 전용 출력 함수다.
	 * 일반 sprintf 대신 이것을 쓰는 것이 커널의 규약이다. */
	return sysfs_emit(buf, "%d\n", avail);
}
/* [한국어] 위 show 함수를 읽기 전용(0444) sysfs 속성으로 등록한다.
 * dev_attr_available_secure_streams 라는 이름의 변수가 만들어지며,
 * 아래 속성 배열과 is_visible 이 그 이름으로 참조한다. */
static DEVICE_ATTR_RO(available_secure_streams);

/* [한국어] 이 그룹에 속한 속성 목록. 지금은 하나뿐이고 NULL 로 끝을
 * 표시한다. 나중에 IDE 관련 속성이 늘면 여기에 추가된다. */
static struct attribute *pci_ide_attrs[] = {
	&dev_attr_available_secure_streams.attr,
	NULL
};

/* [한국어]
 * pci_ide_attr_visible - 이 속성을 sysfs 에 만들지 말지 정한다
 *
 * @kobj: 속성이 붙을 kobject. 호스트 브리지의 것이다.
 * @a: 판단할 속성.
 * @n: 그룹 안에서의 번호. 여기서는 쓰지 않는다.
 * @return: 만들 것이면 그 속성의 mode(권한), 만들지 않을 것이면 0.
 *
 * IDE 를 지원하지 않는 브리지에 이 파일이 보이면 혼란스럽다.
 * 읽어 봐야 -ENXIO 만 나올 뿐이고, 사용자 공간의 도구가 "파일이 있으니
 * 기능이 있다" 고 오판할 수도 있다. 그래서 아예 만들지 않는다.
 *
 * sysfs 그룹의 is_visible 콜백은 속성을 만들 때 각 속성마다 한 번씩
 * 불린다. 조건이 바뀌면 sysfs_update_group() 으로 다시 평가하게 할 수
 * 있고, pci_ide_set_nr_streams() 가 실제로 그렇게 한다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성/갱신 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs_create_group() 또는 sysfs_update_group() → [이 함수]
 */
static umode_t pci_ide_attr_visible(struct kobject *kobj, struct attribute *a, int n)
{
	/* [한국어] kobject 에서 device 로, 다시 호스트 브리지로 되짚는다. */
	struct device *dev = kobj_to_dev(kobj);
	struct pci_host_bridge *hb = to_pci_host_bridge(dev);

	/* [한국어] 속성이 여럿이 될 것에 대비해 어느 것인지 확인한다.
	 * 지금은 하나뿐이라 항상 참이다. */
	if (a == &dev_attr_available_secure_streams.attr)
		/* [한국어] 스트림 자원이 0 이면 숨긴다. 0 을 돌려주면 sysfs 가
		 * 그 파일을 만들지 않는다. */
		if (!hb->nr_ide_streams)
			return 0;

	/* [한국어] 그 외에는 속성에 선언된 권한을 그대로 쓴다.
	 * DEVICE_ATTR_RO 로 만들었으므로 0444 다. */
	return a->mode;
}

/* [한국어] 호스트 브리지에 붙일 IDE 속성 그룹.
 * static 이 아닌 이유는 이 파일 밖에서 참조하기 때문이다. 확인해 보면
 * 선언은 drivers/pci/pci.h:1763 에 있고, 실제로 브리지의 속성 그룹
 * 목록에 넣는 곳은 drivers/pci/probe.c:1694 다. */
const struct attribute_group pci_ide_attr_group = {
	.attrs = pci_ide_attrs,
	.is_visible = pci_ide_attr_visible,
};

/**
 * pci_ide_set_nr_streams() - sets size of the pool of IDE Stream resources
 * @hb: host bridge boundary for the stream pool
 * @nr: number of streams
 *
 * Platform PCI init and/or expert test module use only. Limit IDE
 * Stream establishment by setting the number of stream resources
 * available at the host bridge. Platform init code must set this before
 * the first pci_ide_stream_alloc() call if the platform has less than the
 * default of 256 streams per host-bridge.
 *
 * The "PCI_IDE" symbol namespace is required because this is typically
 * a detail that is settled in early PCI init. I.e. this export is not
 * for endpoint drivers.
 */
/* [한국어]
 * pci_ide_set_nr_streams - 브리지의 스트림 자원 풀 크기를 정한다
 *
 * @hb: 대상 호스트 브리지.
 * @nr: 이 플랫폼이 실제로 지원하는 스트림 수.
 * @return: 없음.
 *
 * pci_ide_init_host_bridge() 가 기본값 256(8비트 Stream ID 의 상한)을
 * 넣어 두는데, 실제 플랫폼이 그보다 적게 지원하면 이 함수로 줄인다.
 *
 * 상류 주석이 두 가지를 강조한다. 하나는 첫 pci_ide_stream_alloc()
 * 호출 전에 설정해야 한다는 것 — 이미 번호가 나간 뒤에 상한을 낮추면
 * 앞뒤가 맞지 않는다. 아래 WARN_ON_ONCE 가 그것을 확인한다.
 *
 * 다른 하나는 심볼 네임스페이스다. EXPORT_SYMBOL_NS_GPL 로 "PCI_IDE"
 * 네임스페이스에 넣어, 그 네임스페이스를 명시적으로 import 한 모듈만
 * 쓸 수 있게 했다. 초기 PCI 초기화 단계에서 정해질 일이지 엔드포인트
 * 드라이버가 만질 것이 아니라는 뜻을 코드로 강제한 것이다.
 *
 * 실행 컨텍스트: 플랫폼 PCI 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 PCI 초기화 코드 또는 시험 모듈 → [이 함수]
 *     → sysfs_update_group() → pci_ide_attr_visible()
 */
void pci_ide_set_nr_streams(struct pci_host_bridge *hb, u16 nr)
{
	/* [한국어] 8비트 Stream ID 로 표현할 수 있는 상한이 256 이므로,
	 * 더 큰 값이 들어와도 거기서 자른다. */
	hb->nr_ide_streams = min(nr, 256);
	/* [한국어] 이미 스트림이 할당된 뒤에 풀 크기를 바꾸면 앞뒤가 맞지
	 * 않는다 — 이미 나간 번호가 새 상한보다 클 수 있기 때문이다.
	 * 그래서 상류 주석이 "첫 pci_ide_stream_alloc() 호출 전에 설정하라"
	 * 고 못 박고, 여기서 그 규칙을 어겼는지 확인한다.
	 * WARN_ON_ONCE 라 경고만 하고 진행한다 — 초기화 순서 문제이므로
	 * 여기서 실패시켜도 되돌릴 방법이 없다. */
	WARN_ON_ONCE(!ida_is_empty(&hb->ide_stream_ida));
	/* [한국어] 위 pci_ide_attr_visible() 을 다시 평가하게 한다.
	 * nr 이 0 으로 바뀌었으면 available_secure_streams 파일이 사라지고,
	 * 0 에서 양수로 바뀌었으면 나타난다. */
	sysfs_update_group(&hb->dev.kobj, &pci_ide_attr_group);
}
EXPORT_SYMBOL_NS_GPL(pci_ide_set_nr_streams, "PCI_IDE");

/* [한국어]
 * pci_ide_destroy - 장치가 사라질 때 IDE 자원을 정리한다
 *
 * @pdev: 제거되는 장치.
 * @return: 없음.
 *
 * 한 줄짜리 함수지만 짝이 분명하다. pci_ide_init() 이 조건 없이
 * ida_init() 을 했으므로, 여기서도 조건 없이 ida_destroy() 를 한다.
 * IDE 를 쓰지 않는 장치라도 ida 는 초기화되어 있으니 파괴해도 안전하다 —
 * pci_ide_init() 이 그 초기화를 무조건 하는 이유가 바로 이 대칭성이다.
 *
 * ida_destroy() 는 아직 반납되지 않은 번호가 있어도 내부 자료구조를
 * 해제한다. 정상적인 경로라면 스트림이 모두 해제된 뒤에 장치가
 * 제거되므로 비어 있어야 한다.
 *
 * 여기서 Stream ID 를 반납하지 않는 점에 주의할 만하다. 그것은 브리지의
 * ida 에 있고 장치의 것이 아니므로, pci_ide_stream_unregister() 가
 * 스트림 해제 시점에 반납한다.
 *
 * 실행 컨텍스트: 장치 제거 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_destroy_dev() [remove.c:157] → [이 함수] → ida_destroy()
 */
void pci_ide_destroy(struct pci_dev *pdev)
{
	/* [한국어] 이 장치의 스트림 인덱스 풀을 해제한다. */
	ida_destroy(&pdev->ide_stream_ida);
}
