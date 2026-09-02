// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express hardware monitoring support
 * Copyright (c) 2019, Guenter Roeck
 */

/*
 * [한국어 설명] NVMe 컨트롤러 SMART/온도 하드웨어 모니터링 (drivers/nvme/host/hwmon.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe 컨트롤러가 보고하는 온도·임계값·크리티컬 경고를 리눅스
 * hwmon 서브시스템(sysfs: /sys/class/hwmon/hwmonX/)에 노출하는 브리지이다.
 * 센서 값은 주로 Get Log Page(Log Identifier = SMART / Health Information)의
 * Composite Temperature 와 Temperature Sensor 1..8 필드에서 읽고, 경고/임계
 * 설정은 Get/Set Features(FID = Temperature Threshold, NVME_FEAT_TEMP_THRESH)
 * 로 over/under 임계를 조회·변경한다. Critical Composite Temperature(cctemp)와
 * Warning Composite Temperature(wctemp)는 Identify Controller 단계에서 이미
 * ctrl 에 캐시된 값을 그대로 millicelsius 로 변환해 보여 준다. 즉 데이터 경로
 * I/O 와 무관한 관리 평면(management plane) 기능이며, lm-sensors 등 사용자
 * 공간 도구가 NVMe SSD 온도를 표준 hwmon ABI 로 읽게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 컨트롤러 생존 주기 중 Identify 이후 초기화 말미에 nvme_hwmon_init(ctrl) 이
 * 호출되어 hwmon 디바이스를 등록하고, 제거/리셋 정리 시 nvme_hwmon_exit() 이
 * 해제한다(core.c). sysfs 읽기 경로는:
 *   유저 open/read hwmon sysfs → hwmon 코어 → nvme_hwmon_ops.read/is_visible
 *   → (임계) nvme_get_features TEMP_THRESH 또는 (입력/알람) Get Log SMART.
 * 쓰기는 temp_max/temp_min 에 대해 nvme_set_features 로 under/over 임계 갱신.
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs/ioctl 계열), Admin 큐 동기 명령으로
 * sleep 가능. 인터럽트 컨텍스트에서 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/core.c: Identify 로 wctemp/cctemp 채움; 컨트롤러 초기화
 *   성공 후 nvme_hwmon_init, 종료 시 nvme_hwmon_exit; nvme_get_log /
 *   nvme_get_features / nvme_set_features Admin 헬퍼 제공.
 * - drivers/hwmon (커널 hwmon 코어): hwmon_device_register_with_info,
 *   hwmon_ops, HWMON_CHANNEL_INFO 매크로로 채널·속성 비트마스크 등록.
 * - include/linux/nvme.h / nvme host nvme.h: SMART log 레이아웃, TEMP_THRESH
 *   비트필드, ctrl->quirks (NO_TEMP_THRESH_CHANGE, NO_SECONDARY_TEMP_THRESH),
 *   ctrl->hwmon_device 포인터.
 * 데이터 흐름: Admin Get Log SMART → data->log 캐시 → millicelsius sysfs;
 * Get/Set Features TEMP_THRESH ↔ over/under 임계. 공유 상태: struct
 * nvme_hwmon_data (ctrl, log, read_lock), ctrl->hwmon_device.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvme_hwmon_data: 컨트롤러 포인터, SMART log 버퍼, 로그 읽기 직렬화 뮤텍스.
 * - nvme_get/set_temp_thresh: Feature TEMP_THRESH 로 센서별 over/under 임계 R/W.
 * - nvme_hwmon_get_smart_log: Get Log Page SMART 전체 갱신.
 * - nvme_hwmon_read/write/read_string/is_visible: hwmon_ops 콜백.
 * - nvme_hwmon_init/exit: 등록·해제 공개 API (CONFIG_NVME_HWMON 가드와 짝).
 */

#include <linux/hwmon.h>		/* [한국어] hwmon_device_register_with_info, hwmon_ops, HWMON_CHANNEL_INFO, enum hwmon_sensor_types/속성 상수 — sysfs 센서 ABI 등록에 필수 */
#include <linux/units.h>		/* [한국어] kelvin_to_millicelsius / millicelsius_to_kelvin — NVMe 는 Kelvin, hwmon ABI 는 millidegree Celsius 를 쓰므로 단위 변환 */
#include <linux/unaligned.h>	/* [한국어] get_unaligned_le16 — SMART log 의 composite temperature 필드가 정렬 보장 없는 packed 레이아웃일 수 있어 안전 로드 */

#include "nvme.h"		/* [한국어] nvme_ctrl, nvme_get_log/features/set_features, SMART log·TEMP_THRESH 마스크, quirks, cctemp/wctemp 필드 */

/*
 * [한국어] 한 NVMe 컨트롤러의 hwmon 인스턴스가 보유하는 런타임 상태.
 * hwmon 디바이스의 drvdata 로 저장되며 init 에서 할당, exit 에서 해제한다.
 */
struct nvme_hwmon_data {
	struct nvme_ctrl *ctrl;
	/* [한국어] 이 센서 세트가 속한 NVMe 컨트롤러.
	 * 설정자: nvme_hwmon_init() 가 data->ctrl = ctrl.
	 * 읽는 자: read/write/is_visible 이 Admin 명령·cctemp/wctemp/quirks 접근.
	 * 값 범위: 유효한 수명 내의 nvme_ctrl * (hwmon 등록 기간 동안 컨트롤러보다
	 *          먼저 해제되지 않도록 exit 순서가 보장되어야 함).
	 * 동기화: 포인터 자체는 init 이후 불변; 가리키는 ctrl 필드 접근은 각 Admin
	 *          헬퍼/컨트롤러 락 관례에 따름. */

	struct nvme_smart_log *log;
	/* [한국어] 마지막(또는 직전 읽기에서 갱신한) SMART/Health log 페이지 버퍼.
	 * 설정자: init 시 할당 후 nvme_hwmon_get_smart_log; 이후 read 경로가 동일
	 *         버퍼에 덮어쓰기. is_visible 은 등록 직후 스냅샷의 temp_sensor[]
	 *         존재 여부로 채널 노출을 결정.
	 * 읽는 자: nvme_hwmon_read 의 temp_input/alarm; is_visible 의 secondary 센서.
	 * 값 범위: sizeof(nvme_smart_log) 유효 버퍼; 온도 필드는 Kelvin.
	 * 동기화: read_lock 뮤텍스로 SMART 재읽기+필드 소비를 직렬화. */

	struct mutex read_lock;
	/* [한국어] SMART log 갱신과 그 결과 필드 파싱을 한 임계 구역으로 묶는 뮤텍스.
	 * 여러 sysfs 읽기가 동시에 Get Log 를 날리거나 반쯤 갱신된 log 를 읽는 것을
	 * 방지한다. 임계값 Get/Set Features 경로는 이 락을 쓰지 않는다(로그 버퍼 무관).
	 * 설정/사용: init 에서 mutex_init; nvme_hwmon_read 의 SMART 의존 속성만 lock.
	 * 동기화: 프로세스 컨텍스트 sleep 가능 락. */
};

/*
 * [한국어]
 * nvme_get_temp_thresh - Get Features(TEMP_THRESH)로 센서 over/under 임계 온도 조회
 *
 * @ctrl: 대상 컨트롤러 (Admin 큐).
 * @sensor: 0=composite, 1.. = 선택 센서 인덱스 (스펙 SELECT 필드에 시프트).
 * @under: true 면 under 임계 타입 비트 설정, false 면 over(기본) 임계.
 * @temp: 출력, millicelsius.
 * @return: 0 성공, -EIO(양수 NVMe status), 또는 음수 커널/전송 에러.
 *
 * 왜 필요한가: hwmon temp_max/temp_min 은 장치가 경고를 올리기 시작하는 임계를
 * 보여 주며, SMART 로그의 순시 온도와 달리 Feature 로 관리되는 설정값이다.
 * 호출 체인: nvme_hwmon_read(temp_max/min) → [여기] → nvme_get_features.
 * 실행 컨텍스트: 프로세스, Admin 동기. 락: read_lock 불필요.
 */
static int nvme_get_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
				long *temp)
{
	unsigned int threshold = sensor << NVME_TEMP_THRESH_SELECT_SHIFT;	/* [한국어] CDW11 에 넣을 선택 필드: 센서 번호를 스펙 SELECT 비트 위치로 시프트한 기저값 */
	u32 status;	/* [한국어] Get Features 성공 시 컨트롤러가 돌려주는 현재 feature 값(임계 Kelvin 등이 비트필드에 인코딩) */
	int ret;	/* [한국어] nvme_get_features 반환 — 0 성공, 양수 NVMe status, 음수 로컬 에러 */

	if (under)	/* [한국어] hwmon temp_min 경로 — under-temperature threshold 타입 요청 */
		threshold |= NVME_TEMP_THRESH_TYPE_UNDER;	/* [한국어] 스펙 TEMP_THRESH Type 비트: under 임계를 고르지 않으면 기본 over 임계가 반환됨 */

	ret = nvme_get_features(ctrl, NVME_FEAT_TEMP_THRESH, threshold, NULL, 0,
				&status);	/* [한국어] Admin Get Features FID=Temperature Threshold; 데이터 버퍼 없음, 결과는 status DW 에 수신 */
	if (ret > 0)	/* [한국어] 장치가 상태 코드로 실패한 완료 — 센서 미지원·잘못된 SELECT 등 */
		return -EIO;	/* [한국어] hwmon/sysfs 에 NVMe status 원값을 노출하지 않고 I/O 에러로 정규화 */
	if (ret < 0)	/* [한국어] 큐 다운·할당 실패 등 호스트 측 에러 — 그대로 상위에 전달 */
		return ret;	/* [한국어] 음수 errno 유지 */
	*temp = kelvin_to_millicelsius(status & NVME_TEMP_THRESH_MASK);	/* [한국어] feature 값 하위 온도 비트만 추출해 Kelvin→m°C 로 변환 후 유저 가시 값 채움 */

	return 0;	/* [한국어] *temp 유효 */
}

/*
 * [한국어]
 * nvme_set_temp_thresh - Set Features(TEMP_THRESH)로 over/under 임계 온도 설정
 *
 * @ctrl: 대상 컨트롤러.
 * @sensor: 센서 SELECT 인덱스.
 * @under: under 임계 여부.
 * @temp: 사용자/hwmon 이 요청한 millicelsius (함수 내부에서 Kelvin 으로 변환).
 * @return: 0 또는 음수 에러; 양수 NVMe status 는 -EIO.
 *
 * 왜 필요한가: lm-sensors 등이 temp_max/temp_min 에 write 하면 컨트롤러 경고
 * 임계를 런타임 변경한다. 스펙 마스크로 clamp 해 잘못된 큰 값이 CDW 에 들어가지
 * 않게 한다. 일부 장치는 quirks 로 is_visible 이 쓰기를 막아 이 함수가 안 불린다.
 * 호출 체인: nvme_hwmon_write → [여기] → nvme_set_features.
 */
static int nvme_set_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
				long temp)
{
	unsigned int threshold = sensor << NVME_TEMP_THRESH_SELECT_SHIFT;	/* [한국어] 대상 센서 SELECT 비트를 담은 feature DWORD 기저 */
	int ret;	/* [한국어] Set Features 결과 */

	temp = millicelsius_to_kelvin(temp);	/* [한국어] hwmon ABI(m°C) → NVMe Feature 가 기대하는 Kelvin 정수 */
	threshold |= clamp_val(temp, 0, NVME_TEMP_THRESH_MASK);	/* [한국어] 스펙 온도 필드 비트 폭으로 포화 — 상위 비트(SELECT/Type)와 충돌·오버플로 방지 */

	if (under)	/* [한국어] temp_min 쓰기 — under-temperature threshold 설정 */
		threshold |= NVME_TEMP_THRESH_TYPE_UNDER;	/* [한국어] Type=Under 비트를 켜 over 임계 슬롯을 덮어쓰지 않음 */

	ret = nvme_set_features(ctrl, NVME_FEAT_TEMP_THRESH, threshold, NULL, 0,
				NULL);	/* [한국어] Admin Set Features 로 컨트롤러 NVM 서브시스템에 임계 반영; 추가 데이터 버퍼 없음 */
	if (ret > 0)	/* [한국어] 장치가 거부한 완료 상태 */
		return -EIO;	/* [한국어] sysfs write 실패로 보이도록 정규화 */

	return ret;	/* [한국어] 0 성공 또는 음수 호스트 에러 */
}

/*
 * [한국어]
 * nvme_hwmon_get_smart_log - 컨트롤러 전역 SMART/Health 로그 페이지를 data->log 에 갱신
 *
 * @data: 대상 hwmon 데이터 (ctrl + log 버퍼).
 * @return: nvme_get_log 의 반환값 (0 성공 등).
 *
 * NSID=ALL, Log=SMART, CSI=NVM 으로 표준 건강 로그를 읽는다. composite/센서 온도·
 * critical_warning 비트가 이 페이지에 있다. init 시 1회, 이후 temp_input/alarm
 * 읽기마다 호출되어 최신값을 보장한다(read_lock 하에서).
 * 호출 체인: init / nvme_hwmon_read → [여기] → nvme_get_log → Admin Get Log Page.
 */
static int nvme_hwmon_get_smart_log(struct nvme_hwmon_data *data)
{
	return nvme_get_log(data->ctrl, NVME_NSID_ALL, NVME_LOG_SMART, 0,
			   NVME_CSI_NVM, data->log, sizeof(*data->log), 0);	/* [한국어] 네임스페이스 비종속 컨트롤러 SMART 로그 전체를 data->log 에 DMA 수신; 온도·크리티컬 경고 필드의 단일 공급원 */
}

/*
 * [한국어]
 * nvme_hwmon_read - hwmon sysfs 가 온도/알람 속성을 읽을 때 호출되는 ops.read
 *
 * @dev: hwmon device (drvdata = nvme_hwmon_data).
 * @type: 센서 타입(여기서는 온도 채널 전제).
 * @attr: hwmon_temp_* 속성.
 * @channel: 0=Composite, 1..8=개별 센서.
 * @val: 출력 long (m°C 또는 alarm 0/1).
 * @return: 0, -EOPNOTSUPP, -EIO 등.
 *
 * 임계·크리티컬은 SMART 없이 ctrl/Feature 에서 즉시 처리. input/alarm 만
 * read_lock + SMART 갱신 후 파싱. channel0 temperature 는 unaligned LE16.
 * 호출 체인: hwmon 코어 sysfs → [nvme_hwmon_read] → get_temp_thresh 또는 get_smart_log.
 */
static int nvme_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct nvme_hwmon_data *data = dev_get_drvdata(dev);	/* [한국어] 등록 시 넘긴 컨트롤러별 hwmon 상태 — log/ctrl/lock 접근의 시작점 */
	struct nvme_smart_log *log = data->log;	/* [한국어] SMART 버퍼 포인터 로컬화; 락 구간에서 최신 내용이 채워진 뒤 필드 소비 */
	int temp;	/* [한국어] Kelvin 단위 원시 온도(합성 또는 개별 센서) */
	int err;	/* [한국어] SMART 읽기 또는 속성 처리 에러 코드 */

	/*
	 * First handle attributes which don't require us to read
	 * the smart log.
	 */
	/* [한국어 설명] 임계·크리티컬은 Identify/Get Features 경로라 SMART Get Log
	 * 비용을 들이지 않는다. 락도 잡지 않아 불필요한 Admin 직렬화를 피한다. */
	switch (attr) {
	case hwmon_temp_max:	/* [한국어] over-temperature threshold — Get Features TEMP_THRESH (Type over) */
		return nvme_get_temp_thresh(data->ctrl, channel, false, val);	/* [한국어] under=false 로 센서별 상한 임계를 m°C 로 *val 에 채움 */
	case hwmon_temp_min:	/* [한국어] under-temperature threshold */
		return nvme_get_temp_thresh(data->ctrl, channel, true, val);	/* [한국어] under=true 로 하한 임계 조회 */
	case hwmon_temp_crit:	/* [한국어] Critical Composite Temperature — Identify Controller 의 cctemp 캐시 사용 */
		*val = kelvin_to_millicelsius(data->ctrl->cctemp);	/* [한국어] 컨트롤러 치명 온도 한계를 hwmon 단위로 변환; Admin 재조회 없이 즉시 반환 */
		return 0;	/* [한국어] cctemp 는 초기 Identify 이후 불변으로 취급 */
	default:
		break;	/* [한국어] input/alarm 등은 SMART 로그 필요 — 아래 락 구간으로 진행 */
	}

	mutex_lock(&data->read_lock);	/* [한국어] SMART 갱신과 필드 파싱을 직렬화 — 동시 sysfs 읽기가 로그 버퍼를 찢지 않게 함 */
	err = nvme_hwmon_get_smart_log(data);	/* [한국어] 최신 composite/센서 온도·critical_warning 을 얻기 위해 Get Log SMART 발행 */
	if (err)	/* [한국어] 로그 읽기 실패 시 오래된 값으로 오판하지 않도록 속성 파싱 생략 */
		goto unlock;	/* [한국어] 락 해제 후 에러 반환 */

	switch (attr) {
	case hwmon_temp_input:	/* [한국어] 현재 온도 읽기 — channel 0 은 composite, 그 외는 temp_sensor[] */
		if (!channel)	/* [한국어] Composite Temperature: SMART 로그 선두 부근 고정 오프셋, 정렬 비보장 LE16 */
			temp = get_unaligned_le16(log->temperature);	/* [한국어] 컨트롤러 종합 온도 Kelvin — 가장 흔히 lm-sensors 가 읽는 값 */
		else	/* [한국어] 개별 Temperature Sensor 1..8 (channel-1 인덱스) */
			temp = le16_to_cpu(log->temp_sensor[channel - 1]);	/* [한국어] 센서별 Kelvin; 0 이면 is_visible 이 채널을 가렸을 가능성이 큼 */
		*val = kelvin_to_millicelsius(temp);	/* [한국어] hwmon ABI 단위(m°C)로 변환해 sysfs 에 노출 */
		break;
	case hwmon_temp_alarm:	/* [한국어] 온도 관련 크리티컬 경고 비트 존재 여부(복합 알람) */
		*val = !!(log->critical_warning & NVME_SMART_CRIT_TEMPERATURE);	/* [한국어] SMART critical_warning 의 temperature 비트를 0/1 알람 값으로 정규화 */
		break;
	default:	/* [한국어] 이 드라이버가 구현하지 않은 속성 — is_visible 과 불일치 시 방어 */
		err = -EOPNOTSUPP;	/* [한국어] 지원하지 않는 hwmon 속성 */
		break;
	}
unlock:
	mutex_unlock(&data->read_lock);	/* [한국어] SMART 임계 구역 종료 — 다른 리더가 Get Log 진행 가능 */
	return err;	/* [한국어] 0 이면 *val 유효 */
}

/*
 * [한국어]
 * nvme_hwmon_write - hwmon temp_max/temp_min 쓰기 → Set Features TEMP_THRESH
 *
 * @dev: hwmon device.
 * @type/@attr/@channel: 읽기와 동일 의미.
 * @val: 설정할 m°C.
 * @return: set_temp_thresh 결과 또는 -EOPNOTSUPP.
 *
 * is_visible 이 0644 로 연 채널만 실질 호출된다. 호출 체인: sysfs store →
 * hwmon write → [여기] → nvme_set_temp_thresh.
 */
static int nvme_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long val)
{
	struct nvme_hwmon_data *data = dev_get_drvdata(dev);	/* [한국어] 컨트롤러 연결 상태 획득 — Set Features 대상 ctrl */

	switch (attr) {
	case hwmon_temp_max:	/* [한국어] over 임계 설정 요청 */
		return nvme_set_temp_thresh(data->ctrl, channel, false, val);	/* [한국어] under=false 로 TEMP_THRESH 프로그램 */
	case hwmon_temp_min:	/* [한국어] under 임계 설정 요청 */
		return nvme_set_temp_thresh(data->ctrl, channel, true, val);	/* [한국어] under=true 로 하한 임계 프로그램 */
	default:
		break;	/* [한국어] input/crit/alarm 등은 읽기 전용 — 쓰기는 거부 */
	}

	return -EOPNOTSUPP;	/* [한국어] 쓰기 미지원 속성 */
}

/*
 * [한국어] hwmon channel 인덱스 → 사용자 가시 라벨 문자열.
 * channel 0 은 스펙 Composite Temperature, 1..8 은 SMART temp_sensor 슬롯.
 * 설정자: 정적 const. 읽는 자: nvme_hwmon_read_string.
 */
static const char * const nvme_hwmon_sensor_names[] = {
	"Composite",	/* [한국어] 채널 0 — 컨트롤러 종합 온도 라벨 */
	"Sensor 1",	/* [한국어] SMART temp_sensor[0] 대응 라벨 */
	"Sensor 2",
	"Sensor 3",
	"Sensor 4",
	"Sensor 5",
	"Sensor 6",
	"Sensor 7",
	"Sensor 8",	/* [한국어] 스펙상 최대 8개 부가 온도 센서 슬롯 */
};

/*
 * [한국어]
 * nvme_hwmon_read_string - temp_label sysfs 에 센서 이름 문자열 제공
 *
 * channel 을 배열 인덱스로 그대로 사용( is_visible 이 유효 채널만 노출 ).
 * 항상 0 반환. 호출: hwmon 코어 label 속성 읽기.
 */
static int nvme_hwmon_read_string(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel, const char **str)
{
	*str = nvme_hwmon_sensor_names[channel];	/* [한국어] 해당 채널의 고정 라벨 포인터를 hwmon 코어에 넘겨 sysfs label 파일 내용이 됨 */
	return 0;	/* [한국어] 문자열 포인터 유효 — 동적 할당 없음 */
}

/*
 * [한국어]
 * nvme_hwmon_is_visible - 채널·속성 조합별로 sysfs 파일 모드(또는 숨김) 결정
 *
 * @_data: 등록 시 전달한 nvme_hwmon_data (const).
 * @type/@attr/@channel: 검사 대상.
 * @return: 0=속성 숨김, 0444=읽기 전용, 0644=읽기/쓰기.
 *
 * 정책: crit 은 channel0 이고 cctemp 비제로일 때만; max/min 은 composite 의
 * wctemp 또는 부가 센서 값이 존재하고 secondary thresh quirk 가 없을 때;
 * NO_TEMP_THRESH_CHANGE quirk 면 임계를 0444 로 고정; alarm 은 composite 만;
 * input/label 은 composite 또는 비제로 temp_sensor. 장치 능력에 맞춰 빈 센서
 * 노드를 만들지 않는 것이 목적.
 */
static umode_t nvme_hwmon_is_visible(const void *_data,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	const struct nvme_hwmon_data *data = _data;	/* [한국어] init 시 SMART 스냅샷과 ctrl 캐시(wctemp/cctemp/quirks)를 보고 노출 여부 결정 */

	switch (attr) {
	case hwmon_temp_crit:	/* [한국어] Critical 온도 — Identify cctemp 가 있을 때만 composite 채널에 의미 있음 */
		if (!channel && data->ctrl->cctemp)	/* [한국어] channel0 이고 치명 온도가 비제로로 Identify 됨 */
			return 0444;	/* [한국어] 읽기 전용 sysfs 노드 생성 */
		break;
	case hwmon_temp_max:
	case hwmon_temp_min:	/* [한국어] over/under 임계 — 장치가 해당 센서 임계를 지원할 때만 노출 */
		if ((!channel && data->ctrl->wctemp) ||
		    (channel && data->log->temp_sensor[channel - 1] &&
		     !(data->ctrl->quirks &
		       NVME_QUIRK_NO_SECONDARY_TEMP_THRESH))) {	/* [한국어] composite 는 wctemp 존재 시; 부가 센서는 SMART 에 센서값이 있고 secondary thresh quirk 가 없을 때 Feature 임계  meaningfully 지원 */
			if (data->ctrl->quirks &
			    NVME_QUIRK_NO_TEMP_THRESH_CHANGE)	/* [한국어] 일부 컨트롤러는 임계 조회만 되고 Set Features 가 깨지거나 위험 — 쓰기 비활성 */
				return 0444;	/* [한국어] 읽기 전용으로 max/min 노출 */
			return 0644;	/* [한국어] 정상 장치: 유저가 sysfs 로 임계 변경 가능 */
		}
		break;
	case hwmon_temp_alarm:	/* [한국어] SMART temperature critical warning — composite 단위 플래그 */
		if (!channel)	/* [한국어] 개별 센서별 알람 비트는 이 로그 레이아웃에서 다루지 않음 */
			return 0444;	/* [한국어] channel0 알람 파일만 생성 */
		break;
	case hwmon_temp_input:
	case hwmon_temp_label:	/* [한국어] 현재값과 라벨 — 센서가 실제로 존재할 때만 */
		if (!channel || data->log->temp_sensor[channel - 1])	/* [한국어] composite 는 항상; 부가 채널은 init 시 읽은 SMART 에서 비제로 센서만 */
			return 0444;	/* [한국어] 읽기 전용 input/label */
		break;
	default:
		break;	/* [한국어] 미구현 속성은 숨김 */
	}
	return 0;	/* [한국어] 해당 (attr,channel) sysfs 파일 생성 안 함 */
}

/*
 * [한국어] hwmon 채널 디스크립터 테이블.
 * chip 에 thermal zone 등록 플래그, temp 에 composite + 센서 8개 속성 마스크.
 * 실제 파일 생성은 is_visible 이 0 이 아닌 모드를 줄 때만 이루어진다.
 */
static const struct hwmon_channel_info *const nvme_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),	/* [한국어] 칩 수준: thermal zone 으로도 등록 가능함을 표시 — 프레임워크 연동 */
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_CRIT | HWMON_T_LABEL | HWMON_T_ALARM,	/* [한국어] 채널0 Composite: 입력·상하한·크리티컬·라벨·알람 전부 후보 */
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,	/* [한국어] 센서1: crit/alarm 없이 입력·임계·라벨 */
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MIN |
				HWMON_T_LABEL),	/* [한국어] 센서8까지 동일 마스크 — is_visible 이 빈 슬롯 제거 */
	NULL	/* [한국어] 채널 테이블 종료 센티널 — hwmon 코어 순회 종료 */
};

/*
 * [한국어] NVMe hwmon 콜백 벡터 — 등록 시 chip_info.ops 로 연결.
 * is_visible/read/read_string/write 가 sysfs 생명주기를 전부 담당.
 */
static const struct hwmon_ops nvme_hwmon_ops = {
	.is_visible	= nvme_hwmon_is_visible,	/* [한국어] 속성 파일 생성·모드(0444/0644) 결정 */
	.read		= nvme_hwmon_read,		/* [한국어] 온도·알람·임계 읽기 */
	.read_string	= nvme_hwmon_read_string,	/* [한국어] label 문자열 */
	.write		= nvme_hwmon_write,		/* [한국어] 임계 쓰기 */
};

/*
 * [한국어] hwmon_device_register_with_info 에 넘기는 칩 설명자.
 * ops+info 로 채널 토폴로지와 콜백을 한 번에 등록한다.
 */
static const struct hwmon_chip_info nvme_hwmon_chip_info = {
	.ops	= &nvme_hwmon_ops,	/* [한국어] 위 콜백 테이블 */
	.info	= nvme_hwmon_info,	/* [한국어] chip/temp 채널 마스크 배열 */
};

/*
 * [한국어]
 * nvme_hwmon_init - 컨트롤러에 hwmon 디바이스를 붙이고 SMART 초기 스냅샷 확보
 *
 * @ctrl: Identify 까지 끝난 NVMe 컨트롤러 (device, wctemp/cctemp 유효 가정).
 * @return: 0 성공, -ENOMEM 또는 등록/로그 실패 코드(경고 후 실패 반환 가능).
 *
 * 할당 순서: data → log → mutex → SMART 1회 읽기 → hwmon 등록 →
 * ctrl->hwmon_device 저장. SMART 실패 시 센서를 올리지 않고 정리(온도 sysfs
 * 없이 컨트롤러는 계속 동작 가능하도록 호출자가 경고만 할 수 있음 — 반환 코드
 * 전달). 실행: 컨트롤러 초기화 프로세스 컨텍스트.
 * 호출자: core.c 초기화 경로. 실패 시 err_free_* 로 부분 할당 해제.
 *
 * 호출 체인:
 *   nvme_init_ctrl 완료 경로 → [nvme_hwmon_init] → nvme_get_log →
 *   hwmon_device_register_with_info
 */
int nvme_hwmon_init(struct nvme_ctrl *ctrl)
{
	struct device *dev = ctrl->device;	/* [한국어] hwmon 부모 device — sysfs 계층에서 컨트롤러 디바이스 아래 센서가 매달림 */
	struct nvme_hwmon_data *data;	/* [한국어] 이번에 생성할 컨트롤러별 hwmon 상태 */
	struct device *hwmon;	/* [한국어] 등록 성공 시 hwmon class device 포인터 */
	int err;	/* [한국어] 단계별 에러 — 공통 해제 라벨로 전파 */

	data = kzalloc_obj(*data);	/* [한국어] hwmon 상태 0 초기화 할당 — 실패 시 센서 기능 전체 스킵 */
	if (!data)	/* [한국어] 상태 구조체 없이는 log/락/등록 진행 불가 */
		return -ENOMEM;	/* [한국어] 호출자가 hwmon 없이 컨트롤러를 유지하거나 상향 처리 */

	data->log = kzalloc_obj(*data->log);	/* [한국어] SMART 로그 페이지 상주 버퍼 — 매 읽기마다 재할당하지 않고 덮어씀 */
	if (!data->log) {	/* [한국어] 온도 원천 버퍼 실패 */
		err = -ENOMEM;	/* [한국어] data 만 해제하는 경로로 */
		goto err_free_data;	/* [한국어] log 없는 data 해제 */
	}

	data->ctrl = ctrl;	/* [한국어] Admin 명령·임계 캐시 접근을 위한 역참조 고정 */
	mutex_init(&data->read_lock);	/* [한국어] 이후 sysfs 동시 읽기용 SMART 직렬화 락 초기화 */

	err = nvme_hwmon_get_smart_log(data);	/* [한국어] 등록 전 1회 SMART 를 읽어 is_visible 이 temp_sensor[] 존재 여부를 판단할 스냅샷 확보 */
	if (err) {	/* [한국어] 로그를 못 읽으면 어떤 센서 채널을 열지 믿을 수 없고 input 도 불가 */
		dev_warn(dev, "Failed to read smart log (error %d)\n", err);	/* [한국어] 운영자가 펌웨어/Admin 경로 문제를 인지하도록 경고 — 컨트롤러 자체 실패는 아님 */
		goto err_free_log;	/* [한국어] log+data 해제, hwmon 미등록 */
	}

	hwmon = hwmon_device_register_with_info(dev, "nvme",
						data, &nvme_hwmon_chip_info,
						NULL);	/* [한국어] 이름 "nvme", drvdata=data, 채널/ops 는 chip_info; 추가 groups NULL */
	if (IS_ERR(hwmon)) {	/* [한국어] class/sysfs 등록 실패 */
		dev_warn(dev, "Failed to instantiate hwmon device\n");	/* [한국어] 센서 노드 생성 실패 로그 */
		err = PTR_ERR(hwmon);	/* [한국어] 커널 에러 코드 추출 */
		goto err_free_log;	/* [한국어] 할당 자원 롤백 */
	}
	ctrl->hwmon_device = hwmon;	/* [한국어] exit 경로에서 unregister 및 drvdata 회수에 사용할 핸들 저장 */
	return 0;	/* [한국어] sysfs 에 온도 노드 노출 시작 */

err_free_log:
	kfree(data->log);	/* [한국어] SMART 버퍼 해제 */
err_free_data:
	kfree(data);	/* [한국어] hwmon 상태 해제 — ctrl->hwmon_device 는 설정되지 않은 상태 */
	return err;	/* [한국어] 실패 코드 상향 */
}

/*
 * [한국어]
 * nvme_hwmon_exit - hwmon 디바이스 등록 해제 및 상태/로그 버퍼 수명 종료
 *
 * @ctrl: hwmon_device 가 NULL 이 아닐 수 있는 컨트롤러.
 *
 * 컨트롤러 제거·모듈 언로드·초기화 롤백에서 호출. unregister 후 drvdata 로
 * data 를 꺼내 log/data 를 해제한다. hwmon_device 가 NULL 이면 no-op (init
 * 실패 또는 이미 정리됨). 호출자: core.c 정리 경로.
 *
 * 호출 체인:
 *   nvme_uninit / 제거 → [nvme_hwmon_exit] → hwmon_device_unregister → kfree
 */
void nvme_hwmon_exit(struct nvme_ctrl *ctrl)
{
	if (ctrl->hwmon_device) {	/* [한국어] init 이 성공해 핸들이 남아 있는 경우에만 해제 — 이중 free 방지 */
		struct nvme_hwmon_data *data =
			dev_get_drvdata(ctrl->hwmon_device);	/* [한국어] 등록 시 심어 둔 data 포인터 회수 — log 버퍼 소유권의 열쇠 */

		hwmon_device_unregister(ctrl->hwmon_device);	/* [한국어] sysfs 노드 제거 및 hwmon 코어 참조 해제 — 이후 read 콜백 진입 없음 */
		ctrl->hwmon_device = NULL;	/* [한국어] 컨트롤러 필드 잔존 포인터 제거 — 재진입 exit 시 no-op */
		kfree(data->log);	/* [한국어] SMART 상주 버퍼 해제 */
		kfree(data);	/* [한국어] hwmon 상태 구조체 해제 */
	}
}
