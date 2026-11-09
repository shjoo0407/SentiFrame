# HW 남은 일

HW 멘티에게 부탁하는 일이다. 끝낸 항목은 이 문서에서 지우고, 다 끝나면 이 파일을 지운다.

## 1. 빠진 파일 올리기

| 파일 | 왜 필요한가 |
|---|---|
| `HW/Circuit_02.kicad_sch` | 메인 회로도의 "02 IMU and optical sensor" 시트다. 최상위 회로도가 이 파일을 부르는데 없어서, 열면 IMU·PPG 시트가 비어 있다 |
| `HW/SentiFrame.kicad_pro` | 메인 보드 프로젝트 파일이다. 없으면 넷 클래스·설계 규칙 같은 설정이 빠진다 |
| `HW/firmware/boards/` | 보드 정의다. `CMakeLists.txt`가 이 폴더를 보드 위치로 쓰므로, 없으면 펌웨어를 빌드할 수 없다 |
| `HW/firmware/tools/` | 라즈베리파이 수신 프로그램(`pi_receiver.py`), `feature_config.py` 등이다. 파이썬 테스트 2개가 이 폴더를 불러온다 |

`boards/`와 `tools/`는 `.gitignore`가 빼고 있어서 올라가지 않았다. PR #7에서 고쳤으므로 **PR #7이 merge된 뒤에** 올린다. 그 전에는 `git add`가 되지 않는다. 회로도 시트와 프로젝트 파일은 커밋할 때 빠진 것으로 보인다.

### 올리는 방법

SentiFrame 폴더에서 차례로 실행한다.

```bash
# 1. 참고 자료 백업 — main 으로 바꾸면 git 이 datasheets·upstream·TDK PDF 를 컴퓨터에서 지운다
mkdir -p ~/Desktop/sentiframe-backup
cp -R HW/assets/datasheets HW/assets/upstream HW/assets/pcba-passives/TDK_MLCC_specification.pdf ~/Desktop/sentiframe-backup/

# 2. 최신 main 으로 바꾸고, 백업한 자료를 다시 넣는다 (이제 .gitignore 로 올라가지 않는다)
git switch main && git pull
cp -R ~/Desktop/sentiframe-backup/datasheets ~/Desktop/sentiframe-backup/upstream HW/assets/
cp ~/Desktop/sentiframe-backup/TDK_MLCC_specification.pdf HW/assets/pcba-passives/

# 3. 다 쓴 hw 브랜치를 지운다 — 남아 있으면 hw/... 이름의 브랜치를 만들 수 없다
git branch -D hw

# 4. 빠진 파일을 올린다
git switch -c hw/missing-files
git add HW/Circuit_02.kicad_sch HW/SentiFrame.kicad_pro HW/firmware/boards HW/firmware/tools
git status   # 빌드 결과물이나 데이터시트가 섞이지 않았는지 확인한다
git commit -m "HW: 빠진 회로도 시트, 프로젝트 파일, 보드 정의, 도구 추가"
git push -u origin hw/missing-files
```

- 1·2단계에서 파일이 없다고 나오면 이미 지워진 것이다. 넘어가도 된다. 자료는 [assets/README.md](assets/README.md)의 링크에서 다시 받을 수 있다
- 그다음 GitHub에서 PR을 연다. 제목은 `[HW] - 빠진 파일 추가`

## 2. 문서 확인하고 채우기

위 PR에서 함께 한다.

- [README.md](README.md), [firmware/README.md](firmware/README.md), [firmware/PROTOCOL.md](firmware/PROTOCOL.md)에서 코드·설계와 다른 곳이 있으면 고친다. 특히 `PROTOCOL.md`는 수신 프로그램을 만들 때 기준이 되므로 꼼꼼히 본다
- 아래 내용을 해당 문서에 직접 적는다

| 내용 | 적을 곳 |
|---|---|
| nRF Connect SDK 버전, 보드 이름, 빌드·플래시 명령, USB 시리얼 속도(baud rate) | [firmware/README.md](firmware/README.md)의 "빌드" — "채울 것" 줄을 지우고 그 자리에 |
| 쓰는 KiCad 버전 | [README.md](README.md)의 "KiCad로 열기" |
| 낙상 판정 기준값(3g, 2g·z 6, 45°, 0.75초, 3초)을 정한 근거 | [firmware/README.md](firmware/README.md)의 "알려진 한계" — 기준값 줄을 근거로 바꾼다. 근거가 없으면 없다고 적는다 |
