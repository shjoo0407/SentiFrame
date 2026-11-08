# SentiFrame 펌웨어

안경에 들어가는 nRF54L15(Raytac AN54LV-15 모듈) 펌웨어다. Zephyr 기반이다.

하는 일

- IMU(BMI270)를 100Hz로 읽고, 샘플마다 낙상을 판정한다 → [낙상 판정 알고리즘](#낙상-판정-알고리즘)
- PPG(MAXM86161)를 약 50Hz로 읽는다 (초록 LED만 사용)
- 충전(BQ25186)과 USB 전원 감지를 제어한다
- USB(CP2102N)나 BLE로 수신기에 데이터와 이벤트를 보낸다 → [PROTOCOL.md](PROTOCOL.md)
- SOS 버튼, LED

## 폴더

| 경로 | 내용 |
|---|---|
| `src/main.c` | 메인 루프 — 센서 읽기, 낙상 판정 호출, 이벤트·상태 전송 |
| `src/core/fall.c` | 낙상 판정 |
| `src/core/protocol.c` | 패킷 인코딩·파싱, CRC, ACK 재전송 대기열 |
| `src/core/rx.c` | 수신 바이트 버퍼 |
| `src/drivers/` | `imu.c`(BMI270), `ppg.c`(MAXM86161), `charger.c`(BQ25186) |
| `src/platform.c` | 보드 핀·ADC·워치독 |
| `src/transport.c` | USB(UART)·BLE 송수신 |
| `include/` | 헤더 |
| `dts/bindings/` | 보드 제어 핀 정의(`sentiframe,control`) |
| `Kconfig`, `prj.conf` | 기능 설정, Zephyr 설정 |
| `profiles/six_axis.conf` | 자이로를 켜는 설정 |
| `config/features.json` | 기능 켜고 끄기 목록 (`tools/feature_config.py`가 읽는다) |
| `tests/` | 컴퓨터에서 돌리는 테스트 |
| `third_party/bmi270/` | Bosch BMI270 드라이버 (BSD-3, `LICENSE` 포함) |

## 기능 설정 (`Kconfig`)

| 설정 | 기본값 | 내용 |
|---|---|---|
| `SF_IMU` | 켜짐 | IMU 수집과 낙상 판정 |
| `SF_GYRO` | **꺼짐** | 자이로 수집. 전류를 아끼려고 기본은 꺼 둔다 |
| `SF_PPG` | 켜짐 | PPG 수집 |
| `SF_CHARGING` | 꺼짐 | 충전 |
| `SF_PACK_APPROVED` | 꺼짐 | 배터리 팩 승인 (승인 전에는 충전하지 않는다) |
| `SF_STREAM_DIVISOR` | 2 | IMU 원시 데이터를 100Hz ÷ N으로 보낸다 (2 → 50Hz, 4 → 25Hz). 낙상 판정은 항상 100Hz 전부를 쓴다 |

## 빌드

`CMakeLists.txt`가 이 폴더를 보드 정의 위치(`BOARD_ROOT`)로 쓴다. 보드 정의(`boards/`)와 빌드 도구(`tools/`)가 아직 저장소에 없어서, 정확한 빌드 명령은 그 파일들이 올라온 뒤 채운다.

채울 것: nRF Connect SDK 버전, 보드 이름, 빌드·플래시 명령, USB 시리얼 속도

## 테스트 (컴퓨터에서)

하드웨어 없이 돌아가는 C 테스트 3종이다. macOS의 `clang`으로 확인했다 (2026-11-08, 전부 통과). 이 폴더(`HW/firmware`)에서 실행한다.

```bash
# 낙상 판정·센서 드라이버·프로토콜 (시나리오 299개)
cc -std=c11 -O1 -w -Iinclude -Ithird_party/bmi270 tests/test_firmware.c \
   src/core/fall.c src/core/protocol.c src/drivers/imu.c src/drivers/ppg.c src/drivers/charger.c \
   third_party/bmi270/bmi2.c third_party/bmi270/bmi270.c -lm -o /tmp/test_firmware && /tmp/test_firmware

# 수신 버퍼 (시나리오 10개)
cc -std=c11 -O1 -w -Iinclude -Itests/rx_stubs tests/test_rx.c src/core/rx.c src/core/protocol.c \
   -lm -o /tmp/test_rx && /tmp/test_rx

# 앱 전체 — 실제 main.c 를 가짜 하드웨어로 돌린다. 기능 설정은 -D 로 넘긴다 (아래는 기본값)
cc -std=c11 -O1 -w -Iinclude -Itests/app_stubs -Ithird_party/bmi270 \
   -DCONFIG_SF_CHARGING=0 -DCONFIG_SF_IMU=1 -DCONFIG_SF_GYRO=0 -DCONFIG_SF_PPG=1 \
   -DCONFIG_SF_PACK_APPROVED=0 -DCONFIG_SF_STREAM_DIVISOR=2 \
   -Dmain=sf_application_main -c src/main.c -o /tmp/main_app.o
cc -std=c11 -O1 -w -Iinclude -Itests/app_stubs -Ithird_party/bmi270 \
   -DCONFIG_SF_CHARGING=0 -DCONFIG_SF_IMU=1 -DCONFIG_SF_GYRO=0 -DCONFIG_SF_PPG=1 \
   -DCONFIG_SF_PACK_APPROVED=0 -DCONFIG_SF_STREAM_DIVISOR=2 \
   tests/test_app.c src/core/protocol.c src/core/fall.c /tmp/main_app.o -lm -o /tmp/test_app
/tmp/test_app normal
```

`test_app`은 시나리오 이름을 받는다. 시나리오마다 맞는 설정이 있다.

| 시나리오 | 맞는 설정 |
|---|---|
| `normal`, `battery_cutoff`, `board_failure`, `rx_ble_gap`, `rx_usb_gap`, `transport_failure`, `watchdog_failure` | 모든 설정 |
| `imu_init_failure`, `imu_read_failure` | `SF_IMU=1` |
| `imu_shutdown_failure`, `imu_shutdown_once` | `SF_IMU=0` (IMU를 끈 상태에서 끄는 동작이 실패하는 경우) |
| `ppg_init_failure`, `ppg_read_failure`, `ppg_off_failure`, `ppg_off_stuck` | `SF_PPG=1` |

설정 24가지 × 맞는 시나리오, 모두 288번을 돌려 전부 통과했다. 파이썬 테스트 2개(`test_feature_config.py`, `test_reference.py`)는 `tools/`가 있어야 돌아간다.

## 낙상 판정 알고리즘

`src/core/fall.c`. 학습한 모델이 아니라 기준값으로 판정하는 규칙이다. "세게 부딪힌 뒤, 쓰러진 자세로 가만히 있다"는 낙상의 모양을 찾는다. IMU 샘플이 들어올 때마다(100Hz) 아래 순서로 처리한다.

0. **끊김 처리**: 샘플 사이가 0.1초 넘게 비면 상태를 처음부터 다시 시작한다. 데이터가 빠진 시간을 "가만히 있음"으로 오해하지 않기 위해서다
1. **최근 2초 기록**: 가속도 크기를 200샘플 동안 기억하고, 평균과 표준편차(최소 0.05g)를 구한다
2. **충격**: 가속도 크기 ≥ 3g, 또는 (최근 기록이 25샘플 이상이고) ≥ 2g 이면서 최근 2초 대비 z ≥ 6 → **충격 이벤트**. 이때의 "평소 중력 방향"을 기준으로 저장한다
3. **확인 (충격 뒤 최대 3초)**: 아래 두 조건이 함께 0.75초 이어지면 → **낙상 의심 이벤트**. 3초 안에 안 되면 낙상이 아닌 것으로 본다
   - 조용함: 가속도 0.7~1.3g(거의 중력만), 자이로가 켜져 있으면 회전 491 원시값(약 30°/s) 미만
   - 기울어짐: 지금 중력 방향이 기준과 45° 넘게 다르다 (cos < 0.7071)
4. **평소 중력 방향 갱신**: 조용할 때만 조금씩(이전 95% + 현재 5%) 갱신한다. 그래야 "충격 직전 자세"가 정확히 남는다

충격 이벤트가 나면 직전 2초 기록도 함께 보낸다(HISTORY). LED는 0.5초 켜진다. 진동 모터는 판정에 따라 켜지지 않는다.

**알려진 한계**

- 넘어진 뒤 바로 일어나면 "조용함"이 이어지지 않아 잡히지 않는다
- 알람은 충격 뒤 최소 0.75초가 걸린다
- 기준값(3g, 2g·z 6, 45°, 0.75초, 3초)을 고른 근거가 코드에 없다. 정해지면 여기에 적는다
- SW에서 이 규칙을 공개 데이터(ErciyesUni 머리 센서, 25Hz)로 평가했다. 재현율 0.38, 특이도 0.98이었다(SW 정리 PR #6의 `SW/CHANGES.md`). 25Hz 데이터는 짧은 충격의 최고값을 놓치므로, 안경(100Hz)으로 모은 데이터로 다시 확인해야 한다
