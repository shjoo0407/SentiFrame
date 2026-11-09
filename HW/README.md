# HW
낙상 감지 안경의 하드웨어 — 회로, 센서 배치, 기구 설계 자료

회로도 리비전 F, 엔지니어링 시제품이다. 회로도 표제란에 "공장 검토와 벤치 테스트 필요"라고 적혀 있다.

## 구성

| 파일 | 내용 |
|---|---|
| `SentiFrame.kicad_sch`, `SentiFrame.kicad_pcb` | **메인 보드** 회로도와 PCB |
| `Circuit_01~08.kicad_sch` | 메인 보드 회로도의 하위 시트 (아래 표) |
| `SentiFrame-PPG.kicad_pro`, `.kicad_sch`, `.kicad_pcb` | **PPG 보드**(MAXM86161) 프로젝트 |
| `Catalog_01~05.kicad_sch` | 부품 카탈로그 — 연결하지 않은 부품 모음이다 ("UNWIRED - NOT FOR FABRICATION"). 메인 회로도에는 들어가지 않는다 |
| `SentiFrame_FootprintCatalog.kicad_pcb` | 풋프린트 모음 |
| `libraries/` | 프로젝트 전용 기호(`SentiFrame.kicad_sym`), 풋프린트(`SentiFrame.pretty/`), 3D 모델(`3dmodels/`) |
| `sym-lib-table`, `fp-lib-table` | 위 라이브러리를 프로젝트 폴더 기준으로 등록한 목록 |
| `SentiFrame.csv` | 부품 목록(BOM). KiCad에서 뽑은 것이라 회로도를 고치면 다시 뽑는다 |
| `assets/` | 데이터시트·제조사 자료 링크, 커패시터 DC 바이어스 특성 → [assets/README.md](assets/README.md) |
| `firmware/` | 안경 펌웨어 → [firmware/README.md](firmware/README.md), 통신 형식 → [firmware/PROTOCOL.md](firmware/PROTOCOL.md) |

메인 보드 하위 시트

| 시트 | 내용 |
|---|---|
| 01 | MCU(nRF54L15)와 2.0V 인터페이스 |
| 02 | IMU와 광학 센서 — **파일이 아직 저장소에 없다** |
| 03 | USB-C와 절연 UART 레벨 변환 |
| 04 | 충전기와 시스템 2.0V 전원 |
| 05 | PPG 5V와 보조 3V 전원 |
| 06 | SOS 버튼, LED, 진동 모터 |
| 07 | 배터리 연결과 전압 측정 |
| 08 | USB 전원 종류 감지(BC1.2, Type-C) |

## KiCad로 열기

- 파일 형식 버전이 `20260206`이다. 이 형식을 읽을 수 있는 KiCad 버전이 필요하다
- 라이브러리와 3D 모델은 프로젝트 폴더 기준(`${KIPRJMOD}`)으로 찾으므로 따로 설정하지 않아도 된다
- 메인 보드의 프로젝트 파일(`SentiFrame.kicad_pro`)이 아직 없다. 올라오기 전까지는 `.kicad_sch`·`.kicad_pcb`를 직접 열어야 하고, 넷 클래스·설계 규칙 같은 프로젝트 설정이 빠진 상태다

## 아직 저장소에 없는 것

올리는 방법은 [TODO.md](TODO.md)에 있다.

| 빠진 것 | 영향 |
|---|---|
| `Circuit_02.kicad_sch` | 메인 회로도에서 IMU·광학 센서 시트가 비어 있다 |
| `SentiFrame.kicad_pro` | 메인 보드의 프로젝트 설정이 없다. BOM에 `${EXCLUDE_FROM_POS_FILES}`가 변수 그대로 찍힌 것도 이 때문으로 보인다 |
| `firmware/boards/` | 보드 정의가 없어 저장소만으로는 펌웨어를 빌드할 수 없다 |
| `firmware/tools/` | 라즈베리파이 수신 프로그램, 기능 설정 도구, 파이썬 테스트가 쓰는 코드 |
