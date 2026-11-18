# app — Qt 6 데스크톱 앱 (C++)

사용자가 보는 화면 전체와, 키 입력마다 반응해야 하는 기능을 맡는다.
DB 연결은 직접 하지 않고 엔진 프로세스(`../engine`)에 맡긴다.

| 폴더 | 역할 |
|---|---|
| `src/ui/` | 메인 창, SQL 에디터, 결과 그리드, 객체 탐색기, 연결 관리 화면, 테마(`Theme`), 아이콘(`Icons`) |
| `src/core/` | 연결 정보 저장(`ConnectionStore`), 비밀번호 보안 저장(`SecretStore`), 방언별 문장 분리(`SqlSplitter`), 식별자 따옴표 처리(`SqlIdentifiers`), 자동완성 문맥 분석(`SqlCompletion`), 열린 탭 저장(`WorkspaceStore`), SQL 파일 인코딩(`TextFile`), 실행 기록(`QueryHistory`), 바인드 변수(`SqlBinds`), 결과 내보내기(`ResultExport`), SQL 정렬(`SqlFormatter`) |
| `src/ipc/` | 엔진 프로세스 실행과 통신 (`EngineClient`) |
| `resources/icons/` | 자체 아이콘 (SVG, `#ICON` 색은 테마에 맞춰 바뀜). 이후 번역(한국어·영어) |
| `tests/` | 단위 테스트 |

## SQL 에디터 단축키

| 키 | 동작 |
|---|---|
| Ctrl+Enter | 커서가 있는 문장 실행 (선택 영역이 있으면 선택 영역 안의 문장들) |
| F5 | 스크립트 전체 실행 |
| Alt+드래그, Alt+Shift+방향키 | 세로 커서·블록 선택 |
| Ctrl+Alt+위/아래 | 위/아래 줄에 커서 추가 |
| Esc | 세로 커서 끝내기 |
| Tab / Shift+Tab | 들여쓰기 / 내어쓰기 (여러 줄 선택 시 줄 단위) |
| Ctrl+/ | 줄 주석 켜기·끄기 |
| Ctrl+E | 커서가 있는 문장의 예상 실행 계획 (실행하지 않음) |
| F4 | 블록 선택한(또는 커서가 있는) 테이블·객체 이름의 DDL 스크립트 (코멘트, 인덱스, 트리거, 시노님, 권한 포함) |
| Ctrl+Space | 자동완성 목록 열기 (입력 중에는 두 글자부터, 점 뒤에서는 바로 열림). Enter/Tab 넣기, Esc 닫기 |
| Shift+마우스 휠 | 좌우 스크롤 (에디터, 결과 그리드, 탐색기) |
| Ctrl+F / Ctrl+R (Ctrl+H) | 찾기 / 바꾸기 막대. Enter 다음, Shift+Enter 이전, Esc 닫기 |
| F3 / Shift+F3 | 다음 / 이전 찾기 |
| Ctrl+Shift+F | SQL 정렬 (선택 영역, 없으면 커서가 있는 문장) |
| F8 (Ctrl+Alt+E) | 실행 기록: 검색해서 에디터에 다시 넣기 |
| Ctrl+O / Ctrl+S / Ctrl+Shift+S | SQL 파일 열기 / 저장 / 다른 이름으로 저장 |

`DESC 테이블`(DESCRIBE)은 Oracle·Tibero·PostgreSQL에서도 쓸 수 있다. 서버 SQL이 아니라 SQL*Plus·psql 명령이라 DBaro 엔진이 직접 컬럼 목록(이름, NOT NULL, 타입, 코멘트)을 만들어 결과 그리드에 보여 준다. 프로시저·함수·패키지는 인자 목록이 나온다. SQL*Plus처럼 `;` 없이 줄 끝에서 끝난다.

- 열린 탭은 저절로 남는다. 앱을 닫았다 열면 SQL, 탭마다의 연결, 열어 둔 파일이 그대로 돌아오고, 저장하지 않은 파일 수정도 남아 있다.
- 바인드 변수 `:name`, `?`(PostgreSQL 제외), SQL*Plus 치환 변수 `&name`·`&&name`(Oracle·Tibero)이 있으면 실행 전에 값을 묻는다. 지난번 값이 미리 채워지고, `&&`로 정한 값은 그 탭에서 다시 묻지 않는다.
- 결과는 오른쪽 클릭 Export... (또는 Query → Export Results...)로 Excel(.xlsx), CSV, TSV, SQL INSERT, JSON, Markdown으로 내보낸다. 전체 행을 고르면 아직 안 가져온 행까지 가져온다.

## 빌드

Qt 6.8 이상과 CMake 3.21 이상이 필요하다. 저장소 루트에서:

```bash
cmake -S app -B build/app -DCMAKE_PREFIX_PATH=<Qt 설치 경로>
cmake --build build/app --config Release
```

Windows 개발 PC 예시 (Qt 6.10.3, MSVC 2022):

```powershell
cmake -S app -B build/app -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build/app --config Release
```

실행하려면 Qt DLL이 필요하다. `C:\Qt\6.10.3\msvc2022_64\bin`을 PATH에 넣거나, 배포용으로는 `windeployqt`로 DLL을 exe 옆에 복사한다.

쿼리를 실행하려면 엔진이 설치돼 있어야 한다. `engine` 폴더에서 `./gradlew installDist`를 한 번 실행하면 앱이 자동으로 찾는다.
Java는 `JAVA_HOME`(JDK 21)을 쓴다. 다른 위치면 `DBARO_JAVA`, `DBARO_ENGINE_HOME`으로 지정한다.
