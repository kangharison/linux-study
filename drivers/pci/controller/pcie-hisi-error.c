// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for handling the PCIe controller errors on
 * HiSilicon HIP SoCs.
 *
 * Copyright (c) 2020 HiSilicon Limited.
 */

/* [한국어] ACPI_HANDLE(), acpi_evaluate_integer(), acpi_get_parent(),
 * acpi_pci_find_root(), device_property_read_u8(). */
/*
 * [한국어 설명] 펌웨어가 GHES 로 알려 주는 HiSilicon PCIe 컨트롤러 오류 처리 (pcie-hisi-error.c)
 *
 * === 파일의 역할 ===
 * HiSilicon HIP 계열 SoC 의 PCIe **컨트롤러 자체** 에서 난 오류를 처리한다.
 * 링크 너머 장치의 오류(표준 AER)가 아니라, 컨트롤러 내부의 AP/TL/MAC/DL/SDI
 * 계층에서 난 오류가 대상이다.
 * 이 드라이버가 다른 PCI 드라이버와 근본적으로 다른 점은 **하드웨어를
 * 전혀 만지지 않는다**는 것이다. 레지스터를 매핑하지도, 인터럽트를 잡지도
 * 않는다. 오류는 펌웨어가 감지해 GHES(Generic Hardware Error Source)라는
 * ACPI 표준 통로로 커널에 알려 주고, 복구를 위한 리셋도 ACPI "RST" 메서드로
 * 펌웨어에 요청한다. 커널이 하는 일은 그 사이에서 로그를 남기고, 리셋 전후로
 * PCI 장치를 제거·재스캔하는 것뿐이다.
 * 복구 방식이 거칠다는 점도 특징이다. 표준 AER 복구(pcie/err.c)가 드라이버에게
 * 오류를 알리고 의견을 물어 단계적으로 진행하는 반면, 여기서는 루트 포트와
 * 그 아래 전부를 **제거하고** → 펌웨어에 리셋을 요청하고 → 1초 기다린 뒤
 * → **다시 스캔한다**. 컨트롤러 리셋이 하위 장치의 상태를 모두 날려 버려
 * 드라이버가 복구할 여지가 없기 때문이다.
 * 그리고 복구 가능(RECOVERABLE) 심각도에만 그렇게 한다. 치명적 오류는
 * 리셋해도 소용이 없고, 이미 정정된 오류는 복구할 것이 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCI 스택이 아니라 ACPI 오류 보고 경로에 매달려 있다.
 *   펌웨어가 컨트롤러 오류를 감지
 *     → GHES 레코드로 커널에 보고 (APEI 서브시스템)
 *       → 벤더 고유 레코드 알림 사슬
 *         → [이 파일] hisi_pcie_notify_error()
 *            → GUID 확인 → socket 속성 확인 → 소켓 번호 대조
 *            → hisi_pcie_handle_error() 로 로그를 남기고
 *               → (복구 가능하면) hisi_pcie_port_do_recovery()
 *                  → 루트 포트 제거 → hisi_pcie_port_reset()("RST" 메서드)
 *                  → ssleep(1) → pci_rescan_bus()
 * 등록 경로는 짧다. ACPI HID "HISI0361" 로 매치된 플랫폼 장치의 probe 가
 * 알림 콜백 하나를 등록하면 끝이며, remove 조차 없다 — devres 가 해제를
 * 맡기 때문이다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 복구 경로가 ssleep(1) 로
 * 1초 이상 잠들고 ACPI 평가도 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: APEI/GHES 코어(acpi/ghes.h)의 벤더 레코드 알림 사슬과
 * devm_ghes_register_vendor_record_notifier(). 이 드라이버의 유일한 진입점이다.
 * 옆쪽: ACPI 코어의 acpi_get_parent(), acpi_pci_find_root(),
 * acpi_evaluate_integer(), 그리고 device_property_read_u8().
 * 아래쪽: PCI 코어의 pci_get_domain_bus_and_slot(),
 * pci_stop_and_remove_bus_device_locked(), pci_rescan_bus() —
 * 제거와 재스캔이 이 드라이버가 PCI 에 하는 일의 전부다.
 * 같은 SoC 계열의 pcie-hisi.c 와는 코드 접점이 없다. 그쪽은 config 접근의
 * 비표준성을 메우고 이쪽은 오류를 처리하며, 서로를 부르지 않는다.
 * 공유 상태: 전역은 GUID 하나(hisi_pcie_sec_guid)와 문자열 표 둘뿐이다.
 * 나머지 상태는 devm 으로 할당한 struct hisi_pcie_error_private 안에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct hisi_pcie_error_data: 커널이 채우는 것이 아니라 **펌웨어와 약속된
 *   배치** 다. GHES 레코드의 payload 를 이 타입으로 그대로 읽으므로,
 *   reserv[2] 같은 정렬용 자리까지 의미가 있다. val_bits 가 어느 필드가
 *   유효한지 알려 주는 열쇠이고, 그 비트 9번부터가 err_misc 배열에 대응한다.
 * - hisi_pcie_notify_error(): 세 겹의 거르기(GUID → socket 속성 존재 →
 *   소켓 번호 일치). 셋 다 NOTIFY_DONE 을 돌려주어 다른 구독자가 이어서
 *   보게 한다 — 남의 레코드를 가로채지 않는다.
 * - hisi_pcie_handle_error(): val_bits 를 확인하며 필드를 하나씩 찍고,
 *   err_misc 는 for_each_set_bit_from() 으로 유효한 것만 훑는다.
 *   복구는 RECOVERABLE 에만 시도한다.
 * - hisi_pcie_port_do_recovery(): 제거 → 리셋 → 대기 → 재스캔. 대상을 찾기
 *   위해 ACPI 부모로 한 단계 올라가는데, 이 핸들러 장치가 호스트 브리지의
 *   자식으로 선언되어 있기 때문이다.
 * - hisi_pcie_port_reset(): "RST" ACPI 메서드 평가. 파일 상단 세 매크로가
 *   펌웨어의 소켓 기준 포트 번호를 메서드가 요구하는 코어 ID + 코어 내 포트
 *   번호로 바꾼다.
 * - hisi_pcie_get_string(): 문자열 표 조회. 펌웨어가 준 값이 표 밖이면
 *   "unknown" 으로 답해, 알 수 없는 오류도 로그에는 남게 한다.
 * - hisi_pcie_error_handler_probe(): 알림 등록 한 줄이 전부. notifier_block 을
 *   구조체 첫 필드로 둔 배치가 콜백의 container_of 를 성립시킨다.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - hisi_pcie_handle_error() 가 버전을 찍는 조건이 VERSION 비트가 아니라
 *   SOC_ID 비트다. 두 비트가 어긋난 레코드에서는 버전이 출력되지 않으며,
 *   soc_id 필드 자체는 이 함수 어디에서도 찍히지 않는다.
 * - hisi_pcie_get_string() 이 인덱스를 순회하며 비교하지만, 배열 접근이므로
 *   범위 검사 한 줄과 결과가 같다.
 * - hisi_pcie_port_do_recovery() 에서 리셋이 실패하면 이미 장치를 제거한
 *   뒤이므로 되돌릴 수 없다. 시스템은 그 포트를 잃은 채 남는다.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * err_misc[33] 각 레지스터의 의미, HISI_PCIE_CORE_PORT_ID 가 하위 3비트를
 * 1 비트 미는 이유, 그리고 "RST" 메서드의 정확한 동작은 HiSilicon 문서에만
 * 있어 확인할 수 없었다. 코드가 하는 일만 적었다.
 *
 * === NVMe 관점 ===
 * 직접적인 코드 접점은 없지만, 결과는 NVMe 에 그대로 미친다. HIP SoC 서버에
 * 꽂힌 NVMe SSD 가 이 컨트롤러 뒤에 있다면, 복구가 시작되는 순간
 * pci_stop_and_remove_bus_device_locked() 가 그 SSD 를 통째로 제거한다.
 * NVMe 드라이버의 error_detected/slot_reset/resume 콜백을 거치는 표준 AER
 * 복구와 달리, 여기서는 드라이버에게 물어보지 않고 remove 를 부르므로
 * 진행 중이던 I/O 는 그 경로로 실패한다. 재스캔 뒤에는 새 장치로 다시
 * 발견되어 /dev 노드가 바뀔 수 있다.
 * dmesg 에서 "HISI : HIP : PCIe controller error" 로 시작하는 줄이 보이면
 * 그 복구가 일어난 것이다.
 */

#include <linux/acpi.h>
/* [한국어] struct acpi_hest_generic_data 와 acpi_hest_get_payload(),
 * 그리고 devm_ghes_register_vendor_record_notifier(). GHES(Generic Hardware
 * Error Source)는 펌웨어가 하드웨어 오류를 커널에 알리는 ACPI 표준 통로다. */
#include <acpi/ghes.h>
/* [한국어] BIT() 와 for_each_set_bit_from(), BITMAP_FROM_U64(). */
#include <linux/bitops.h>
/* [한국어] ssleep(). */
#include <linux/delay.h>
/* [한국어] pci_get_domain_bus_and_slot(), pci_stop_and_remove_bus_device_locked(),
 * pci_rescan_bus(). */
#include <linux/pci.h>
/* [한국어] platform_driver 와 module_platform_driver. */
#include <linux/platform_device.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/kfifo.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/spinlock.h>

/* HISI PCIe controller error definitions */
/* [한국어] 펌웨어가 함께 넘겨 주는 잡다한 오류 레지스터의 개수. */
#define HISI_PCIE_ERR_MISC_REGS	33

/* [한국어] 아래 val_bits 의 각 비트가 어느 필드가 유효한지 알려 준다. 펌웨어가
 * 채우지 못한 필드를 커널이 읽지 않게 하는 장치다. */
#define HISI_PCIE_LOCAL_VALID_VERSION		BIT(0)
/* [한국어] SoC ID 가 유효함. */
#define HISI_PCIE_LOCAL_VALID_SOC_ID		BIT(1)
/* [한국어] 소켓 ID. */
#define HISI_PCIE_LOCAL_VALID_SOCKET_ID		BIT(2)
/* [한국어] Nimbus ID. */
#define HISI_PCIE_LOCAL_VALID_NIMBUS_ID		BIT(3)
/* [한국어] 서브 모듈 ID. */
#define HISI_PCIE_LOCAL_VALID_SUB_MODULE_ID	BIT(4)
/* [한국어] 코어 ID. */
#define HISI_PCIE_LOCAL_VALID_CORE_ID		BIT(5)
/* [한국어] 포트 ID. */
#define HISI_PCIE_LOCAL_VALID_PORT_ID		BIT(6)
/* [한국어] 오류 종류. */
#define HISI_PCIE_LOCAL_VALID_ERR_TYPE		BIT(7)
/* [한국어] 오류 심각도. */
#define HISI_PCIE_LOCAL_VALID_ERR_SEVERITY	BIT(8)
/* [한국어] 여기서부터가 err_misc 배열의 유효 비트다. 위 아홉이 BIT() 매크로인데
 * 이것만 숫자 9 인 것은, 아래에서 비트 **번호** 로 쓰기 때문이다 —
 * for_each_set_bit_from() 이 마스크가 아니라 시작 인덱스를 받는다. */
#define HISI_PCIE_LOCAL_VALID_ERR_MISC		9

/* [한국어] 이 벤더 오류 레코드를 식별하는 GUID. 펌웨어가 GHES 레코드의 섹션 타입에
 * 이 값을 실어 보내고, 알림 콜백이 그것을 비교해 자기 레코드를 가려낸다. */
static guid_t hisi_pcie_sec_guid =
	GUID_INIT(0xB2889FC9, 0xE7D7, 0x4F9D,
		  0xA8, 0x67, 0xAF, 0x42, 0xE9, 0x8B, 0xE7, 0x72);

/*
 * Firmware reports the socket port ID where the error occurred.  These
 * macros convert that to the core ID and core port ID required by the
 * ACPI reset method.
 */
/* [한국어] 위 영어 주석대로 펌웨어는 소켓 기준 포트 ID 를 보고하는데, ACPI 리셋
 * 메서드는 코어 ID 와 코어 안의 포트 ID 를 요구한다. 세 매크로가 그 변환을
 * 맡는다. 이것은 코어 ID 와 포트 ID 를 합쳐 소켓 기준 번호를 만든다. */
#define HISI_PCIE_PORT_ID(core, v)       (((v) >> 1) + ((core) << 3))
/* [한국어] 소켓 기준 번호에서 코어 ID 를 뽑는다. 코어 하나에 포트가 여덟이라
 * 3비트를 민다. */
#define HISI_PCIE_CORE_ID(v)             ((v) >> 3)
/* [한국어] 코어 안에서의 포트 번호를 뽑는다. 하위 3비트를 남기고 1 비트 미는 것은
 * 리셋 메서드가 그 형식을 요구하기 때문으로 보이나, 이 트리에 근거
 * 문서가 없다. */
#define HISI_PCIE_CORE_PORT_ID(v)        (((v) & 7) << 1)

struct hisi_pcie_error_data {
	/* [한국어] 어느 필드가 유효한지 알려 주는 비트맵. 이 구조체를 읽는 유일한 열쇠다.
	 * 설정자: 펌웨어.
	 * 읽는 자: hisi_pcie_handle_error() 가 필드마다 대응 비트를 확인한다.
	 * 값 범위: 위 HISI_PCIE_LOCAL_VALID_ 계열 비트의 조합. 0 이면 쓸 정보가
	 *   하나도 없다는 뜻이라 그 함수가 곧바로 물러난다.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료다. */
	u64	val_bits;
	/* [한국어] 레코드 형식의 버전.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	version;
	/* [한국어] SoC 식별자.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	soc_id;
	/* [한국어] 소켓 번호. 알림 콜백이 이 값으로 자기 소켓의 오류인지 가른다.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	socket_id;
	/* [한국어] Nimbus(HiSilicon 의 다이 이름) 식별자.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	nimbus_id;
	/* [한국어] 어느 계층에서 난 오류인지. 아래 hisi_pcie_sub_module[] 의 인덱스다.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	sub_module_id;
	/* [한국어] 코어 번호.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	core_id;
	/* [한국어] 코어 안의 포트 번호. 위 두 값이 합쳐져 리셋 대상을 지목한다.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	port_id;
	/* [한국어] 심각도. 아래 hisi_pcie_error_sev[] 의 인덱스이며, 복구 가능일 때만
	 *   실제 복구를 시도한다.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u8	err_severity;
	/* [한국어] 오류 종류. 문자열 표가 없어 16진수로 그대로 찍는다.
	 * 설정자: 펌웨어. 이 구조체는 커널이 채우는 것이 아니라 GHES 레코드에서
	 *   그대로 읽어 오는 **펌웨어와의 약속된 배치** 다.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 대응 비트를 확인한 뒤에만.
	 * 동기화: 레코드마다 새로 오는 읽기 전용 자료라 보호가 없다. */
	u16	err_type;
	/* [한국어] 정렬을 맞추기 위한 예약 자리. 펌웨어와 배치를 맞춰야 하므로 필요하다.
	 * 설정자/읽는 자: 없다.
	 * 값 범위: 의미 없음.
	 * 동기화: 해당 없음. */
	u8	reserv[2];
	/* [한국어] 잡다한 오류 레지스터 사본 33개.
	 * 설정자: 펌웨어.
	 * 읽는 자: hisi_pcie_handle_error() 가 val_bits 의 9번 비트부터를 훑으며
	 *   세워진 것만 찍는다.
	 * 값 범위: 하드웨어 레지스터 값. 의미는 HiSilicon 문서에만 있어
	 *   이 트리에서 확인할 수 없다.
	 * 동기화: 읽기 전용. */
	u32	err_misc[HISI_PCIE_ERR_MISC_REGS];
};

struct hisi_pcie_error_private {
	/* [한국어] GHES 알림 사슬에 등록할 블록. 이 구조체의 **첫 필드**라 콜백이
	 *   container_of 로 바깥을 되찾을 수 있다.
	 * 설정자: probe 가 notifier_call 을 채워 등록한다.
	 * 읽는 자: GHES 코어가 오류 레코드마다 이 콜백을 부른다.
	 * 동기화: devres 가 등록·해제를 관리한다. */
	struct notifier_block	nb;
	/* [한국어] 이 핸들러가 붙은 플랫폼 장치의 device.
	 * 설정자: probe.
	 * 읽는 자: 알림 콜백이 소켓 번호를 읽고 platform_device 를 되찾는 데 쓴다.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device *dev;
};

enum hisi_pcie_submodule_id {
	/* [한국어] AP(Application) 계층. */
	HISI_PCIE_SUB_MODULE_ID_AP,
	/* [한국어] TL(Transaction Layer). */
	HISI_PCIE_SUB_MODULE_ID_TL,
	/* [한국어] MAC 계층. */
	HISI_PCIE_SUB_MODULE_ID_MAC,
	/* [한국어] DL(Data Link Layer). */
	HISI_PCIE_SUB_MODULE_ID_DL,
	/* [한국어] SDI 계층. PCIe 계층 구조를 따라 오류 출처를 나눈다. */
	HISI_PCIE_SUB_MODULE_ID_SDI,
};

/* [한국어] 위 enum 을 사람이 읽을 문자열로 옮긴 표. 지정 초기화라 인덱스가 곧
 * enum 값이며, 빠진 자리는 NULL 이 되어 아래 조회 함수가 "unknown" 으로
 * 답한다. */
static const char * const hisi_pcie_sub_module[] = {
	/* [한국어] AP. */
	[HISI_PCIE_SUB_MODULE_ID_AP]	= "AP Layer",
	[HISI_PCIE_SUB_MODULE_ID_TL]	= "TL Layer",
	[HISI_PCIE_SUB_MODULE_ID_MAC]	= "MAC Layer",
	[HISI_PCIE_SUB_MODULE_ID_DL]	= "DL Layer",
	[HISI_PCIE_SUB_MODULE_ID_SDI]	= "SDI Layer",
};

enum hisi_pcie_err_severity {
	/* [한국어] 복구 가능. 이 값일 때만 실제 복구를 시도한다. */
	HISI_PCIE_ERR_SEV_RECOVERABLE,
	/* [한국어] 치명적. */
	HISI_PCIE_ERR_SEV_FATAL,
	/* [한국어] 이미 정정됨. */
	HISI_PCIE_ERR_SEV_CORRECTED,
	/* [한국어] 없음. */
	HISI_PCIE_ERR_SEV_NONE,
};

/* [한국어] 심각도 문자열 표. 위 서브 모듈 표와 같은 방식이다. */
static const char * const hisi_pcie_error_sev[] = {
	/* [한국어] 복구 가능. */
	[HISI_PCIE_ERR_SEV_RECOVERABLE]	= "recoverable",
	[HISI_PCIE_ERR_SEV_FATAL]	= "fatal",
	[HISI_PCIE_ERR_SEV_CORRECTED]	= "corrected",
	[HISI_PCIE_ERR_SEV_NONE]	= "none",
};

/* [한국어]
 * hisi_pcie_get_string - 문자열 표에서 인덱스에 해당하는 이름을 얻는다
 *
 * @array: 문자열 표.
 * @n: 그 크기.
 * @id: 찾는 인덱스.
 * @return: 해당 문자열, 없으면 "unknown".
 *
 * 서브 모듈 이름과 심각도 이름 두 표를 같은 방식으로 조회한다.
 *
 * 펌웨어가 준 값을 인덱스로 쓰는 것이라 범위 검사가 필수다. 표에 없는
 * 값이 오면 "unknown" 으로 답해, 알 수 없는 오류도 로그에는 남게 한다.
 *
 * [상류 코드 관찰] 인덱스와 id 가 같은지 순회하며 비교하지만, 배열 접근이므로
 * `return (id < n && array[id]) ? array[id] : "unknown";` 한 줄과 결과가 같다.
 * 표가 다섯 항목뿐이라 실제 비용 차이는 없다.
 *
 * 실행 컨텍스트: 오류 보고 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 범위 밖은 "unknown" 이다.
 *
 * 호출 체인:
 *   hisi_pcie_handle_error() → [이 함수]
 */
static const char *hisi_pcie_get_string(const char * const *array,
					size_t n, u32 id)
{
	/* [한국어] 순회 인덱스. */
	u32 index;

	/* [한국어] 표를 처음부터 훑는다. */
	for (index = 0; index < n; index++) {
		/* [한국어] 인덱스가 찾는 값과 같고 그 자리가 비어 있지 않으면, */
		if (index == id && array[index])
			/* [한국어] 그 문자열을 돌려준다. */
			return array[index];
	}

	/* [한국어] 찾지 못했거나 그 자리가 NULL 이면 "unknown".
	 * [상류 코드 관찰] 인덱스와 id 가 같은지 순회하며 비교하는데, 배열
	 *   접근이므로 `id < n && array[id]` 한 줄로 끝날 일이다. 결과는 같다. */
	return "unknown";
}

/* [한국어]
 * hisi_pcie_port_reset - ACPI "RST" 메서드로 펌웨어에 포트 리셋을 요청한다
 *
 * @pdev: 이 오류 핸들러의 플랫폼 장치.
 * @chip_id: 소켓(칩) 번호.
 * @port_id: 소켓 기준 포트 번호.
 * @return: 0 = 성공, -EIO = 메서드가 없거나 리셋이 실패.
 *
 * 커널이 레지스터를 직접 만지지 않고 펌웨어에 맡기는 것이 이 함수의 성격이다.
 * 이 SoC 의 컨트롤러 리셋 절차가 펌웨어에만 있기 때문이며, 같은 이유로
 * 이 드라이버 전체가 ACPI 없이는 동작하지 않는다.
 *
 * 인자 셋을 만드는 대목에서 파일 상단 세 매크로가 쓰인다. 함수 위 영어
 * 주석이 밝히듯 펌웨어는 소켓 기준 포트 번호를 보고하는데 RST 메서드는
 * 코어 ID 와 코어 안의 포트 번호를 요구하므로, 그 변환이 필요하다.
 *
 * 실패를 두 단계로 가른다. 메서드가 없거나 평가가 실패한 경우와, 평가는
 * 됐는데 반환값이 0 이 아닌 경우다. 둘 다 -EIO 이지만 로그가 다르다.
 *
 * 실행 컨텍스트: GHES 알림 처리 경로. 프로세스 컨텍스트이며 ACPI 평가가
 * 잠들 수 있다.
 *
 * 에러 경로: 두 경우 모두 -EIO.
 *
 * 호출 체인:
 *   hisi_pcie_port_do_recovery() → [이 함수] → acpi_evaluate_integer("RST")
 */
static int hisi_pcie_port_reset(struct platform_device *pdev,
				u32 chip_id, u32 port_id)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 장치의 ACPI 핸들. 리셋 메서드가 그 아래 있다. */
	acpi_handle handle = ACPI_HANDLE(dev);
	/* [한국어] ACPI 메서드에 넘길 인자 셋. */
	union acpi_object arg[3];
	/* [한국어] 그 인자 목록을 감싸는 서술자. */
	struct acpi_object_list arg_list;
	/* [한국어] 평가 결과. */
	acpi_status s;
	/* [한국어] 메서드가 돌려줄 값. 0 이 성공이다. */
	unsigned long long data = 0;

	/* [한국어] 첫 인자는 칩(소켓) ID. */
	arg[0].type = ACPI_TYPE_INTEGER;
	arg[0].integer.value = chip_id;
	/* [한국어] 둘째는 코어 ID. 소켓 기준 포트 번호에서 뽑아낸다. */
	arg[1].type = ACPI_TYPE_INTEGER;
	arg[1].integer.value = HISI_PCIE_CORE_ID(port_id);
	/* [한국어] 셋째는 코어 안의 포트 번호. 위 세 매크로가 만든 변환이 여기서 쓰인다. */
	arg[2].type = ACPI_TYPE_INTEGER;
	arg[2].integer.value = HISI_PCIE_CORE_PORT_ID(port_id);

	/* [한국어] 인자 세 개. */
	arg_list.count = 3;
	/* [한국어] 배열을 가리킨다. */
	arg_list.pointer = arg;

	/* [한국어] "RST" 라는 이름의 ACPI 메서드를 평가한다. 리셋을 커널이 직접 하지 않고
	 * 펌웨어에 맡기는 것으로, 이 SoC 의 컨트롤러 리셋 절차가 펌웨어에만
	 * 있기 때문이다. */
	s = acpi_evaluate_integer(handle, "RST", &arg_list, &data);
	/* [한국어] 메서드가 없거나 평가가 실패하면, */
	if (ACPI_FAILURE(s)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "No RST method\n");
		/* [한국어] 입출력 오류로 답한다. */
		return -EIO;
	}

	/* [한국어] 메서드는 성공했는데 반환값이 0 이 아니면 리셋 자체가 실패한 것이다. */
	if (data) {
		/* [한국어] 기록하고, */
		dev_err(dev, "Failed to Reset\n");
		/* [한국어] 같은 오류로 답한다. */
		return -EIO;
	}

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * hisi_pcie_port_do_recovery - 루트 포트를 통째로 제거했다가 리셋 후 다시 스캔한다
 *
 * @dev: 이 오류 핸들러의 플랫폼 장치.
 * @chip_id: 소켓 번호.
 * @port_id: 소켓 기준 포트 번호.
 * @return: 0 = 성공, -ENODEV = 대상을 찾지 못함, -EIO = 리셋 실패.
 *
 * 복구 방식이 거칠다는 점이 이 함수의 특징이다. 표준 AER 복구(pcie/err.c)가
 * 드라이버에게 오류를 알리고 의견을 물어 단계적으로 진행하는 것과 달리,
 * 여기서는 루트 포트와 그 아래 전부를 **제거하고** → 리셋하고 → **다시
 * 스캔한다**.
 *
 * 그래야 하는 이유가 있다. 컨트롤러 리셋이 그 아래 모든 장치의 상태를
 * 날려 버리므로, 드라이버가 복구할 여지가 남지 않는다. 장치를 지웠다가
 * 새로 발견하는 편이 일관된 상태에 이르는 유일한 길이다.
 *
 * 대상을 찾는 과정이 ACPI 를 두 번 거친다. 이 오류 핸들러 장치는 PCI 호스트
 * 브리지의 자식으로 선언되어 있어, 부모로 한 단계 올라가야 브리지에 닿고,
 * 거기서 acpi_pci_find_root() 로 도메인과 루트 버스를 얻는다.
 *
 * 리셋 실패는 되돌릴 수 없다. 이미 장치를 제거한 뒤이므로, 시스템은 그
 * 포트를 잃은 채 남는다.
 *
 * ssleep(1) 은 리셋 뒤 링크가 다시 설 시간을 주는 것으로, 함수 안의 영어
 * 주석이 그 이유를 밝힌다.
 *
 * 실행 컨텍스트: GHES 알림 처리 경로. 프로세스 컨텍스트이며 1초 이상 잠든다.
 *
 * 에러 경로: 대상을 찾지 못하면 -ENODEV(이때는 아직 아무것도 제거하지 않았다),
 * 리셋 실패는 -EIO(이때는 이미 제거한 뒤다).
 *
 * 호출 체인:
 *   hisi_pcie_handle_error() → [이 함수]
 *     → acpi_get_parent() → acpi_pci_find_root()
 *     → pci_get_domain_bus_and_slot()
 *     → pci_stop_and_remove_bus_device_locked()
 *     → hisi_pcie_port_reset() → ssleep() → pci_rescan_bus()
 */
static int hisi_pcie_port_do_recovery(struct platform_device *dev,
				      u32 chip_id, u32 port_id)
{
	/* [한국어] ACPI 평가 결과. */
	acpi_status s;
	/* [한국어] 진단 메시지용 device. */
	struct device *device = &dev->dev;
	/* [한국어] 이 장치의 ACPI 핸들. 아래에서 부모로 올라간다. */
	acpi_handle root_handle = ACPI_HANDLE(device);
	/* [한국어] 찾아낼 ACPI PCI 루트. */
	struct acpi_pci_root *pci_root;
	/* [한국어] 그 루트의 버스. */
	struct pci_bus *root_bus;
	/* [한국어] 복구 대상 루트 포트. */
	struct pci_dev *pdev;
	/* [한국어] 그 포트를 지목할 도메인·버스·devfn. */
	u32 domain, busnr, devfn;

	/* [한국어] 부모 ACPI 노드로 올라간다. 이 오류 핸들러 장치는 PCI 호스트 브리지의
	 * 자식으로 선언되어 있어, 한 단계 올라가야 브리지에 닿는다. */
	s = acpi_get_parent(root_handle, &root_handle);
	/* [한국어] 실패하면, */
	if (ACPI_FAILURE(s))
		return -ENODEV;
	/* [한국어] 그 노드에 대응하는 ACPI PCI 루트를 찾는다. */
	pci_root = acpi_pci_find_root(root_handle);
	/* [한국어] 없으면, */
	if (!pci_root)
		return -ENODEV;
	/* [한국어] 그 루트의 버스와, */
	root_bus = pci_root->bus;
	/* [한국어] 세그먼트(도메인) 번호를 얻는다. */
	domain = pci_root->segment;

	/* [한국어] 루트 버스 번호. */
	busnr = root_bus->number;
	/* [한국어] 포트 ID 를 장치 번호로 삼는다. 기능은 0 이다. */
	devfn = PCI_DEVFN(port_id, 0);
	/* [한국어] 그 위치의 루트 포트를 찾는다. 참조가 올라간 채로 온다. */
	pdev = pci_get_domain_bus_and_slot(domain, busnr, devfn);
	/* [한국어] 없으면, */
	if (!pdev) {
		/* [한국어] 어느 위치를 찾다 실패했는지 남기고, */
		dev_info(device, "Fail to get root port %04x:%02x:%02x.%d device\n",
			 domain, busnr, PCI_SLOT(devfn), PCI_FUNC(devfn));
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	/* [한국어] 루트 포트와 그 아래 전부를 제거한다. _locked 판이라 잠금을 알아서 잡는다.
	 * 복구가 이렇게 거친 이유는, 컨트롤러 리셋이 그 아래 모든 장치의 상태를
	 * 날려 버리기 때문이다 — 표준 AER 복구처럼 드라이버에게 물어보는 절차를
	 * 쓸 수 없다. */
	pci_stop_and_remove_bus_device_locked(pdev);
	/* [한국어] 조회로 올린 참조를 놓는다. */
	pci_dev_put(pdev);

	/* [한국어] 펌웨어에 리셋을 요청한다. */
	if (hisi_pcie_port_reset(dev, chip_id, port_id))
		/* [한국어] 실패하면 여기서 끝난다. 이미 장치를 제거한 뒤라 시스템은 그 포트를
		 * 잃은 상태로 남는다. */
		return -EIO;

	/*
	 * The initialization time of subordinate devices after
	 * hot reset is no more than 1s, which is required by
	 * the PCI spec v5.0 sec 6.6.1. The time will shorten
	 * if Readiness Notifications mechanisms are used. But
	 * wait 1s here to adapt any conditions.
	 */
	/* [한국어] 리셋 뒤 링크가 다시 설 시간을 준다. 위 영어 주석이 그 이유를 밝힌다. */
	ssleep(1UL);

	/* add root port and downstream devices */
	/* [한국어] 열거·해제 잠금을 잡고, */
	pci_lock_rescan_remove();
	/* [한국어] 루트 버스를 다시 훑어 장치들을 되살린다. 제거와 재스캔이 짝을 이루는
	 * 복구 방식이다. */
	pci_rescan_bus(root_bus);
	/* [한국어] 잠금을 푼다. */
	pci_unlock_rescan_remove();

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * hisi_pcie_handle_error - 오류 레코드를 로그로 풀어 쓰고, 복구 가능하면 복구한다
 *
 * @pdev: 이 오류 핸들러의 플랫폼 장치.
 * @edata: 펌웨어가 준 오류 레코드.
 *
 * 두 가지를 한다 — 사람이 읽을 로그를 남기고, 심각도가 복구 가능일 때만
 * 실제 복구를 시도한다.
 *
 * val_bits 가 이 함수 전체를 지배한다. 펌웨어가 채우지 못한 필드가 있을 수
 * 있으므로, 필드마다 대응 비트를 확인한 뒤에만 읽는다. 비트가 하나도 서
 * 있지 않으면 곧바로 물러난다.
 *
 * err_misc 배열은 비트 9번부터를 시작으로 삼아 for_each_set_bit_from() 으로
 * 훑는다. 33개 레지스터 중 실제로 유효한 것만 찍히며, 비트 번호에서 시작
 * 번호를 빼면 배열 인덱스가 된다.
 *
 * 복구를 복구 가능(RECOVERABLE)에만 시도하는 것이 판단의 핵심이다. 치명적
 * 오류는 리셋해도 소용이 없고, 이미 정정된 오류는 복구할 것이 없다.
 *
 * [상류 코드 관찰] 버전을 찍는 조건이 VERSION 비트가 아니라 SOC_ID 비트다.
 * 그 결과 두 비트가 어긋난 레코드에서는 버전이 출력되지 않으며, soc_id
 * 필드 자체는 이 함수 어디에서도 찍히지 않는다.
 *
 * 실행 컨텍스트: GHES 알림 처리. 프로세스 컨텍스트이며 복구가 1초 이상
 * 잠들 수 있다.
 *
 * 에러 경로: 복구 실패는 로그만 남긴다. 반환값이 없어 상위에 알릴 방법이 없다.
 *
 * 호출 체인:
 *   hisi_pcie_notify_error() → [이 함수]
 *     → hisi_pcie_get_string() ×2 → hisi_pcie_port_do_recovery()
 */
static void hisi_pcie_handle_error(struct platform_device *pdev,
				   const struct hisi_pcie_error_data *edata)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 순회 인덱스와 복구 결과. */
	int idx, rc;
	/* [한국어] val_bits 를 비트맵 형식으로 바꾼다. for_each_set_bit_from() 이
	 * unsigned long 배열을 요구하기 때문이다. */
	const unsigned long valid_bits[] = {BITMAP_FROM_U64(edata->val_bits)};

	/* [한국어] 유효한 정보가 하나도 없으면, */
	if (edata->val_bits == 0) {
		/* [한국어] 경고를 남기고, */
		dev_warn(dev, "%s: no valid error information\n", __func__);
		return;
	}

	/* [한국어] 오류 보고의 시작을 알린다. */
	dev_info(dev, "\nHISI : HIP : PCIe controller error\n");
	/* [한국어] SoC ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SOC_ID)
		/* [한국어] 버전을 찍는다.
		 * [상류 코드 관찰] 검사하는 비트는 SOC_ID 인데 찍는 값은 version 이다.
		 *   바로 위 VERSION 비트와 짝이 어긋나 있으며, 그 결과 VERSION 비트만
		 *   서 있고 SOC_ID 가 꺼진 레코드에서는 버전이 출력되지 않는다.
		 *   또 soc_id 필드를 찍는 곳이 이 함수 어디에도 없다. */
		dev_info(dev, "Table version = %d\n", edata->version);
	/* [한국어] 소켓 ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SOCKET_ID)
		/* [한국어] 찍는다. */
		dev_info(dev, "Socket ID = %d\n", edata->socket_id);
	/* [한국어] Nimbus ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_NIMBUS_ID)
		/* [한국어] 찍는다. */
		dev_info(dev, "Nimbus ID = %d\n", edata->nimbus_id);
	/* [한국어] 서브 모듈 ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_SUB_MODULE_ID)
		/* [한국어] 표에서 이름을 찾아 찍는다. */
		dev_info(dev, "Sub Module = %s\n",
			 hisi_pcie_get_string(hisi_pcie_sub_module,
				      ARRAY_SIZE(hisi_pcie_sub_module),
				      edata->sub_module_id));
	/* [한국어] 코어 ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_CORE_ID)
		/* [한국어] 찍는다. */
		dev_info(dev, "Core ID = core%d\n", edata->core_id);
	/* [한국어] 포트 ID 가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_PORT_ID)
		/* [한국어] 찍는다. */
		dev_info(dev, "Port ID = port%d\n", edata->port_id);
	/* [한국어] 심각도가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_ERR_SEVERITY)
		/* [한국어] 표에서 이름을 찾아 찍는다. */
		dev_info(dev, "Error severity = %s\n",
			 hisi_pcie_get_string(hisi_pcie_error_sev,
				      ARRAY_SIZE(hisi_pcie_error_sev),
				      edata->err_severity));
	/* [한국어] 오류 종류가 유효하면, */
	if (edata->val_bits & HISI_PCIE_LOCAL_VALID_ERR_TYPE)
		/* [한국어] 16진수로 찍는다. 이쪽만 문자열 표가 없다. */
		dev_info(dev, "Error type = 0x%x\n", edata->err_type);

	/* [한국어] 레지스터 덤프의 시작을 알린다. */
	dev_info(dev, "Reg Dump:\n");
	/* [한국어] err_misc 의 유효 비트가 시작되는 번호. */
	idx = HISI_PCIE_LOCAL_VALID_ERR_MISC;
	/* [한국어] 그 번호부터 세워진 비트만 훑는다. _from 판이라 시작 위치를 지정할 수
	 * 있고, 상한은 시작 + 레지스터 개수다. */
	for_each_set_bit_from(idx, valid_bits,
			      HISI_PCIE_LOCAL_VALID_ERR_MISC + HISI_PCIE_ERR_MISC_REGS)
		/* [한국어] 비트 번호에서 시작 번호를 빼면 배열 인덱스가 된다. 같은 뺄셈이 이름과
		 * 값 양쪽에 쓰인다. */
		dev_info(dev, "ERR_MISC_%d = 0x%x\n", idx - HISI_PCIE_LOCAL_VALID_ERR_MISC,
			 edata->err_misc[idx - HISI_PCIE_LOCAL_VALID_ERR_MISC]);

	/* [한국어] 복구 가능한 오류가 아니면, */
	if (edata->err_severity != HISI_PCIE_ERR_SEV_RECOVERABLE)
		/* [한국어] 보고만 하고 끝낸다. 치명적 오류는 복구해도 소용이 없고, 정정된 오류는
		 * 복구할 것이 없기 때문이다. */
		return;

	/* Recovery for the PCIe controller errors, try reset
	 * PCI port for the error recovery
	 */
	/* [한국어] 위 영어 주석대로 컨트롤러 리셋으로 복구를 시도한다. 세 매크로가
	 * 코어 ID 와 포트 ID 를 소켓 기준 번호로 합쳐 넘긴다. */
	rc = hisi_pcie_port_do_recovery(pdev, edata->socket_id,
			HISI_PCIE_PORT_ID(edata->core_id, edata->port_id));
	/* [한국어] 실패하면, */
	if (rc)
		/* [한국어] 기록만 한다. 반환값이 없어 상위에 알릴 방법이 없다. */
		dev_info(dev, "fail to do hisi pcie port reset\n");
}

/* [한국어]
 * hisi_pcie_notify_error - GHES 레코드가 우리 것인지 세 겹으로 가려 낸다
 *
 * @nb: 등록해 둔 notifier_block.
 * @event: 알림 종류. 쓰지 않는다.
 * @data: struct acpi_hest_generic_data — 오류 레코드 헤더.
 * @return: NOTIFY_OK = 처리함, NOTIFY_DONE = 우리 것이 아님.
 *
 * GHES(Generic Hardware Error Source)는 펌웨어가 하드웨어 오류를 커널에
 * 알리는 ACPI 표준 통로다. 벤더 고유 레코드는 모든 구독자에게 뿌려지므로,
 * 자기 것을 가려내는 것이 이 콜백의 첫 일이다.
 *
 * 세 겹으로 거른다.
 * 1. 섹션 GUID 가 이 드라이버의 것인가. 다른 벤더의 레코드를 걸러 낸다.
 * 2. 이 장치에 "socket" 속성이 있는가. 없으면 판정할 근거가 없다.
 * 3. 레코드의 소켓 번호가 이 장치의 것과 같은가. 소켓마다 이 드라이버가
 *    하나씩 붙으므로, 각자 자기 소켓 것만 처리한다.
 *
 * 세 경우 모두 NOTIFY_DONE 을 돌려주어 다른 구독자가 이어서 보게 한다.
 * NOTIFY_STOP 이 아니라는 점이 중요하다 — 남의 레코드를 가로채면 안 된다.
 *
 * 실행 컨텍스트: GHES 알림 사슬. 프로세스 컨텍스트이며, 아래 처리가
 * 1초 이상 잠들 수 있다.
 *
 * 에러 경로: 없다. 판정 결과가 반환값이다.
 *
 * 호출 체인:
 *   펌웨어 오류 보고 → APEI/GHES 코어 → 벤더 레코드 알림 사슬 → [이 함수]
 *     → hisi_pcie_handle_error()
 */
static int hisi_pcie_notify_error(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	/* [한국어] GHES 코어가 넘긴 오류 레코드 헤더. */
	struct acpi_hest_generic_data *gdata = data;
	/* [한국어] 그 뒤에 붙은 벤더 고유 데이터. 이것이 위 구조체다. */
	const struct hisi_pcie_error_data *error_data = acpi_hest_get_payload(gdata);
	/* [한국어] 등록 시 넘겨 둔 상태. */
	struct hisi_pcie_error_private *priv;
	/* [한국어] 이 핸들러의 device. */
	struct device *dev;
	/* [한국어] 그것을 품은 플랫폼 장치. */
	struct platform_device *pdev;
	/* [한국어] 레코드가 밝힌 섹션 GUID. */
	guid_t err_sec_guid;
	/* [한국어] 이 장치가 담당하는 소켓 번호. */
	u8 socket;

	/* [한국어] 레코드 헤더의 섹션 타입을 GUID 로 읽어, */
	import_guid(&err_sec_guid, gdata->section_type);
	/* [한국어] 우리 GUID 가 아니면 남의 레코드다. */
	if (!guid_equal(&err_sec_guid, &hisi_pcie_sec_guid))
		/* [한국어] 처리하지 않았음을 알린다. 다른 구독자가 이어서 볼 수 있게 한다. */
		return NOTIFY_DONE;

	/* [한국어] notifier_block 에서 바깥 상태를 되찾는다. */
	priv = container_of(nb, struct hisi_pcie_error_private, nb);
	/* [한국어] 그 안의 device. */
	dev = priv->dev;

	/* [한국어] DT/ACPI 속성에서 이 장치가 담당하는 소켓 번호를 읽는다. */
	if (device_property_read_u8(dev, "socket", &socket))
		/* [한국어] 속성이 없으면 판정할 수 없으므로 넘긴다. */
		return NOTIFY_DONE;

	/* [한국어] 레코드의 소켓과 다르면 다른 소켓의 오류다. 소켓마다 이 드라이버가
	 * 하나씩 붙으므로, 각자 자기 소켓 것만 처리한다. */
	if (error_data->socket_id != socket)
		/* [한국어] 넘긴다. */
		return NOTIFY_DONE;

	/* [한국어] device 에서 플랫폼 장치를 되찾는다. */
	pdev = container_of(dev, struct platform_device, dev);
	/* [한국어] 실제 처리로 넘긴다. */
	hisi_pcie_handle_error(pdev, error_data);

	/* [한국어] 처리했음을 알린다. */
	return NOTIFY_OK;
}

/* [한국어]
 * hisi_pcie_error_handler_probe - GHES 벤더 레코드 알림에 콜백을 등록한다
 *
 * @pdev: 매치된 플랫폼 장치.
 * @return: 0 = 성공, -ENOMEM = 할당 실패, 그 밖에 등록 오류.
 *
 * 이 드라이버가 하는 준비의 전부다. 하드웨어를 전혀 만지지 않으며,
 * 레지스터도 인터럽트도 잡지 않는다 — 오류 보고가 모두 펌웨어를 거쳐
 * GHES 로 오기 때문이다.
 *
 * notifier_block 을 구조체의 첫 필드로 두는 배치가 여기서 값을 한다.
 * 콜백은 nb 포인터만 받으므로, container_of 로 device 를 되찾아야
 * 소켓 번호를 읽을 수 있다.
 *
 * remove 함수가 없다. devm_ghes_register_vendor_record_notifier() 가 devres
 * 액션으로 해제를 걸어 두므로, 드라이버가 떨어질 때 커널이 알아서 뗀다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 모두 devm_ 이라 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   module_platform_driver → ACPI 매치(HISI0361) → [이 함수]
 *     → devm_kzalloc() → devm_ghes_register_vendor_record_notifier()
 */
static int hisi_pcie_error_handler_probe(struct platform_device *pdev)
{
	/* [한국어] 할당할 상태. */
	struct hisi_pcie_error_private *priv;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 상태를 devm 으로 할당한다. */
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!priv)
		return -ENOMEM;

	/* [한국어] 알림 콜백을 연결한다. */
	priv->nb.notifier_call = hisi_pcie_notify_error;
	/* [한국어] device 를 기록해 콜백이 소켓 번호를 읽을 수 있게 한다. */
	priv->dev = &pdev->dev;
	/* [한국어] GHES 벤더 레코드 알림에 등록한다. devm_ 이라 해제가 자동이다. */
	ret = devm_ghes_register_vendor_record_notifier(&pdev->dev, &priv->nb);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고, */
		dev_err(&pdev->dev,
			"Failed to register hisi pcie controller error handler with apei\n");
		return ret;
	}

	/* [한국어] 성공. 이 시점부터 펌웨어가 보내는 오류 레코드가 콜백으로 들어온다. */
	return 0;
}

static const struct acpi_device_id hisi_pcie_acpi_match[] = {
	/* [한국어] 이 오류 핸들러의 ACPI HID. */
	{ "HISI0361", 0 },
	/* [한국어] 배열 끝. */
	{ }
};

static struct platform_driver hisi_pcie_error_handler_driver = {
	.driver = {
		/* [한국어] sysfs 와 로그에 보일 이름. */
		.name	= "hisi-pcie-error-handler",
		/* [한국어] 위 ACPI 매치 표. of_match_table 이 아니라 acpi_match_table 인 것은
		 * 이 드라이버가 ACPI 로만 동작하기 때문이다 — GHES 자체가 ACPI 기능이다. */
		.acpi_match_table = hisi_pcie_acpi_match,
	},
	/* [한국어] probe. remove 는 없는데, devres 가 알림 해제를 맡기 때문이다. */
	.probe		= hisi_pcie_error_handler_probe,
};
/* [한국어] 모듈로 등록한다. */
module_platform_driver(hisi_pcie_error_handler_driver);

/* [한국어] 모듈 설명. */
MODULE_DESCRIPTION("HiSilicon HIP PCIe controller error handling driver");
