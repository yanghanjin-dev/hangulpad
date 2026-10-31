# 한글패드 / HangulPad

가볍고 단순한 한글 워드 프로세서를 목표로 하는 오픈소스 프로젝트입니다.  
C11 문서 엔진과 Windows Win32 문서 보기·편집 UI를 개발하고 있습니다. **HWP 5.x, HWPX, HWPML(한/글 XML)을 엽니다.**

A lightweight Korean word processor with a C11 HWP/HWPX engine and a Windows viewer/editor.

> **기본 개발 환경:** Windows PowerShell / MSYS2 UCRT64 / CMake / Ninja.

**현재 버전: 0.14.0** (2026-10-31) — 변경 내역은 [`CHANGELOG.md`](CHANGELOG.md)

0.14.0은 **여러 단계 되돌리기·다시 실행**(Ctrl+Z / Ctrl+Shift+Z, 텍스트·문단 추가/삭제·서식·모두 바꾸기, 최대 200단계)을 추가했고, 0.13.1에서 한/글이 HWP로 저장한 문서의 셀을 HWPX 원본과 같은 높이로 다시 배치합니다(시험 27개 모두 통과).

0.13.0은 **HWPML(한/글 XML, 국가법령정보센터 법령 파일 등)**을 열고, 미주를 문서 끝에, 머리말·꼬리말·각주 안의 표·그림과 위첨자·아래첨자를 표시합니다.

0.12.0은 **머리말·꼬리말**(쪽 번호 필드, 홀짝 쪽 구별), **각주**(본문 번호와 쪽 아래 구분선·내용), **문단 번호·글머리표·개요 번호**를 표시합니다.

0.11.2는 편집·서식 저장 때 탭(너비·채움 점선)과 전각 공백을 보존하고, HWP 변환본에 각주/미주 모양·쪽 테두리·표 셀 영역·대체 글꼴·글꼴 종류(HFT)를 넣습니다.

0.11.0은 **문단 안 일부 글자에 글자 모양**을 주고, 쪽 보기에서 **더블클릭한 자리에서 편집**을 시작합니다(편집 중 쪽 클릭으로 커서 이동, 쪽 위에 커서·선택 표시).

0.10.2는 HWP를 HWPX로 변환할 때 탭·문단 번호·글머리표·스타일 정의와 한컴 패키지 구성(settings·manifest·미리보기)을 함께 저장합니다.

0.10.1은 한컴오피스가 HWP로 저장한 문서(셀 줄 배치가 비어 있는 파일)도 HWPX 원본과 같은 쪽 수로 배치합니다.

0.10.0은 **한글 2010 방식의 메뉴·도구 상자·상황선**과 **글자 모양·문단 모양**, 찾기/바꾸기, 화면 확대·두 쪽 보기,
인쇄, 문서 정보, 최근 문서를 추가했습니다. 아직 없는 기능은 메뉴에 흐리게 표시합니다. [메뉴와 기능 구현 내역](docs/menus-v0100-2026-10-30.md).

0.9.1은 고해상도(화면 배율 125%·150% 등) 화면에서 문서와 대화상자를 선명하게 표시합니다.

0.9.0은 **HWP 5.x 문서를 엽니다.** HWP 레코드를 같은 내용의 HWPX 부품으로 바꾸어 기존 HWPX 리더와
화면 배치를 그대로 사용합니다. 배포용 문서는 읽기 전용으로 열리고, 암호 문서는 열지 않습니다.
HWP 문서를 저장하면 원본은 그대로 두고 새 `.hwpx` 파일로 저장합니다. [HWP 읽기 구현과 검증](docs/hwp-reader-v090-2026-10-29.md).

0.8.4는 PDF 41쪽 대조를 바탕으로 투명 그림, 목차 탭, 상자 번호, 표 앵커·셀 여백·테두리를 개선했습니다. [전체 대조와 남은 차이](docs/pdf-fidelity-v084-2026-10-27.md).

0.8.3은 공백까지 이어지는 연속 밑줄과 셀별 점선 테두리를 표시합니다. [밑줄·점선 검증](docs/decorations-v083-2026-10-26.md).

0.8.2는 인라인 표에 원본 문단의 가로 정렬과 바깥 여백을 적용합니다. [표 정렬 검증](docs/table-align-v082-2026-10-26.md).

0.8.1은 반각 공백·글자별 자간·합성 굵기 폭과 내어쓰기·양쪽 정렬을 보정합니다. [간격 수정 및 검증](docs/spacing-v081-2026-10-26.md).

## 본문 편집 (0.8.0)

새 문서 또는 기존 HWPX에서 **본문 편집**(F2, 도구 상자 "편")을 누르면 왼쪽 문단 목록과 오른쪽 입력창이 나타납니다.
본문·표 셀·중첩 표 안 문단을 선택하여 텍스트를 수정할 수 있습니다. 선택·복사·잘라내기·붙여넣기는
Windows 기본 입력창을 사용하며 한글 IME를 지원합니다. 되돌리기(`Ctrl+Z`)·다시 실행(`Ctrl+Shift+Z`)은 문서 전체의 편집 기록을 따릅니다(0.14.0).

- `Alt+N`/`Ctrl+N`: 새 문서, `Alt+O`/`Ctrl+O`: 불러오기, `Alt+S`/`Ctrl+S`: 저장, `Alt+V`: 다른 이름으로 저장.
- `Enter`: 문단 안 줄바꿈, `Ctrl+Enter`: 선택 문단 뒤에 새 문단 추가. 편집 메뉴에서 문단 삭제.
- `F2`: 본문 편집 ↔ 쪽 보기, `F5`: 미리 보기. 편집 중에는 입력창 아래에 쪽 미리 보기가 함께 표시됩니다.
- `Alt+L` 글자 모양, `Alt+T` 문단 모양, `Ctrl+B/I/U`, `Ctrl+Shift+M/L/C/R/T`(정렬) — 문단 목록에서 여러 문단을 골라 한 번에 적용.
- `Ctrl+F` 찾기, `Ctrl+H` 찾아 바꾸기, `Alt+G` 찾아가기, `Alt+P` 인쇄. 전체 목록: 도움말 → 단축키 일람.
- 제목의 `*`는 저장하지 않은 변경입니다. 다른 문서를 열거나 종료할 때 저장 여부를 확인합니다.

저장은 원본 XML의 텍스트 범위만 교체하며 그림·표·구역 설정과 다른 패키지 부품을 보존합니다.
바뀐 구간은 해당 run의 서식을 사용하고, 새 문단은 기준 문단의 문단/글자 스타일을 상속합니다.
수정한 문단의 오래된 줄 위치 정보는 제거하여 다시 배치합니다. 엔진 API는 `include/hp/edit.h`입니다.

현재 편집은 **문단별 텍스트 편집과 글자·문단 서식**입니다(입력은 편집 입력창, 위치는 쪽 위에서 클릭으로 고름). 쪽 위에 바로 입력하는 WYSIWYG,
개체 삽입·삭제·이동, 구역 구성 변경은 지원하지 않습니다. 개체·구역 제어를 포함한 문단과 영역의
마지막 문단 삭제는 거부합니다. 되돌리기는 텍스트 입력, 문단 추가·삭제, 글자·문단 모양, 모두 바꾸기를
한 단계씩 되돌립니다(입력은 다른 문단으로 옮기거나 입력을 멈춰 미리 보기가 갱신될 때 한 단계가 됨). 편집하지 않은 기존 본문은 원본을 유지하지만 수정 후 한컴과 동일한 페이지 수나
줄바꿈을 보장하지 않습니다. ZIP 내부의 기존 미리보기 이미지는 갱신하지 않습니다.

## Status

| Area | Status |
|------|--------|
| `hp` IR (`include/hp/ir.h`) | paragraphs/runs/sections, tables (nested), images, containers, equations, saved line layout |
| XML parser (`src/xml.c`) | Own dependency-free DOM parser: entities, char refs, CDATA, namespaces (local names), byte spans for lossless patching, depth limit 512, fuzzed |
| HWPX read/write | Read: paragraphs/styles/tables/images/floats/equations. **Save is lossless** for opened files — every original part is kept byte-for-byte; only changed parts are regenerated (appended paragraphs are inserted into the original section XML, title is patched in `content.hpf`). New documents get a minimal package. |
| HWP 5.x | **Read** (`src/cfb.c` compound file, `src/hwp.c` records → OWPML parts → HWPX reader; distribution-only docs read-only; password/DRM refused). Write: no — HWP documents are saved as HWPX |
| HWPML 2.x | **Read** (`src/hwpml.c`: 한/글 XML documents such as 국가법령정보센터 downloads named .hwp → OWPML parts → HWPX reader). Saved as HWPX |
| CLI (`build/hp`) | `open` / `new` / `save` (also `.hwp` → `.hwpx`) / `append` / `roundtrip` / `probe` / `--version` (UTF-8/Korean paths on Windows) |
| Desktop UI (Windows) | `HangulPad.exe` engine layout (fonts, tables/nested, images, floats, containers, equations); 0.10.0: classic 한글 menus, two toolbars, status bar, zoom/fit/two-page view, character/paragraph format dialogs, find/replace, print |
| Full HWP record parse / HWP write | Deferred |

GUI 0.7.1은 명시적 페이지/단 나눔·구역별 용지 설정과 긴 표/문단의 페이지 분할을 지원합니다.
구역별 시작 번호·이어매기기와 새 번호 시작·쪽번호 감추기를 반영합니다. 상단의 물리적 페이지 이동 번호와 문서의 인쇄 쪽번호를 구분합니다.
문자 종류별 원본 글꼴을 유지하고 누락 문자에 대체 글꼴을 적용하며, 도움말의 대체 글꼴 내역에서 적용 결과를 확인할 수 있습니다.
셀 정렬·여백 상속, 그림 잘라내기와 저장 기준선·세로 위치도 반영합니다. [0.7.0 서식·배치 검증](docs/font-layout-update-2026-10-26.md).
원본 글꼴이 설치되어 있으면 우선 사용하며 언어별 장평·자간, 셀 안 정렬·줄바꿈을 반영합니다.
반복 설정과 제목 셀 표시가 있는 표는 다음 페이지에 머리행을 반복합니다. 병합 셀과 중첩 표의
실제 글자 영역을 함께 검사해 가능한 경우 글자 사이에서 분할하고, 잘린 셀의 경계선을 표시합니다.
페이지 수와 화면이 한컴 출력과 완전히 같다는 의미는 아닙니다. 머리행과 본문을 가로지르는 병합,
페이지보다 큰 개체, 중첩 표 자체의 머리행 반복 및 탭/양쪽 정렬/부동 개체는 추가 개선 대상입니다.
현재 3종 샘플은 원본 PDF와 같은 17/20/4쪽으로 배치되지만, 글꼴·기호·상대 크기·세부 배치 차이는 남아 있습니다.
검증 결과: [0.6.0 페이지 수 개선 보고서](docs/layout-v060-2026-10-26.md),
[0.5.0 표 분할 보고서](docs/table-pagination-v050-2026-10-26.md),
[0.4.0 레이아웃 보고서](docs/layout-v040-2026-10-26.md).

Default save format: **HWPX**.

## Layout

```
include/hp/   Public headers (ir, hwpx, hwp, common)
src/          Engine + CLI sources (xml.c = XML parser, hwpx_read.c / hwpx_write.c, gui_win.c)
tests/        Unit tests, fixtures, golden IR dumps (tests/golden), fuzzers (tests/fuzz)
CMakeLists.txt  Windows build / CTest / ZIP packaging
```

## Build & test — Windows (기본)

WSL 없이 Windows PowerShell에서 CLI·GUI·C 테스트를 빌드합니다.
기본 도구는 MSYS2 **UCRT64 GCC + CMake + Ninja + libzip**입니다.
MSVC는 현재 검증 대상이 아닙니다.

### 최초 준비

1. https://www.msys2.org/ 에서 MSYS2를 설치합니다 (기본 경로 `C:\msys64`).
2. MSYS2 UCRT64 창에서 다음을 실행합니다. 업데이트가 창 종료를 요청하면 다시 열어 진행합니다.

```sh
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-libzip
```

### PowerShell에서 개발

소스 폴더 (`CMakeLists.txt`가 있는 폴더)에서 실행합니다.

```powershell
# 빌드 + XML/왕복/샘플 golden 회귀 검사
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
# 주요 페이지 이미지 회귀 검사 (Python + Pillow 필요)
.\scripts\build-windows.ps1 -Action VisualTest -PythonExe python
# 빌드만
.\scripts\build-windows.ps1 -Action Build
# 테스트 통과 후 EXE·DLL·샘플·라이선스를 ZIP으로 패키징
.\scripts\build-windows.ps1 -Action Package
# 다른 MSYS2 설치 위치 / 디버그 구성
.\scripts\build-windows.ps1 -ToolchainRoot D:\msys64\ucrt64 -Configuration Debug
```

실행 정책에 의해 `.ps1` 직접 실행이 제한되면 첫 명령과 동일하게
`powershell -NoProfile -ExecutionPolicy Bypass -File` 뒤에 스크립트와 인수를 전달하세요.
시스템 실행 정책은 변경하지 않습니다.

결과: `build/windows-release/hp.exe`, `build/windows-release/HangulPad.exe`.
로컬 실행에는 UCRT64 `bin`이 PATH에 있어야 합니다. GUI는 파일 메뉴로 샘플을 엽니다.
배포 ZIP은 `dist/`에 생성되며 의존 DLL과 `documents/`가 함께 들어갑니다.
스크립트는 필요한 동안만 PATH를 설정하고 종료 시 복원합니다.

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build\windows-release\hp.exe --version
.\build\windows-release\HangulPad.exe
.\build\windows-release\hp.exe new .\demo.hwpx --text "한글패드" --title "샘플"
.\build\windows-release\hp.exe append .\demo.hwpx .\추가.hwpx --text "추가 문단"
```

테스트 임시 파일은 `build/windows-release/test-work/`에 둡니다 (테스트 입력은 `ctest` 실행마다 원본에서 새로 복사).
Golden 검사는 원본 `tests/golden/`을 직접 읽어 전체 IR을 비교합니다.

의도적인 리더 변경 시 golden 갱신 (LF로 기록되어 Linux와 바이트 동일):

```powershell
.\scripts\build-windows.ps1 -Action GoldenUpdate   # 그 다음 tests\golden 변경 내용을 검토
```

> PowerShell의 `irdump.exe ... > 파일` 리디렉션은 쓰지 마세요 (Windows PowerShell 5.1은 UTF-16으로 저장해 golden이 깨집니다).

**Synology Drive / OneDrive 폴더에서 개발할 때:** 빌드 폴더(`build/`, `dist/`)는 동기화에서 제외하거나
`-BuildRoot "$env:LOCALAPPDATA\hangulpad-build"` 처럼 동기화 밖에 두세요. 빌드 중 동기화 프로그램이
파일을 잠가 빌드·테스트가 실패하거나, 수천 개의 빌드 파일이 NAS로 올라갈 수 있습니다.

**메모리 검사:** MinGW에는 AddressSanitizer가 없어 Windows 빌드로는 메모리 오류·누수를 검사할 수 없습니다.
CI의 Linux 작업이 ASan/UBSan 검사를 맡습니다 (로컬: Linux/WSL에서 `make all CFLAGS=... -fsanitize=address,undefined`).

### Linux / 기존 크로스 빌드 (보조)

기존 GNU Make 명령은 `Makefile.posix`에 보존돼 있습니다. Linux에서는
`make all`, `make golden-update`, `make fuzz`를 계속 사용할 수 있습니다.
Windows 기본 경로는 PowerShell/CMake이며 GNU Make 설치는 필요 없습니다.
퍼징은 기존 clang/libFuzzer POSIX 경로로 유지합니다.

Saving never deletes the target first: libzip writes a temporary file and replaces
the target only after a successful write, and a read-only target is refused.

For opened documents, saving existing-content edits (`needs_rewrite`), removing
loaded paragraphs, or changing the section mapping returns `HP_ERR_UNSUPPORTED`.
If appending cannot preserve the original section XML, saving also fails instead
of regenerating that section as plain text. The existing destination is retained.
Arbitrary direct IR mutations remain unsupported. Use hp_editor_* in hp/edit.h for source-preserving paragraph text edits, insertion and deletion.

## 버전 규칙

버전은 [유의적 버전(SemVer)](https://semver.org/lang/ko/) `MAJOR.MINOR.PATCH` 를 따르고,
번호는 **`include/hp/version.h` 한 곳에서만** 정합니다 (Makefile·CLI·GUI·exe 속성·
새 문서의 `version.xml`·포터블 패키지 이름이 모두 여기서 가져갑니다).

| 자리 | 올리는 경우 |
|---|---|
| MAJOR | 1.0 = 편집 화면 + HWP 읽기 완성. 그 뒤로는 호환이 깨지는 변경 |
| MINOR | 기능 추가, 파일 형식 동작 변경 (예: 원본 보존 저장) |
| PATCH | 버그 수정만 |

개발할 때마다:
1. `include/hp/version.h` 의 `HP_VERSION_MAJOR/MINOR/PATCH`, `HP_VERSION_STRING`,
   `HP_VERSION_COMMAS`, `HP_VERSION_DATE` 를 함께 올립니다 (`CTest`가 일치 여부를 검사).
2. `CHANGELOG.md` 맨 위에 새 버전 항목(추가/수정/변경)을 씁니다.
3. 빌드·테스트 후 Windows 포터블을 다시 만듭니다. 확인: `hp.exe --version`, ZIP 파일 이름.

## License

MIT OR Apache-2.0 — see [`LICENSE`](LICENSE), [`LICENSE-MIT`](LICENSE-MIT), [`LICENSE-APACHE`](LICENSE-APACHE).

## Attribution

See [`NOTICE`](NOTICE) for the Hancom HWP / OWPML attribution placeholder required when referencing Hancom's public format documentation.

## Not yet

- Binary HWP write (HWP documents are saved as HWPX)
- Typing directly on the page (0.11.0: click on the page to place the caret, type in the edit box; formats apply to the selected characters)
- Styles, numbering/bullets, page setup editing, table/picture editing (menus are present, greyed; 0.12.0 shows numbers/bullets, editing them is not yet possible)




