# app — Qt 6 데스크톱 앱 (C++)

사용자가 보는 화면 전체와, 키 입력마다 반응해야 하는 기능을 맡는다.
DB 연결은 직접 하지 않고 엔진 프로세스(`../engine`)에 맡긴다.

| 폴더 | 역할 |
|---|---|
| `src/cli/` | 명령줄 도구 `dbaro-cli` (`CliApp`, 콘솔 입출력 `CliConsole`). 창 안의 CLI 탭(`ui/ConsoleTab`)도 `CliApp`을 쓴다 |
| `src/ui/` | 앱 전체와 창 관리(`AppController`), 연결 선택 창(`LauncherWindow`), 연결 창(`MainWindow`), SQL 에디터, 결과 그리드, 객체 탐색기, 연결 관리 화면, 단일 실행(`SingleInstance`), 작업 표시줄 연동(`WindowsIntegration`), 테마(`Theme`), 아이콘(`Icons`) |
| `src/ai/` | AI 도우미의 Claude API 연결(`ClaudeClient`, SSE 스트리밍), 도구 루프(`AiAgent`), 설정과 API 키(`AiSettings`). 화면과 DB 도구는 `ui/AiPanel`·`ui/AiDbTools` |
| `src/core/` | 연결 정보 저장(`ConnectionStore`), 비밀번호 보안 저장(`SecretStore`), 방언별 문장 분리(`SqlSplitter`), 식별자 따옴표 처리(`SqlIdentifiers`), 자동완성 문맥 분석(`SqlCompletion`), 열린 탭 저장(`WorkspaceStore`), SQL 파일 인코딩(`TextFile`), 실행 기록(`QueryHistory`), 바인드 변수(`SqlBinds`), 결과 내보내기(`ResultExport`), SQL 정렬(`SqlFormatter`), 터미널 글자 표(`TextTable`) |
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
| Alt+X | 커서가 있는 문장을 실행해서 얻은 실제 실행 계획 (단계별 실제 행 수·시간, INSERT·UPDATE·DELETE·MERGE는 실행 후 되돌림) |
| F4 | 블록 선택한(또는 커서가 있는) 테이블·객체 이름의 DDL 스크립트 (코멘트, 인덱스, 트리거, 시노님, 권한 포함) |
| Shift+F4 | 테이블 상세 (컬럼, 인덱스, 외래 키, DDL) |
| Ctrl+Delete | 결과 편집 중 고른 행을 지울 행으로 표시 (다시 누르면 취소) |
| 두 번 클릭 | 그 단어를 고르고 같은 단어를 모두 칠한다 (Esc로 지움) |
| Ctrl+Space | 자동완성 목록 열기 (입력 중에는 두 글자부터, 점 뒤에서는 바로 열림). Enter/Tab 넣기, Esc 닫기. `JOIN `·`ON ` 다음 띄어쓰기는 외래 키로 잇는 테이블·조건이 있으면 그것만 띄움 |
| Shift+마우스 휠 | 좌우 스크롤 (에디터, 결과 그리드, 탐색기) |
| Ctrl+F / Ctrl+R (Ctrl+H) | 찾기 / 바꾸기 막대. Enter 다음, Shift+Enter 이전, Esc 닫기 |
| F3 / Shift+F3 | 다음 / 이전 찾기 |
| Ctrl+Shift+F | SQL 정렬 (선택 영역, 없으면 커서가 있는 문장) |
| F8 (Ctrl+Alt+E) | 실행 기록: 검색해서 에디터에 다시 넣기 |
| Alt+F12 | CLI 탭 열기 (`dbaro-cli` 대화형을 창 안에서) |
| Ctrl+Tab / Ctrl+Shift+Tab (Ctrl+PgDn / Ctrl+PgUp) | 다음 / 이전 탭 (끝에서는 반대쪽 끝으로) |
| Ctrl+마우스 휠 | 글자 크기 (SQL 에디터, CLI 탭 따로. 모든 탭에 적용되고 다음에도 유지) |
| Ctrl+O / Ctrl+S / Ctrl+Shift+S | SQL 파일 열기 / 저장 / 다른 이름으로 저장 |

`DESC 테이블`(DESCRIBE)은 Oracle·Tibero·PostgreSQL에서도 쓸 수 있다. 서버 SQL이 아니라 SQL*Plus·psql 명령이라 DBaro 엔진이 직접 컬럼 목록(이름, NOT NULL, 타입, 코멘트)을 만들어 결과 그리드에 보여 준다. 프로시저·함수·패키지는 인자 목록이 나온다. SQL*Plus처럼 `;` 없이 줄 끝에서 끝난다.

- 연결마다 창이 따로 열린다. 시작하면 연결 선택 창이 뜨고, 연결을 고르면 그 연결의 창이 열리며 바로 접속한다(탐색기도 펼침). 다른 연결은 Ctrl+Alt+O(Database → Connections...)로 새 창에 연다. Windows 작업 표시줄의 DBaro 아이콘을 오른쪽 클릭하면 저장된 연결을 바로 열 수 있다.
- 열린 탭은 연결마다 저절로 남는다. 그 연결 창을 다시 열면 SQL과 열어 둔 파일이 그대로 돌아오고, 저장하지 않은 파일 수정도 남아 있다.
- 바인드 변수 `:name`, `?`(PostgreSQL 제외), SQL*Plus 치환 변수 `&name`·`&&name`(Oracle·Tibero)이 있으면 실행 전에 값을 묻는다. 지난번 값이 미리 채워지고, `&&`로 정한 값은 그 탭에서 다시 묻지 않는다.
- 결과는 결과 표 위의 Export... (또는 오른쪽 클릭, Query → Export Results...)로 Excel(.xlsx), CSV, TSV, SQL INSERT, JSON, Markdown으로 내보낸다. 전체 행을 고르면 아직 안 가져온 행까지 가져온다.
- 결과 표 위의 도구 막대: 행 거르기(필터, 셀 오른쪽 클릭 → Filter by This Value), 머리글을 눌러 정렬, Fetch All(남은 행 모두), Record(지금 행을 세로로, 긴 값·JSON 보기), Edit(테이블 하나의 결과를 고쳐 UPDATE·INSERT·DELETE로 반영. 기본 키가 결과에 있어야 한다).
- 엑셀(.xlsx)·CSV 파일을 테이블로 불러오기: Query → Import Data... 또는 탐색기에서 테이블(있는 테이블)이나 스키마(새 테이블) 오른쪽 클릭.
- 스키마 바꾸기: 쿼리 탭 위 연결 정보 막대의 Schema(MySQL·MariaDB는 Database) 목록에서 고른다. Oracle·Tibero는 `ALTER SESSION SET CURRENT_SCHEMA`, PostgreSQL은 `search_path`, MySQL은 `USE`. 탭마다 기억해서 다시 접속해도 유지된다.
- 탐색기에서 여러 객체·컬럼을 Ctrl·Shift+클릭으로 골라 에디터로 끌어다 놓으면 이름이 들어간다. Ctrl+C로 복사.
- 실행 계획(Ctrl+E, Alt+X) 트리에서 Object 칸의 링크(MySQL은 Operation 칸)를 두 번 클릭하면 그 단계가 읽는 테이블·인덱스 정보(인덱스와 통계, 컬럼)가 별도 창에 나온다. 오른쪽 클릭 Show Object Info도 같다.
- 실제 실행 계획(Alt+X)은 예상과 실제 행 수가 10배 이상(100행 이상) 다른 단계를 주황색으로 칠한다. 옵티마이저가 잘못 짐작한 곳이라 통계를 확인해 볼 자리다. Oracle은 `DBMS_XPLAN.DISPLAY_CURSOR 'ADVANCED ALLSTATS LAST'` 글(Query Block Name, Outline Data, Peeked Binds, Predicate·Column Projection Information 포함)을 먼저 보이고(Tree로 바꿔 볼 수 있음), `V$SESSION`, `V$SQL_PLAN_STATISTICS_ALL`을 읽을 권한(예: SELECT_CATALOG_ROLE)이 있어야 한다.
- 실행 계획의 Text 보기는 Alt+드래그로 사각형(세로) 범위를 고른다. 표의 한 컬럼(Operation, Rows 등)만 골라 Ctrl+C(오른쪽 클릭 Copy)로 복사한다. 그냥 누르거나 Esc를 누르면 풀린다.
- Log·Output 탭은 오른쪽 클릭 → Clear로 비운다.
- 실행·계획·F4 DDL·테이블 상세가 0.4초 넘게 걸리면 기다리는 중임을 보인다: 결과 영역에 하는 일·문장·흐르는 시간·Cancel 카드, 탭 아이콘의 돌아가는 원호, F4는 에디터 오른쪽 위 알림.

## 명령줄 (dbaro-cli)

GUI와 같은 연결 목록·저장한 비밀번호·실행 기록을 쓴다. `dbaro.exe` 옆에 있다.
연결 창 안에서도 CLI 탭(Alt+F12)으로 같은 명령을 쓸 수 있다. 터미널처럼 프롬프트 뒤에 바로 쓰고 Enter(커서는 늘 입력 자리에만 보인다). 위/아래로 입력 기록, Ctrl+C로 실행 취소(글자를 고른 채면 복사), Ctrl+L 화면 지우기, Ctrl+드래그(또는 Alt+드래그)로 표의 컬럼처럼 사각형을 골라 복사, 여러 줄을 붙여 넣으면 차례로 실행한다. CLI 탭은 행 수와 칸 너비를 자르지 않고 모두 보여 준다(`\max`, `\width`로 제한할 수 있다).

```powershell
dbaro-cli -l                                       # 저장된 연결
dbaro-cli dev "select * from emp"                   # 실행하고 끝 (연결은 이름, 이름 일부, id)
dbaro-cli -c dev -f script.sql --format csv -o out.csv --bom
Get-Content script.sql | dbaro-cli dev              # 표준 입력의 스크립트
dbaro-cli dev                                      # 대화형: 문장은 ; 로 끝, \help, \q
dbaro-cli dev --gui                                # 그 연결의 GUI 창 열기
```

**SQL 파일 실행 (SQL*Plus처럼):** DBMS마다 스크립트 폴더를 정해 두면(Database → Script Folders..., CLI 탭의 Scripts, `\scripts <폴더>`) `@이름 [인자...]`(`.sql` 생략 가능, 하위 폴더는 `@폴더/이름`)으로 실행한다. `@@이름`은 부른 스크립트 옆 파일, 전체 경로도 된다. 인자는 `&1`, `&2`...로, `DEFINE 이름 = 값`은 `&이름`으로 들어가고(작은따옴표 안도), 정하지 않은 `&이름`은 값을 묻는다(`&&`는 한 번만). Oracle·Tibero 스크립트의 `PROMPT`, `REM`, `SET`(SQL*Plus처럼 줄여 써도 됨: `SET HEAD OFF`, `SET FEED ON`, `SET PAGES 0` ... `ECHO`·`DEFINE`·`FEEDBACK`·`HEADING`·`PAGESIZE 0`·`AUTOCOMMIT`만 동작하고 나머지 화면 설정은 무시), `ACCEPT 변수 [DEFAULT 값] [PROMPT '글'] [HIDE]`, `DEFINE`, `UNDEFINE`, `VARIABLE 이름 타입 [= 값]`(NUMBER, VARCHAR2, CHAR, CLOB)과 문장 안의 `:이름` 바인드 변수, `PRINT`, `EXEC`, `WHENEVER SQLERROR EXIT|CONTINUE`, `EXIT`(스크립트만 끝냄), `COL ... FORMAT/HEADING`(무시)을 알아듣고, MySQL·MariaDB는 `DELIMITER`. 오류는 `ERROR at 파일:줄`로 알리고 SQL*Plus처럼 다음 문장으로 넘어간다(`WHENEVER SQLERROR EXIT`나 `--stop-on-error`면 멈춤).

**세로 보기:** 문장 끝을 `;` 대신 `\G`로 쓰면 결과를 행마다 `컬럼: 값`으로 보여 준다 (MySQL처럼).

`--format table|csv|tsv|json`, `--max-rows <n>`(표는 기본 1000, 나머지는 모두), `--max-width <n>`(표 칸 너비, 기본 60, 0이면 자르지 않음), `--schema <name>`, `--continue-on-error`(`-e`의 여러 문장), `--stop-on-error`(스크립트 파일), `--no-autocommit`, `--no-history`, `-q`. 비밀번호는 저장한 것 → 환경 변수 `DBARO_PASSWORD` → 입력. 끝 코드는 0 성공, 1 실패한 문장이 있음, 2 사용법·연결 오류.

**Claude Code·Claude Desktop에서 쓰기 (MCP 서버):** `dbaro-cli mcp`는 DBaro의 DB 도구를 MCP 서버(표준 입출력)로 낸다. 사용자의 Claude Code·Claude Desktop이 자기 Claude 로그인(구독)으로 부르고, DBaro는 Claude를 부르지 않는다(API 키 필요 없음). 등록 명령과 Claude Desktop 설정은 Database → Use with Claude Code...에서 복사하거나 바로 등록할 수 있다.

```powershell
claude mcp add --transport stdio --scope user dbaro -- "<DBaro 폴더>\dbaro-cli.exe" mcp
```

도구: `list_connections`, `list_schemas`, `list_tables`, `describe_table`, `get_ddl`, `run_query`(문장 하나, 읽기 전용 세션, 행 수 제한), `explain_query`(예상 계획), `open_in_dbaro`(SQL을 실행하지 않고 DBaro의 새 쿼리 탭에 연다: `dbaro.exe --connection <id> --open-sql <임시 파일> --title <제목>`). GUI의 AI 도우미와 같은 코드라 읽기 전용 검사가 같다. 비밀번호를 저장한 연결만 쓸 수 있다(물을 수 없다. `DBARO_PASSWORD`가 있으면 그것). 운영 연결은 `run_query`를 막는다(구조·계획은 된다). 옵션: `-c <연결>`(여러 번, 그 연결만), `--allow-production`, `--max-rows <n>`(기본 100, 최대 1000), `--no-rows`(값 없이 컬럼 이름·행 수만), `--connections-file <path>`.

**Open Claude Code (Ctrl+Alt+C):** Database 메뉴나 AI 도우미 창의 Claude Code 버튼. 사용자의 `claude`를 새 터미널(Windows 11은 기본 터미널, 보통 Windows Terminal)에서 연다. 지금 연결·스키마·DB 버전을 `--append-system-prompt`로 알려 연결 이름을 말하지 않아도 그 연결의 dbaro 도구를 쓴다. dbaro MCP 서버가 등록돼 있지 않으면 그 세션에만 `--mcp-config`로 붙인다. 작업 폴더는 그 DBMS의 스크립트 폴더(없으면 `%USERPROFILE%\DBaro`, OneDrive와 동기화되는 문서 폴더는 쓰지 않는다). DBaro는 띄우기만 하고 입출력을 읽지 않는다. AI 도우미 창의 **Ask**를 **Claude Code**로 두면(API 키가 없으면 기본) Explain SQL·Tune SQL·Fix Error와 입력란의 질문이 새 Claude Code 터미널의 첫 질문으로 열린다(`claude "질문"`, 사용자의 구독으로). Windows 명령줄에 넣기에 너무 긴 질문(2만 자 넘음)은 클립보드에 넣고 알린다. Claude Code 세션에서 DBaro를 띄웠어도 그 세션의 환경 변수(`CLAUDE_CODE_*` 등, 사용자가 직접 정한 것은 빼고)는 넘기지 않는다.

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

단위 테스트(`tests/`, Qt Test)는 `dbaro_tests`(SQL 이름 바꾸기)와 `dbaro_ai_tests`(AI 도우미: 이 PC 안의 가짜 Claude API 서버로 스트리밍·도구 루프·대화 규칙, 읽기 전용 검사. 실제 API는 부르지 않는다)로 빌드된다. Qt Test 모듈이 없으면 건너뛴다 (`-DDBARO_BUILD_TESTS=OFF`로 끌 수 있다).

```powershell
cmake --build build/app --config Release --target dbaro_tests dbaro_ai_tests
ctest --test-dir build/app -C Release --output-on-failure   # Qt DLL이 PATH에 있어야 한다
```

쿼리를 실행하려면 엔진이 설치돼 있어야 한다. `engine` 폴더에서 `./gradlew installDist`를 한 번 실행하면 앱이 자동으로 찾는다.
Java는 `JAVA_HOME`(JDK 21)을 쓴다. 다른 위치면 `DBARO_JAVA`, `DBARO_ENGINE_HOME`으로 지정한다.
