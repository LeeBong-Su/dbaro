# DBaro (디비바로)

DB를 바로 다루는 크로스플랫폼 DB 접속 툴.
Oracle, MariaDB, MySQL, PostgreSQL, Tibero를 지원한다.

## 폴더 구조

```
dbaro/
├─ app/        Qt 6 (C++) 데스크톱 앱: 에디터, 결과 그리드, 객체 탐색기, 파서·자동완성
├─ engine/     Kotlin/JVM 드라이버 호스트: JDBC 연결, 실행·취소, 메타데이터 조회
├─ protocol/   앱과 엔진 사이 통신 규약
├─ dev/        개발·테스트용 DB (Docker Compose)
├─ bench/      성능 기준과 측정
├─ packaging/  배포본에 넣는 안내문, 제3자 라이선스 고지
├─ scripts/    빌드·배포 스크립트
└─ docs/       설계 문서
```

## 개발 환경

| 도구 | 버전 | 용도 |
|---|---|---|
| Qt | 6.8 이상 (개발 PC는 6.10.3) | 앱 |
| CMake | 3.21 이상 | 앱 빌드 |
| C++ 컴파일러 | C++20 지원 (MSVC 2022, Clang, GCC) | 앱 빌드 |
| JDK | 21 | 엔진 (Gradle은 Wrapper가 받아 쓴다) |
| Docker 호환 도구 | Compose v2 | 테스트용 DB |

Windows 설치 방법은 [docs/dev-setup-windows.md](docs/dev-setup-windows.md)에 있다.
빌드 방법은 `app/README.md`, `engine/README.md`에 있다.

## 배포본 만들기 (Windows)

```powershell
.\scripts\package-windows.ps1            # 엔진 테스트까지 하려면 -RunTests
```

`dist\DBaro-<버전>-windows-x64\` 폴더와 같은 이름의 ZIP이 만들어진다. 사용자 PC에는 Java나 Qt를 설치할 필요가 없다.

| 들어가는 것 | 내용 |
|---|---|
| `dbaro.exe`, `dbaro-cli.exe`, `Qt6*.dll`, 플러그인 폴더 | 앱, 명령줄 도구, Qt (`windeployqt`로 수집) |
| `vcruntime140*.dll`, `msvcp140*.dll` 등 | MSVC 런타임 (앱 옆에 두는 방식) |
| `engine\lib\*.jar` | 엔진 |
| `runtime\` | `jlink`로 만든 Java 21 런타임 (JDBC·TLS·한국어 문자셋에 필요한 모듈만) |
| `README.txt`, `THIRD_PARTY_NOTICES.md`, `BUILD-INFO.txt` | 안내문, 제3자 라이선스 고지, 빌드에 쓴 버전 |

스크립트는 마지막에 배포본의 Java로 엔진을 띄워 응답하는지 확인한다. JDBC 드라이버는 넣지 않는다 (첫 연결 때 내려받거나 사용자가 지정).

### MSI 설치 프로그램

```powershell
.\scripts\build-msi-windows.ps1            # 배포 폴더까지 새로 만든다. 이미 있으면 -SkipPackage
```

`dist\DBaro-<버전>-windows-x64.msi`가 만들어진다 (WiX v5, `packaging\windows\DBaro.wxs`).

- 더블클릭하면 관리자 권한 없이 사용자별로 설치된다 (`%LOCALAPPDATA%\Programs\DBaro`). 시작 메뉴와 바탕 화면에 바로 가기를 만든다.
- 설치 화면은 한국어이고, 완료 화면에 설치 위치와 만든 바로 가기를 보여 준다. "DBaro 실행"을 체크한 채 마치면 바로 실행한다.
- 전체 사용자용: `msiexec /i DBaro-<버전>-windows-x64.msi ALLUSERS=1` (관리자, `Program Files`에 설치)
- 바탕 화면 바로 가기 없이: `msiexec /i DBaro-<버전>-windows-x64.msi DESKTOPSHORTCUT=0`
- 화면 글은 `packaging\windows\DBaro.ko-KR.wxl`, 화면 그림은 `packaging\windows\installer-*.png`(빌드할 때 BMP로 바꾼다)에 있다.
- 새 버전 MSI를 설치하면 이전 버전을 대신한다. 제거해도 사용자 데이터(연결 정보, 비밀번호, 설정)는 남는다.
- WiX는 v5를 쓴다. v6 이상은 매출 기준 유지보수 비용과 EULA 동의 조건이 있다.

### 코드 서명

스마트 앱 제어가 켜진 Windows 11은 Microsoft 신뢰 루트 프로그램에 속한 인증 기관의 인증서로 서명하지 않은 exe를 실행하지 못하게 막는다.
배포본에서 서명이 없는 파일은 `dbaro.exe`, `dbaro-cli.exe`뿐이다 (Qt, Java 런타임, MSVC 런타임은 이미 서명돼 있다).

서명 설정은 저장소에 두지 않고 환경 변수로 준다. 설정하면 두 스크립트가 exe와 MSI에 서명하고, 배포본의 모든 exe·dll이 유효하게 서명됐는지 확인한다.
설정하지 않으면 서명 없이 만들고 경고만 한다.

```powershell
$env:DBARO_SIGN_ARGS = '/sha1 <인증서 지문>'   # Windows 인증서 저장소(하드웨어 토큰 포함)의 코드 서명 인증서
.\scripts\build-msi-windows.ps1
```

- Azure Artifact Signing은 `/dlib <경로>\Azure.CodeSigning.Dlib.dll /dmdf <경로>\metadata.json`을 넣고 `DBARO_SIGN_TIMESTAMP`를 `http://timestamp.acs.microsoft.com`으로 바꾼다.
- 타임스탬프 서버 기본값은 `http://timestamp.digicert.com`이다 (서명할 때 파일 해시를 보낸다).
- 자체 서명 인증서는 스마트 앱 제어가 받아 주지 않는다. 스크립트도 신뢰할 수 없는 서명이면 실패한다. 자세한 내용은 `scripts\sign-windows.ps1` 머리말에 있다.

## 문서

- [아키텍처](docs/architecture.md)
- [통신 규약](protocol/README.md)
- [성능 기준](bench/README.md)
