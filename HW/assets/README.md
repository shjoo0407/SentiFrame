# 참고 자료

데이터시트와 제조사 CAD 자료는 저장소에 올리지 않고 링크로 남긴다.

- 공개 저장소라 재배포가 허락되지 않은 파일을 올리면 안 된다
- 저장소가 무거워진다 (PDF·CAD 원본만 약 98MB였다)

내 컴퓨터에는 `assets/datasheets/`, `assets/upstream/`에 그대로 두면 된다. `HW/.gitignore`에 등록돼 있어 올라가지 않고, 회로도의 `Local_Datasheet` 링크도 계속 동작한다.

## 데이터시트

개정판은 설계할 때 받은 PDF의 표지에 적힌 값이다. 표지에 없으면 PDF 파일 정보의 날짜를 적었다. 제조사가 새 개정판을 내면 내용이 바뀔 수 있으니, 새로 받을 때 비교해 본다.

| 부품 | 쓰는 곳 | 데이터시트 | 개정판 |
|---|---|---|---|
| AN54LV-15 (nRF54L15 모듈) | U1 | [Raytac](https://www.raytac.com/document/act.php?act=1&index_id=101) | 문서 날짜 2026-07-08 |
| BMI270 (IMU) | U2 | [Bosch](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf) | Rev 1.6 |
| MAXM86161A (PPG) | U3 | [Analog Devices](https://www.analog.com/media/en/technical-documentation/data-sheets/maxm86161a.pdf) | Rev 1 (2026-01) |
| MAXM86161A 랜드 패턴 | U3 | MAXM86161A 데이터시트 페이지의 패키지 정보 (도면 번호 90-100106) | Rev A |
| CP2102N (USB-UART) | U4 | [Silicon Labs](https://www.silabs.com/documents/public/data-sheets/cp2102n-datasheet.pdf) | Rev 1.5 |
| TXU0202 | U5 | [TI](https://www.ti.com/lit/ds/symlink/txu0202.pdf) | 2022-03 |
| TPD2EUSB30 | U6 | [TI](https://www.ti.com/lit/ds/symlink/tpd2eusb30.pdf) | 2021-06 |
| BQ25186 (충전) | U7 | [TI](https://www.ti.com/lit/ds/symlink/bq25186.pdf) | 2025-01 |
| TPS62840 | U8 | [TI](https://www.ti.com/lit/ds/symlink/tps62840.pdf) | 2020-03 |
| TPS63900 | U9 | [TI](https://www.ti.com/lit/ds/symlink/tps63900.pdf) | 2020-10 |
| TPS7A02 | U10 | [TI](https://www.ti.com/lit/ds/symlink/tps7a02.pdf) | 2022-09 |
| TPS22918 | U11, U12 | [TI](https://www.ti.com/lit/ds/symlink/tps22918.pdf) | 2017-07 |
| BQ24392 | U13 | [TI](https://www.ti.com/lit/ds/symlink/bq24392.pdf) | SLIS146G (2017-09) |
| TUSB320LAI | U14 | [TI](https://www.ti.com/lit/ds/symlink/tusb320lai.pdf) | 2017-05 |
| TPD1E10B06 | D1~D3 | [TI](https://www.ti.com/lit/ds/symlink/tpd1e10b06.pdf) | 2024-08 |
| APHHS1005SURCK (LED) | D5 | [Kingbright](https://www.kingbrightusa.com/images/catalog/spec/aphhs1005surck.pdf) | Ver.18A |
| USB4105 (USB-C) | J1 | [GCT](https://gct.co/files/drawings/usb4105.pdf) | — |
| DFE201612E (인덕터) | L2, L3 | Murata 웹사이트에서 `DFE201612E` 검색 | 2024-11 |
| 303-103 (진동 모터) | M1 | [Precision Microdrives](https://precisionmicrodrives.com/cdn/catalog_product/a43fe981-4a1e-4aa2-bfc0-005ce2c3eb36/303-103-datasheet.pdf) | 2025-05 |
| DMN1019USN | Q1~Q5 | [Diodes](https://www.diodes.com/assets/Datasheets/DMN1019USN.pdf) | Rev 2 |
| SEMITEC 103AT (서미스터) | TH1 | [SEMITEC](https://www.semitec-global.com/uploads/2022/01/P12-13-AT-Thermistor.pdf) | 2017-03 |
| SN74LVC2G125 | 회로도 Circuit_03 | [TI](https://www.ti.com/lit/ds/symlink/sn74lvc2g125.pdf) | 2017-03 |
| TPS22916 | 회로도에 없음 (검토용) | [TI](https://www.ti.com/lit/ds/symlink/tps22916.pdf) | Rev H (2021-12) |
| TPS2553 | 회로도에 없음 (검토용) | [TI](https://www.ti.com/lit/ds/symlink/tps2553.pdf) | SLVS841F (2016-08) |
| TPS25947 | 회로도에 없음 (검토용) | [TI](https://www.ti.com/lit/ds/symlink/tps25947.pdf) | 2026-05 |
| TPS3700 | 회로도에 없음 (검토용) | [TI](https://www.ti.com/lit/ds/symlink/tps3700.pdf) | 2019-02 |
| TPS3839 | 회로도에 없음 (검토용) | [TI](https://www.ti.com/lit/ds/symlink/tps3839.pdf) | 2015-07 |

저항(Yageo)·커패시터(TDK)·커넥터(JST)·스위치(Omron)는 `SentiFrame.csv`(BOM)의 Datasheet 열에 부품 번호별 링크가 있다. 커패시터의 DC 바이어스 특성은 TDK 링크의 부품 페이지에서 받을 수 있고, 받은 값은 `pcba-passives/*-bias.csv`에 있다.

## 제조사 CAD 자료

- **AN54LV-15 풋프린트·3D 모델**: Raytac에서 받은 `AN54LV-15 Footprint_Design Guide` 패키지(2026-09-03판). 이 프로젝트에 필요한 3D 모델은 `libraries/3dmodels/AN54LV-15_manufacturer.step`에 이미 들어 있다
- **KiCad 기본 라이브러리**(Battery_Management, Interface_USB, Regulator_Switching, Sensor_Motion, Sensor_Optical): KiCad를 설치하면 함께 들어 있다. 원본은 [kicad-symbols](https://gitlab.com/kicad/libraries/kicad-symbols). 이 프로젝트는 필요한 기호를 `libraries/SentiFrame.kicad_sym`으로 옮겨 쓴다
