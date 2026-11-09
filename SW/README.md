# SW
낙상 감지 소프트웨어 — 데이터 전처리, 낙상 탐지 모델, 추론 코드

이번 정리에서 무엇을 왜 바꿨는지는 [CHANGES.md](CHANGES.md)에 있다.

## 폴더

| 폴더 | 내용 |
|---|---|
| [`fall_detection/`](fall_detection/) | 머리 IMU 낙상 탐지 — 데이터, 모델, 실시간 평가, 지연 측정 |
| [`ppg/`](ppg/) | PPG 심박 처리 (가상 센서로 만든 첫 버전) |
| [`archive/`](archive/) | 이전 실험 스크립트. 결과를 믿으면 안 된다 — 이유는 archive/README.md |

## fall_detection 파일

| 파일 | 하는 일 |
|---|---|
| `data.py` | 머리 센서(`340506`) 기록 읽기, 라벨(9xx = 낙상), 단위 변환(g, °/s), 참가자 단위 분할 |
| `models.py` | 여섯 모델: Robust Z, Moving Z, IQR, 로지스틱 회귀, 오토인코더, LightGBM |
| `streaming.py` | 실시간 판정 흉내 — 창 만들기, 학습 창 라벨, 기록 단위 재현율·특이도·탐지 지연 |
| `firmware_rule.py` | 안경 펌웨어의 규칙 기반 판정(`fall.c`)을 옮긴 기준선 |
| `benchmark.py` | 판정 1회 지연 측정 (창을 한 번에 하나씩) |
| `select_window.py` | 창 길이·판정 간격 고르기 (dev 참가자 교차검증) |
| `run_streaming.py` | 최종 비교 — 테스트는 여기서 한 번만 쓴다 |
| `compare_models.py` | 이전 방식(기록 전체를 한 번에 넣는 평가). 비교용으로 남겨 둔다 |

## 실행

데이터는 저장소에 올리지 않는다. UCI에서 받은 zip 안의 `Tests.rar`를 풀고, 그 안의 참가자별 rar(`101.rar`~`209.rar`)도 모두 풀어 한 폴더에 모은다. 그 폴더(참가자 폴더 `101`~`209`가 들어 있는 곳)를 저장소 밖에 두고 경로를 넘긴다.

```bash
pip install -r requirements.txt
cd fall_detection
python select_window.py --data-folder /경로/Tests   # 약 7분. results/window_selection/ 에 저장
python run_streaming.py --data-folder /경로/Tests   # 약 2분. results/streaming/ 에 저장
```

`results/`는 `.gitignore`에 들어 있어 올라가지 않는다.

데이터 출처: [UCI — Simulated Falls and Daily Living Activities](https://archive.ics.uci.edu/dataset/455/simulated+falls+and+daily+living+activities+data+set), 센서 번호·활동 코드는 같은 페이지의 Information.pdf
