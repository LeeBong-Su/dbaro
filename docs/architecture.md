# DBaro 아키텍처

DBaro는 Qt 6(C++) 앱과 Kotlin/JVM 엔진, 두 프로세스로 동작한다.
기준은 개발 속도가 아니라 제품 완성도다.

```
[app: Qt 6 / C++]   SQL 에디터 · 결과 그리드 · 객체 탐색기 · SQL 파서와 자동완성 · 메타데이터 캐시
[dbaro-cli]         같은 연결 목록·비밀번호·실행 기록으로 명령줄에서 실행 (app의 코드를 함께 씀, 창 안의 CLI 탭도 같은 코드)
      │  표준 입출력, 결과는 페이지 단위로 전송 (protocol/README.md)
[engine: Kotlin/JVM]  JDBC 드라이버 동적 로딩 · 실행/취소 · 메타데이터 조회
```

## 왜 이렇게 나눴나

- **엔진이 JVM인 이유:** 1순위 Oracle과 5순위 Tibero를 가장 정확하게 다루는 건 벤더 JDBC 드라이버다.
  Tibero JDBC는 순수 Java 드라이버(Type 4)라 jar 하나로 모든 OS에서 동작한다.
- **앱이 Qt인 이유:** 반응성, 한글 입력(IME), 모니터별 화면 배율, OS 간 일관성, 프레임워크 수명을 함께 보면 가장 균형이 좋다.
- **별도 프로세스인 이유:** 드라이버가 응답 없이 멈추거나 JVM이 GC로 잠깐 서도 앱 화면은 계속 반응한다.
  드라이버가 죽으면 엔진만 다시 띄운다.
- **파서와 자동완성이 앱 쪽인 이유:** 키 입력마다 프로세스를 오가지 않고, GC 영향도 받지 않게 한다.

## 지원 DB와 방언 계열

| 계열 | DB | 메타데이터 | 스크립트 문장 분리 |
|---|---|---|---|
| oracle | Oracle, Tibero | `USER_*`, `DBA_*`, `V$*` 뷰. `ALL_*` 뷰는 객체가 많으면 느림 | PL/SQL 블록, 한 줄에 `/`만 있는 블록 끝 |
| mysql | MySQL, MariaDB | `information_schema`, `SHOW` 명령 | `DELIMITER` (클라이언트 명령) |
| postgres | PostgreSQL | `pg_catalog` | `$$ ... $$` 함수 본문 |

## JDBC 드라이버 배포 정책

드라이버는 앱에 넣지 않고 첫 연결 때 Maven Central에서 내려받는다. Tibero만 사용자가 jar를 지정한다.
폐쇄망에서는 같은 jar를 드라이버 폴더에 파일 이름 그대로 미리 넣어 두면 내려받지 않는다 (배포본 `README.txt`).

| DB | 드라이버 | 라이선스 | 정책 |
|---|---|---|---|
| Oracle | ojdbc11, orai18n(한글 등 문자셋) | Oracle 무료 이용 약관(FUTC) | 내려받기 |
| MariaDB | MariaDB Connector/J | LGPL 2.1 | 내려받기 |
| MySQL | MySQL Connector/J | GPLv2 + FOSS 예외 | 내려받기 (함께 배포하지 않음) |
| PostgreSQL | pgJDBC | BSD-2 | 내려받기 |
| Tibero | `tibero7-jdbc.jar` | 벤더 독점 | 사용자가 jar 경로 지정 |

## 실행 흐름

1. 쿼리 탭에서 실행하면 앱이 실행 전 안전 확인을 한다 (`app/src/core/SqlSafety`).
   - 읽기 전용 연결에서는 데이터·구조를 바꾸는 문장을 막는다.
   - WHERE 없는 UPDATE·DELETE, 운영 연결의 DROP·TRUNCATE는 확인 대화상자를 띄운다.
2. 연결 창을 열거나 탭을 고르면 그 탭이 바로 엔진에 연결한다(비밀번호는 OS 보안 저장소 또는 입력). 탭마다 별도 연결이다. 비밀번호 입력을 취소했거나 접속에 실패하면 실행할 때까지 다시 시도하지 않는다. 접속 중에 실행하면 접속이 끝난 뒤 실행한다.
3. 문장을 차례로 실행하고, 오류가 나면 멈추고 에디터 커서를 오류 위치로 옮긴다.
4. 결과는 500행씩 받는다. 표를 끝까지 스크롤하면 다음 페이지를 가져온다.
5. 수동 커밋 모드에서 DML을 실행하면 "커밋 안 된 변경"으로 표시한다. 탭을 닫거나 연결을 바꾸거나 앱을 끌 때 확인을 받고, 끊을 때는 롤백한다.

메시지 형식과 메서드는 `../protocol/README.md`에 있다.

## 연결 정보 저장

| 항목 | 위치 | 비고 |
|---|---|---|
| 연결 목록 | OS 앱 데이터 폴더의 `connections.json` (Windows: `%APPDATA%\DBaro\DBaro\connections.json`) | 비밀번호는 넣지 않는다 |
| 비밀번호 | OS 보안 저장소. Windows 자격 증명 관리자, macOS 로그인 키체인(서비스 `DBaro`), 이름 `DBaro/connection/<연결 id>` | Linux libsecret은 아직 없음. 그 OS에서는 접속할 때마다 묻는다 |

연결마다 환경(개발, 테스트, 스테이징, 운영)을 지정한다. 환경 색상은 툴바, 탐색기, 쿼리 탭 위쪽의 연결 정보 막대에 표시되고, 운영 연결은 상태 표시줄에 `PRODUCTION`으로 나온다.

Oracle 세션은 V$SESSION에서 DBaro 것으로 알아볼 수 있게 PROGRAM(`v$session.program` 접속 속성)과 MODULE(`OCSID.MODULE` 클라이언트 정보)을 `DBaro`로 정한다.

## AI 도우미

연결 창 오른쪽 도크(`ui/AiPanel`, Database → AI Assistant Ctrl+Alt+I). Claude Messages API를 직접 부른다(`ai/ClaudeClient`: C++ 공식 SDK가 없어 HTTP, SSE 스트리밍, `anthropic-version: 2026-10-21`).

- 대화 루프(`ai/AiAgent`): 응답의 tool_use를 앱이 실행하고 결과를 한 user 메시지로 돌려보내 다시 묻는다(한 답에 도구 최대 30번). 응답 블록은 바꾸지 않고 대화에 붙인다(thinking의 signature 포함). 적응형 생각(`thinking: adaptive`), `output_config.effort`, 자동 프롬프트 캐시(`cache_control`), 공식 API의 Opus·Sonnet 5.5는 서버 대체 모델(`fallbacks: "default"`, 베타 `server-side-fallback-2026-07-01`)과 도구 입력 즉시 스트리밍(`eager_input_streaming`, 입력 JSON은 앱이 엄격하게 검사하고 깨졌으면 실행하지 않는다). 거절(`refusal`)이면 그 질문 차례를 대화에서 지운다. 429·5xx·529·연결 오류는 두 번까지 다시 시도한다. 시스템 프록시와 OS 인증서 저장소를 쓴다.
- 도구(`ui/AiDbTools`, 모두 이 PC에서 실행): `list_schemas`·`list_tables`·`describe_table`·`get_ddl`은 탐색기와 같은 메타데이터 캐시, `run_query`는 따로 연 읽기 전용(JDBC read-only, 자동 커밋) 세션에서 문장 하나만(SELECT·SHOW·DESCRIBE·EXPLAIN, 괄호 안까지 쓰기 키워드·잠금 읽기·EXPLAIN ANALYZE 거부, 60초 제한, 설정한 행 수까지, 셀 300자·전체 4만 자), `explain_query`는 예상 계획만, `propose_sql`은 실행하지 않고 사용자에게 보인다(쿼리 탭에서 열기).
- 보내는 것: 질문, DB 종류·버전·연결 이름, AI가 읽은 구조와 코멘트, 포함을 고른 에디터 SQL·오류, AI가 실행한 조회 결과(설정에서 끄면 컬럼 이름·행 수만). 처음 쓸 때 안내하고 동의를 받는다. 운영 연결(또는 설정)은 조회마다 승인 창을 띄운다.
- Claude Code·Claude Desktop에서 쓰기(`cli/McpServer`, `dbaro-cli mcp`): 같은 도구를 MCP 서버(표준 입출력, 줄마다 JSON-RPC 2.0, tools만)로 낸다. 사용자의 Claude 클라이언트가 자기 로그인으로 부르고 DBaro는 Claude를 부르지 않는다. claude.ai 로그인(OAuth)을 DBaro 안에 넣거나 앱이 사용자의 `claude` CLI를 대신 실행하는 방식은 Anthropic 약관상 허용되지 않거나 확인이 필요해 쓰지 않는다. 연결마다 `AiDbTools`(자기 읽기 전용 세션)를 두고 `policy`(행 수, 값 보내기)·`approve`(창 없이 허용. 운영 연결의 `run_query`는 그 전에 막음)를 넣는다. 호출은 하나씩 처리하고, 읽기 전용 검사는 접속 전에 한다. `open_in_dbaro`는 SQL을 임시 파일(`%TEMP%\dbaro-sql-*.sql`)로 써서 `dbaro.exe --connection <id> --open-sql <파일> --title <제목>`을 실행하고(떠 있으면 단일 실행으로 그 창이 받는다), DBaro는 읽은 뒤 임시 폴더의 그 이름 파일만 지운다. 등록 안내는 `ui/ClaudeCodeDialog`(Database → Use with Claude Code...).
- Open Claude Code(`ui/ClaudeCodeLauncher`, Ctrl+Alt+C, AI 도우미 창의 Claude Code 버튼): 사용자의 `claude`를 새 콘솔(CREATE_NEW_CONSOLE, 표준 핸들은 넘기지 않음)로 띄운다. 지금 연결·스키마·DB 버전을 `--append-system-prompt`로, dbaro가 등록돼 있지 않으면(`~/.claude.json`의 `mcpServers`) `--mcp-config`(앱 데이터의 `claude-mcp.json`)를 넘긴다. 작업 폴더는 DBMS의 스크립트 폴더, 없으면 `%USERPROFILE%\DBaro`. Claude Code 세션의 환경 변수(`CLAUDECODE`, `CLAUDE_CODE_*`, `ANTHROPIC_BASE_URL` 등)는 사용자·시스템 환경에 정해 둔 것이 아니면 뺀다. 그 뒤로는 관여하지 않는다(일반 터미널에서 claude를 연 것과 같다). DBaro 안에 터미널을 넣는 방식은 쓰지 않았다(필요하면 범용 터미널 탭으로 따로).
- 설정(`ai/AiSettings`, `ui/AiSettingsDialog`): 모델, effort, API 주소(회사 게이트웨이면 베타 기능을 보내지 않는다), 행 수, 결과 보내기, 조회마다 묻기는 QSettings `ai/...`. API 키는 자격 증명 관리자 `DBaro/connection/ai-anthropic`(없으면 환경 변수 `ANTHROPIC_API_KEY`). 요금은 키의 계정에 청구된다.

## 성능 기준

목표 수치는 `../bench/README.md`에 있다. CI에서 매번 측정한다.

## 아직 정하지 않은 것

- 결과가 아주 클 때의 전송 형식 (지금은 CBOR 행 배열. 병목이 확인되면 Arrow IPC 등 검토)
- 자동완성용 파서 방식: tree-sitter 기반 또는 직접 작성 (입력 중인 잘못된 SQL을 견디고, 바뀐 부분만 다시 분석해야 함)

## 화면과 테마

배치는 IntelliJ 계열 IDE의 사용 방식을 참고하되, 아이콘과 색상은 DBaro 자체로 만든다.

- 창 구성(`AppController`): IntelliJ의 프로젝트 창처럼 연결마다 창이 하나다
  - 시작하면 연결 선택 창(`LauncherWindow`): 저장된 연결을 최근에 연 순서로 보여 주고(검색, DB 종류, 주소, 환경, 열린 창 표시) 새 연결 만들기·편집도 한다. 연결을 열면 숨는다
  - 연결 창(`MainWindow`): 제목은 `연결 이름 — DBaro`, 창 테두리는 환경 색(운영은 빨강, Windows 11). 창 안의 탭과 탐색기는 모두 그 연결만 쓴다. 다른 연결은 Database → Connections...(Ctrl+Alt+O)로 새 창에 연다. 같은 연결을 다시 고르면 열린 창을 앞으로 가져온다
  - 마지막 연결 창을 닫으면 선택 창으로 돌아가고, 선택 창까지 닫으면(또는 File → Exit) 끝난다. 선택 창의 설정으로 지난번에 열려 있던 연결 창들을 시작할 때 다시 열 수 있다
  - 프로세스는 하나다(`SingleInstance`, 사용자별 로컬 소켓). 모든 창이 Java 엔진 하나와 메타데이터·실행 기록·작업 공간 파일을 함께 쓴다. 두 번째로 실행하면 인자(`--connection <id>`, `--launcher`, `--new-connection`)를 떠 있는 DBaro에 넘기고 끝난다
  - 작업 표시줄(`WindowsIntegration`): 앱 ID `DBaro.Desktop`으로 모든 창을 한 아이콘에 묶고, 오른쪽 클릭 점프 목록에 저장된 연결(최근 순, 운영은 `(PRODUCTION)`)과 작업(Connections..., New Connection...)을 둔다. 연결 목록이 바뀌거나 연결을 열 때마다 새로 쓴다. 설치본 바로 가기에도 같은 앱 ID를 넣는다
- 메인 툴바: 왼쪽은 연결 선택 창·새 탭, 오른쪽은 실행·중지·트랜잭션
- 창 왼쪽 끝의 도구 창 막대: 도구 창(Database Explorer 등)을 열고 닫는다
- 쿼리 탭: 닫기 버튼은 선택된 탭과 마우스를 올린 탭에만 보이고, 가운데 클릭으로도 닫힌다. Ctrl+Tab·Ctrl+Shift+Tab(Ctrl+PgDn·PgUp, View 메뉴)으로 다음·이전 탭에 가고 끝에서는 반대쪽 끝으로 돈다. 에디터·CLI는 이 키를 쓰지 않고 창에 넘긴다(에디터의 Shift+Tab 내어쓰기는 Ctrl 없이만)
- 글자 크기(`FontZoom`): SQL 에디터와 CLI 탭에서 Ctrl+마우스 휠로 1pt씩(6~48pt, 터치패드의 작은 값은 모아서) 바꾼다. 둘은 따로이고, 바꾸면 모든 창의 같은 종류 화면에 바로 적용하고 설정에 남겨 새 탭과 다음 실행에도 쓴다. 상태 표시줄에 크기를 잠깐 보여 준다
- 쿼리 탭은 `Query 1`, `Query 2`... (파일 탭은 파일 이름, 객체 조회·DDL 탭은 객체 이름). 탭마다 DB 세션을 따로 연다
- 연결 정보 막대(`ConnectionBar`): 쿼리 탭 맨 위에 연결 이름을 크게 보여 준다. DB 종류·주소·사용자, 환경 배지(운영은 빨강), 읽기 전용, 커밋 안 된 변경, 접속 상태(서버 버전)를 함께 표시하고 배경도 환경 색으로 옅게 물든다
- 스키마 바꾸기: 접속하면 연결 정보 막대의 이름 옆에 그 탭의 지금 스키마(MySQL·MariaDB는 Database)가 목록으로 나온다. 고르면 엔진 `setSchema`가 Oracle·Tibero `ALTER SESSION SET CURRENT_SCHEMA`, PostgreSQL `SET search_path TO 스키마, public`, MySQL·MariaDB `USE`를 실행한다(로그에 문장이 남음). 탐색기의 스키마 오른쪽 클릭(Set as Current Schema of the Tab, MySQL은 두 번 클릭)도 같다. 에디터에서 직접 실행한 `ALTER SESSION SET CURRENT_SCHEMA`·`USE`·`SET search_path`·`SET SCHEMA`도 서버에 지금 스키마를 물어 반영한다. 고른 스키마는 탭에 기억해서(작업 공간에도 남음) 다시 접속하거나 앱을 다시 켜면 `connect`의 `schema`로 바로 적용하고, 그 스키마가 없어졌으면 기본 스키마로 돌아간다. 자동완성, DDL(F4), 테이블 상세(Shift+F4), 계획 객체 정보는 이 스키마에서 이름을 찾는다(메타데이터 세션은 탭과 다른 세션이라 `currentSchema`로 함께 보낸다)
- 데이터베이스 바꾸기(PostgreSQL): 접속하면 연결 정보 막대의 Schema 앞에 Database 목록(엔진 `metaDatabases`: 같은 서버에서 접속할 수 있는 DB, 템플릿 제외)이 나온다. PostgreSQL은 세션 안에서 DB를 바꿀 수 없어서 고르면 그 탭만 끊고 그 DB로 다시 접속한다(커밋 안 된 변경이 있으면 먼저 묻고, 스키마는 그 DB의 기본 `search_path`부터). 고른 DB는 탭에 기억해서(작업 공간에도 남음) 다시 접속·재시작해도 그 DB로 접속한다. 메타데이터는 DB마다 세션·캐시를 따로 둔다: `Db::metadataId(연결 id, DB)`를 연결 id 자리에 쓰면 `MetadataService`가 DB만 바꾼 연결 사본으로 접속한다(비밀번호는 원래 연결 것). 자동완성·스키마 목록·DDL·테이블 상세·계획 객체 정보·불러오기는 그 탭의 DB를, 탐색기·새 탭·CLI 탭은 지금 쿼리 탭의 DB를 따른다(탐색기 연결 옆에 `PostgreSQL · DB`)
- Database Explorer(`ObjectExplorer`): 연결 → 스키마 → 객체 종류(Tables, Views, Functions ...) → 객체 → 컬럼. 펼칠 때 필요한 만큼만 엔진의 `meta*` 메서드로 가져오고, 쿼리 탭과 따로 메타데이터 세션(자동 커밋, 자동완성과 공유)을 쓴다. 연결 창을 열면 바로 연결을 펼치고, 접속한 스키마는 맨 위에 굵게 두고 바로 펼친다. 테이블·뷰를 두 번 클릭하면 새 탭에서 `SELECT *`를 실행하고, 컬럼을 두 번 클릭하면 이름을 에디터에 넣는다. MySQL·MariaDB는 스키마(데이터베이스)를 두 번 클릭하면 지금 탭의 데이터베이스를 바꾼다. 여러 항목을 Ctrl·Shift+클릭으로 함께 골라 에디터로 끌어다 놓으면 놓은 자리에 이름이 `, `로 이어져 들어가고(객체는 `스키마.이름`, 따옴표가 필요한 이름은 감쌈, 트리 순서), Ctrl+C·오른쪽 클릭 Copy N Names로 복사한다. 오른쪽 클릭 메뉴: 새 쿼리 탭, 새로고침, 연결 끊기, 연결 편집, 이름 복사·넣기, 지금 스키마로
- 이름 바꾸기 리팩토링(`RenameDialog`, 탐색기 테이블·컬럼 오른쪽 클릭): 찾고 바꾸는 규칙은 `core/SqlRename`(SQL 낱말 단위: 주석 건너뜀, 문자열 안은 기본 끔, PostgreSQL `$$` 본문은 코드로, 따옴표 이름은 대소문자 그대로, 스키마·별칭·NEW·OLD 한정자 구분, 컬럼은 그 테이블을 쓰는 문장 안에서만)이고 파일·탭·DB 객체 소스에 똑같이 쓴다(`app/tests/SqlRenameTest`). DB 객체는 메타데이터 세션으로 엔진 `renameImpact`를 부른다. 적용하면 파일은 `TextFile`로 원래 인코딩·줄 끝을 지켜 쓰고, 열린 탭은 한 번의 편집으로 고치며, DB는 `SqlRename::renameStatement`와 다시 만들 객체를 이은 스크립트를 새 쿼리 탭에 연다(실행은 사용자가: 읽기 전용·운영 확인을 그대로 거친다)
- 리버스 엔지니어링 탭(`ReverseTab`, Database → Reverse Engineer, 탐색기 스키마 오른쪽 클릭): `ReverseDialog`에서 스키마와 테이블(메타데이터 캐시)을 고르면 자기 세션을 따로 열어 엔진 `reverseModel`로 모델을 한 번에 읽는다. ERD 페이지(`ErdView`, QGraphicsScene): 테이블 상자(PK·FK·PF 표시, 타입, 코멘트는 툴팁), 외래 키 선(자식 쪽 까마귀 발, 부모 쪽 막대, 컬럼 줄에 붙음), 자동 배치(외래 키로 이어진 묶음마다 가리켜지는 테이블을 왼쪽에 두는 층 배치, 부모만 되는 테이블은 자식 바로 왼쪽, 너무 긴 층은 옆 줄로, 홀로 있는 테이블은 아래 격자), 끌어 옮기기·빈 곳 끌어 화면 이동·Ctrl+휠 확대, 컬럼 전부·키만·이름만, 고른 테이블의 선 강조, PNG·SVG 내보내기(테마와 상관없이 흰 바탕). 테이블 페이지: 컬럼·키·참조·코멘트와 인덱스·FK, 테이블 정의서 내보내기(`ResultExport`로 엑셀·CSV·Markdown). DDL 페이지: 처음 볼 때 `schemaDdl`로 만들고(Cancel 가능) 저장·새 쿼리 탭으로
- 모니터 탭(`MonitorTab`, Database → Session Monitor Ctrl+Alt+M · Lock Waits · Space Usage): 창마다 하나, 자기 세션(자동 커밋, 지금 보고 있는 DB)을 따로 열어 쿼리 탭이 실행 중이어도 본다. 세션 페이지는 엔진 `monitorSessions`를 표로(배경 프로세스는 숨김, Active only·거르기, 막힌 세션은 주황·막는 세션은 빨강, 자기 세션은 기울임), 고른 세션의 SQL·대기·클라이언트를 아래에, Kill Session·Cancel Statement·Open SQL in Query Tab. 락 페이지는 `monitorLocks`를 막는 세션 → 기다리는 세션 트리와 잡힌 락 표로. 세션·락은 정한 간격(기본 5초, `monitor/autoRefreshSeconds`)마다 탭이 보일 때만 다시 읽는다. 용량 페이지(`monitorSpace`)는 테이블스페이스 사용률 막대(80%↑ 주황, 90%↑ 빨강), DB·스키마 크기, 큰 테이블·세그먼트 50개를 열 때와 Refresh 때 읽는다. 세션 종료는 확인 창(운영 DB면 먼저 경고)을 거치고 읽기 전용 연결에서는 막는다. DBaro 세션은 Oracle PROGRAM·PostgreSQL application_name이 `DBaro`라 목록에서 알아볼 수 있다
- SELECT가 아닌 문장(DML, DDL, USE 등)을 실행하면 결과 영역이 Log 탭으로 바뀐다. 스크립트는 마지막 문장 기준이다
- 결과 영역 오른쪽 위: 마지막 문장의 행 수(`Rows: 500+`처럼 더 남았으면 `+`, 스크롤로 더 읽으면 늘어남) 또는 영향 받은 행 수와 걸린 시간(`Elapsed`). 툴팁에 실행 시간과 행을 읽은 시간을 나눠 보여 주고, 실행 중에는 흐르는 시간을 보여 준다
- 오래 걸리는 일 표시(`BusyIndicator`): 0.4초가 넘으면(금방 끝나는 일은 깜빡이지 않게) 결과·Plan 화면을 흐리게 덮고 가운데에 카드(돌아가는 원호, 하는 일 `Running the query`·`Running statement 2 of 5`·`Getting the estimated plan` 등, 실행 중인 문장, 흐르는 시간, Cancel)를 띄운다. 흐린 막은 마우스를 막지 않아 이전 결과를 계속 볼 수 있다. Log·Output을 보고 있으면 흐리게 하지 않고 카드만 오른쪽 위에 둔다. 그 탭의 아이콘 자리에서도 원호가 돌아 다른 탭에서도 보인다. F4 DDL을 기다리는 동안에는 누른 탭의 에디터 오른쪽 위에 `Generating DDL for ...` 알림(여럿이면 남은 수)을, 테이블 상세(Shift+F4)는 테이블을 읽는 동안 카드를 보인다
- 자동완성(`SqlCompleter`, 문맥 분석은 `core/SqlCompletion`): 입력 중(두 글자부터, 점 바로 뒤) 또는 Ctrl+Space로 목록을 띄운다. 커서가 있는 문장만 보고 FROM/JOIN/UPDATE/INTO 뒤는 테이블·뷰·스키마, SELECT/WHERE/ON/SET 등 뒤는 그 문장에 나온 테이블의 컬럼과 별칭·함수·키워드, `별칭.` 뒤는 그 테이블의 컬럼, `스키마.` 뒤는 그 스키마의 객체를 제안한다. 외래 키로 JOIN을 잇는다: `JOIN t 별칭 ON ` 다음은 방금 JOIN한 테이블과 앞 테이블을 잇는 조건(`d.DEPT_ID = e.DEPT_ID`, 여러 컬럼 키는 AND로), `JOIN ` 다음은 앞 테이블과 이어진 테이블을 별칭과 ON 조건까지(`DEPT d ON d.DEPT_ID = e.DEPT_ID`, 별칭은 이름 단어 첫 글자, 겹치면 번호) 제안한다. 엔진 `tableInfo`의 foreignKeys·referencedBy로 양쪽 방향을 다 보고, 띄어쓰기로 열 때는 이 제안이 있을 때만 이것만 띄운다(Ctrl+Space·입력 중에는 맨 위). 문자열·주석 안에서는 띄우지 않는다. 키워드·함수는 입력한 글자의 대소문자를 따르고, 함수는 괄호 안에 커서를 둔다
- 메타데이터(`MetadataService`): Database Explorer와 자동완성이 연결마다 메타데이터 세션 하나와 캐시를 함께 쓴다. 자동완성은 비밀번호를 묻지 않는다(저장했거나 이미 입력한 연결만). 탭에서 DDL을 실행하면 그 연결의 캐시를 비워 새 객체가 바로 제안된다. Explorer의 새로고침은 캐시를 무시하고 다시 읽는다
- DDL 보기(F4): SQL 탭에서 테이블·객체 이름을 블록 선택하거나 커서를 올린 채 F4를 누르면(또는 Explorer에서 객체를 고르고 F4, 오른쪽 클릭 Show DDL) 객체 본문과 코멘트, 인덱스, 트리거, 그 객체를 가리키는 시노님, 권한 부여를 한 스크립트로 새 탭(`이름 DDL`, 같은 연결)에 연다. 같은 객체를 다시 열면 그 탭을 새로 고친다. 메타데이터 세션에서 만들므로 탭에서 쿼리가 실행 중이어도 된다. 스키마 없이 고르면 그 탭의 지금 스키마(연결 정보 막대에서 고른 것)에서 찾는다
- DESC: `DESC [schema.]object`(DESCRIBE)는 SQL*Plus·psql의 클라이언트 명령이라 Oracle·Tibero·PostgreSQL 서버가 모른다. 엔진(`Describe`)이 가로채 딕셔너리로 컬럼 목록(이름, NOT NULL, 타입, 코멘트, PostgreSQL은 기본값도)을 만들어 일반 조회 결과처럼 돌려준다. 프로시저·함수·패키지는 인자 목록, 시노님은 가리키는 객체. SQL*Plus처럼 DESC 문장은 `;` 없이 줄 끝에서 끝난다. MySQL·MariaDB는 서버가 직접 처리한다
- 실제 실행 계획(Alt+X, 엔진 `ActualPlan`): 커서가 있는 문장을 실행해서 단계마다 실제 행 수·반복·시간을 얻는다. Oracle은 `STATISTICS_LEVEL=ALL`로 실행하고 `DBMS_XPLAN.DISPLAY_CURSOR 'ADVANCED ALLSTATS LAST'`(글, 먼저 보임: Query Block Name / Object Alias, Outline Data, Peeked Binds, Predicate·Column Projection Information, Hint Report 포함)와 `V$SQL_PLAN_STATISTICS_ALL`(트리: Starts, E-Rows, A-Rows, A-Time, Buffers, Reads, Writes, OMem, 1Mem, Used-Mem, Used-Tmp. 작업 영역 값은 바이트라 글과 같게 K·M으로), PostgreSQL은 `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)`, MySQL은 `EXPLAIN ANALYZE` 트리 글을 단계로 나누고, MariaDB는 `ANALYZE` 표를 쓴다. 문장은 트랜잭션 안에서 실행하고 되돌린다(자동 커밋이면 잠시 끄고, 수동 커밋이면 세이브포인트까지만 되돌려 하던 변경은 남김). 쿼리와 DML만 받고, 실행 중에는 Cancel로 멈춘다. Plan 탭 제목에 단계 수·돌려준 행 수·실행(계획) 시간·되돌림을 보이고, 예상과 실제 행 수가 10배 이상(100행 이상) 다른 단계는 칠하고(툴팁에 두 값), 실행되지 않은 단계는 흐리게 보인다. Tibero는 아직 지원하지 않는다
- 예상 실행 계획(Ctrl+E, `PlanView`): 커서가 있는 문장을 실행하지 않고 계획만 본다. 결과 영역의 Plan 탭에 단계별 트리(비용, 예상 행 수, 접근·필터 조건)로 보여 주고, DB 고유 형식의 글(Oracle DBMS_XPLAN, PostgreSQL EXPLAIN 등)이 있으면 Text로 바꿔 볼 수 있다(`BlockSelectTextView`: Alt+드래그로 사각형 범위를 골라 한 컬럼만 복사). 테이블·인덱스를 읽는 단계는 객체 이름이 링크로 보이고, 그 링크를 두 번 클릭하면(또는 오른쪽 클릭 Show Object Info) 객체 정보 창(`PlanObjectWindow`, 연결 창마다 하나를 계속 씀, 포커스는 계획 트리에 남음)이 뜬다. MySQL·MariaDB 계획은 테이블을 별칭으로 적으므로 엔진이 문장의 FROM·JOIN에서 실제 테이블로 바꿔 둔다. 엔진 `planObject`가 인덱스면 그 테이블을 찾고, 테이블의 인덱스(컬럼, 고유, 통계)와 컬럼, 테이블 통계를 준다. 계획이 쓴 인덱스는 칠하고 그 인덱스 컬럼은 굵게 보인다. 통계는 Oracle·Tibero `ALL_TABLES`·`ALL_INDEXES`(행 수, 블록, 마지막 분석, B-tree 높이, 고유 키 수, 클러스터링 팩터), PostgreSQL `pg_class`·`pg_stat_all_*`(행 수, 크기, 스캔 횟수, 분석·VACUUM 시각, 인덱스 정의), MySQL `information_schema.TABLES`·`STATISTICS`(행 수, 크기, 카디널리티)
- 두 번 클릭한 단어: 에디터에서 단어를 두 번 클릭하면 같은 단어(대소문자 무시, 단어 단위. `$`와 Oracle의 `#`도 이름 글자)를 모두 찾기처럼 칠하고 상태 표시줄에 개수를 보여 준다. 선택이 바뀌거나(다른 곳 클릭, 입력) Esc를 누르면 지운다
- Log·Output 탭: 오른쪽 클릭 메뉴의 Clear로 비운다
- Shift+마우스 휠: 에디터, 결과 그리드, 로그, 탐색기에서 좌우로 스크롤한다
- 작업 공간(`core/WorkspaceStore`): 연결 창마다 열린 탭(SQL, 커서, 파일 경로·인코딩, DDL 탭 표시)을 앱 데이터 폴더의 `workspace.json`에 연결별로 남긴다. 창 크기·위치도 연결마다 기억한다. 편집이 1.5초 멈추면, 그리고 창을 닫을 때 저장한다(임시 파일에 쓴 뒤 바꿔치기). 그 연결 창을 다시 열면 그대로 되살리고 DB 연결은 실행할 때 다시 맺는다. 지운 연결의 탭 기록은 함께 지운다. 예전 형식(창 하나의 탭 목록)은 읽을 때 연결별로 나눈다. 저장한 그대로인 파일 탭은 파일에서 다시 읽고, 저장하지 않은 파일 수정은 내용째 남겨서 앱을 닫을 때 묻지 않는다
- SQL 파일(`core/TextFile`): Ctrl+O로 열고 Ctrl+S로 저장한다. 인코딩은 BOM → UTF-8 → OS 코드 페이지(한국어 Windows는 CP949) 순으로 알아내고, 읽은 인코딩·BOM·줄 끝(CRLF) 그대로 다시 저장한다. 코드 페이지에 없는 글자가 있으면 UTF-8로 바꿔 저장할지 묻는다. 파일 탭 제목은 파일 이름(고쳤으면 `*`), 상태 표시줄에 인코딩을 보여 준다. 고친 파일 탭을 닫으면 저장할지 묻는다
- 찾기/바꾸기(`FindBar`): 에디터 바로 위에 붙는 막대. 대소문자, 단어 단위, 정규식(바꾸기에서 `$1`), 선택 영역 안에서만(여러 줄을 고른 채 열면 자동). 찾은 곳을 모두 칠하고 위치(`3/12`)를 보여 주며, 모두 바꾸기는 한 번의 Ctrl+Z로 되돌린다
- 실행 기록(`core/QueryHistory`, `HistoryDialog`): 실행한 문장마다 연결, 시간, 결과(행 수·오류), 걸린 시간을 `history.jsonl`에 덧붙인다(최근 1만 개). 같은 연결에서 같은 문장을 연달아 실행하면 한 항목으로 합치고, `IDENTIFIED BY`·`PASSWORD '...'`의 비밀번호는 가려서 남긴다. F8로 열어 단어 검색(모든 단어 포함), 이 연결만 보기, 에디터에 넣기
- 바인드 변수(`core/SqlBinds`, `BindDialog`): 실행·실행 계획 전에 `:name`, `?`(PostgreSQL은 JSONB 연산자와 겹쳐 제외), `&name`·`&&name`(Oracle·Tibero의 SQL*Plus 치환 변수)을 찾아 값을 묻고 문장에 글로 넣는다. 문자열·주석·인용 식별자 안, `::` 변환, `:=` 대입, 배열 조각, 트리거·프로시저 본문(`:NEW`)은 건너뛴다. 값은 Auto(숫자는 그대로, 나머지는 따옴표)·Text·Raw SQL·NULL로 넣고, 치환 변수는 입력한 그대로 넣는다. 로그에는 값을 넣은 문장, 실행 기록에는 넣기 전 문장이 남고, 오류 위치는 에디터 위치로 되돌린다
- 결과 도구 막대(`ResultPanel`): 결과 표 위에 필터·Fetch All·Record·Edit·Export. 거르기와 정렬은 `QSortFilterProxyModel`로 가져온 행 안에서 한다(원본 모델은 그대로). 입력한 단어가 모두 들어 있는 행만 보이고, 셀 오른쪽 클릭으로 그 값인 행만 거른다. 머리글을 누르면 오름차순 → 내림차순 → 원래 순서이고 숫자 컬럼은 숫자로, NULL은 앞에 둔다. Record는 오른쪽에 지금 행을 세로(컬럼 | 값)로, 지금 셀의 값을 통째로(JSON은 펼쳐서) 보여 준다. 복사·내보내기는 보이는 순서를 따른다
- 결과 편집(`core/ResultEdits`, `ResultModel`): Edit을 누르면 결과를 만든 문장이 테이블 하나에서 나온 SELECT(조인·GROUP BY·DISTINCT·UNION·FROM 부질의 없음)인지 보고, 엔진 `tableInfo`로 그 테이블의 컬럼과 기본 키를 읽는다. 기본 키 컬럼이 결과에 모두 있어야 하고, 테이블에 없는 결과 컬럼(계산식·별칭)은 고칠 수 없다. 셀 고치기, + Row, Delete Rows(Ctrl+Delete, 다시 누르면 취소), Set NULL, Revert. 반영하지 않은 편집은 색으로 보인다(고친 셀 노랑, 추가한 행 초록, 지울 행 빨강·취소선). Apply는 DELETE → UPDATE → INSERT 문장을 보여 준 뒤 그 탭의 세션에서 `executeBatch`(처음 실패에서 멈춤)로 실행하고, 모두 성공해야 표의 편집을 비운다. 행은 기본 키의 원래 값으로 찾고(NULL이면 IS NULL), 값은 컬럼 타입에 맞는 SQL 글로 넣는다. 수동 커밋이면 커밋해야 남는다. 반영하지 않은 편집이 있으면 새로 실행하거나 탭을 닫을 때 묻는다
- 테이블 상세(`TableDetailTab`, Shift+F4 또는 탐색기 오른쪽 클릭 Table Details): 컬럼(키·NULL·기본값·코멘트), 인덱스, 외래 키, 이 테이블을 가리키는 외래 키, DDL(처음 볼 때 만듦)을 탭으로. 메타데이터 세션에서 읽고 Select Rows·Import Data로 이어 간다
- 데이터 불러오기(`core/DataImport`, `ImportDialog`, Query → Import Data... 또는 탐색기 오른쪽 클릭): 엑셀(.xlsx)·CSV·TSV를 테이블로. .xlsx는 Qt의 `QZipReader`(비공개 API)로 열어 공유 문자열·셀 서식(날짜 서식이면 일련번호를 날짜로, 1904 날짜 체계 포함)·수식 결과·참/거짓을 읽고, CSV는 구분자(쉼표·탭·세미콜론·|)와 인코딩(BOM, UTF-8, CP949)을 알아낸다. 있는 테이블이면 컬럼을 읽어 머리글 이름으로 짝짓고(대소문자·공백·_ 무시), 새 테이블이면 값으로 타입을 짐작해 CREATE TABLE을 먼저 만든다(이름·타입 고칠 수 있음). 숫자 컬럼의 `1,234.5`는 `1234.5`로, 날짜는 `yyyy-MM-dd`·`yyyy/M/d`·`yyyy.MM.dd`·`yyyyMMdd`(+시각)을 알아본다. 미리보기 50줄을 확인하고 Import(500문장씩 `executeBatch`, 진행 표시와 취소, 처음 실패에서 멈추기 선택) 또는 Open as SQL(새 탭에 문장을 넣음). 수동 커밋이면 끝난 뒤 커밋할지 묻는다. 예전 .xls는 .xlsx로 저장해서 쓰도록 안내한다
- 결과 내보내기(`core/ResultExport`, `ExportDialog`): Excel(.xlsx, 첫 줄 굵게·고정, 숫자 컬럼은 숫자 셀, 15자리가 넘는 숫자는 글자), CSV(엑셀용 UTF-8 BOM), TSV, SQL INSERT(Oracle 날짜는 `TO_DATE`, 바이너리는 DB별 16진 표기), JSON(컬럼 순서 유지), Markdown. 전체 행이면 남은 행을 먼저 모두 가져온다. 파일 또는 클립보드로 보내고, INSERT의 테이블 이름은 결과를 만든 문장의 FROM에서 짐작한다. .xlsx는 외부 라이브러리 없이 ZIP(deflate는 Qt의 zlib)과 XML을 직접 쓴다
- SQL 정렬(`core/SqlFormatter`, Ctrl+Shift+F): 절 키워드의 오른쪽 끝을 맞추고 목록은 앞쪽 쉼표로 줄을 나누는 모양. 부질의는 여는 괄호 뒤를 기준으로 같은 모양으로 들여 쓴다. 키워드·알려진 함수만 대문자로 바꾸고 이름·문자열·주석은 그대로 둔다. 공백과 대소문자만 바꾸므로 뜻은 바뀌지 않으며, DDL과 PL/SQL은 건드리지 않는다
- CLI 탭(`ConsoleTab`, Alt+F12 또는 File → New CLI Tab, 탐색기 연결 오른쪽 클릭): 연결 창 안에서 `dbaro-cli` 대화형을 그대로 쓴다(같은 `CliApp`, 같은 명령과 출력). 프로세스를 따로 띄우지 않고 GUI의 엔진에 세션을 하나 더 열어 쿼리 탭과 따로 실행된다. `CliApp`은 엔진과 비동기로 주고받으므로 실행 중에도 창이 멈추지 않는다(명령줄 프로그램은 한 동작이 끝날 때까지 이벤트 루프를 돌리며 기다린다). 터미널처럼 출력 맨 아래의 프롬프트 뒤에 바로 쓰고 Enter로 한 줄씩 넘긴다(앞 줄이 끝나야 다음 줄, 실행 중에는 입력을 받지 않음). 지난 출력은 고를 수 있지만 고칠 수 없고(거기서 글자를 치면 입력 자리로, 잘라내기는 복사만), 깜박이는 커서는 입력 자리에만 보인다(지난 출력을 그냥 누르면 보던 곳은 두고 커서만 입력 자리로, 글을 고르는 동안에는 숨김). 입력한 글자는 늘 입력 글자 모양이다(프롬프트 바로 뒤에서 지웠다 다시 써도 프롬프트 색을 물려받지 않음). Ctrl(또는 Alt)+드래그는 화면의 사각형 범위를 줄마다 골라(글자 가운데가 범위 안이면 포함, 돋움체 표와 영문 줄이 섞여도 보이는 그대로) Ctrl+C·오른쪽 클릭으로 복사한다. 창 안에서는 결과를 모두 가져오고 표 칸도 자르지 않는다(명령줄 프로그램은 표 1000행·칸 60자 기본, `\max`·`\width`·`--max-width`). 화면에는 20만 줄까지 남기고 넘으면 오래된 줄부터 지운다. 프롬프트가 보이는 동안 온 출력은 프롬프트 줄 위에 들어간다(`ConsoleView`). 여러 줄을 붙여 넣으면 차례로 실행하고 마지막 줄은 입력 자리에 남긴다. 탐색기 이름을 끌어다 놓으면 입력 자리에 들어간다. 위/아래 입력 기록, Ctrl+C 실행 취소·쓰던 문장 버리기, Ctrl+L 화면 지우기, 위쪽에 출력 형식·Stop·Clear·Scripts(이 DBMS의 스크립트 폴더 정하기·열기, 폴더 안 .sql 파일을 고르면 `@이름`으로 실행). 창의 Auto-commit 설정과 지금 쿼리 탭의 스키마로 시작하고, 실행 기록은 F8 목록에 바로 들어가며, 읽기 전용 연결 막기와 WHERE 없는 UPDATE·DELETE·운영 DROP 확인은 쿼리 탭과 같다. 커밋 안 된 변경이 있으면 탭·창을 닫을 때 묻는다. 코딩용 한글 글꼴(D2Coding 등)이 있으면 화면 전체에 쓰고, 없으면 표의 줄(`+--`, `|`)만 돋움체(한글이 정확히 두 칸이라 세로줄이 맞음)로, 나머지 줄과 입력은 영문 고정폭 글꼴로 그린다(돋움체는 `\`를 `₩`로 그려서)
- CLI 스크립트(`CliApp`, `core/ScriptFolders`, `ScriptFoldersDialog`): 대화형 입력과 스크립트 파일을 한 줄씩 같은 길(`handleLine`)로 처리한다. 문장 첫머리(비었거나 주석뿐)에서만 명령을 알아본다: `\` 명령, `@`·`@@`·`START`(Oracle·Tibero), SQL*Plus 명령(`PROMPT`·`REM`은 모든 DB, 나머지는 Oracle·Tibero. 명령과 `SET` 항목은 SQL*Plus의 줄여 쓰기 규칙(가장 짧은 길이 이상의 앞부분)으로 알아보고, `SET`은 SQL*Plus 항목일 때만이라 `SET TRANSACTION`·`SET ROLE`과 PostgreSQL·MySQL의 `SET`은 SQL), MySQL·MariaDB `DELIMITER`(그 뒤 문장은 앞에 `DELIMITER x`를 붙여 `SqlSplitter`가 나눈다). 스크립트 폴더는 DBMS별 설정(`scripts/<종류>`)이라 GUI와 `dbaro-cli`가 같이 쓴다. `@이름`은 스크립트 폴더 → 부른 스크립트 폴더 → 지금 폴더, `@@이름`은 부른 스크립트 폴더부터 찾고 확장자가 없으면 `.sql`을 붙여 본다. 파일은 인코딩을 알아내 읽고(`TextFile`), 줄마다 넘기며 바로 끝나는 줄은 그 자리에서 이어 간다(긴 파일도 재귀로 쌓이지 않게). 스크립트는 20단계까지 겹칠 수 있다. 치환 변수(`&1`, `&이름`, `&&이름`, `이름.`)는 SQL*Plus처럼 작은따옴표 안에서도 바꾸고 주석 안은 둔다. 정하지 않은 것은 묻고(CLI 탭은 입력 창, 명령줄은 콘솔), `SET DEFINE OFF`면 바꾸지 않는다. 오류는 파일 이름과 SQL이 시작하는 줄(앞의 주석 줄 제외)로 알린다. `\G`로 끝난 문장(문자열 밖일 때만, 여러 줄도)은 결과를 MySQL처럼 행마다 `컬럼: 값`으로 찍는다. `ACCEPT`는 그 PROMPT 글로 값을 묻고(CLI 탭은 입력 창, `HIDE`면 가림, 명령줄은 콘솔, 물을 수 없으면 DEFAULT) `DEFINE`에 넣는다. `VARIABLE`로 정한 바인드 변수는 CLI가 값을 갖고 있고, 문장의 `:이름`(문자열·주석 밖)을 엔진의 위치 바인드로 넘긴다: SQL 문장은 나온 자리마다 `?`와 값, PL/SQL 블록(`EXEC`, `BEGIN`, `DECLARE`)은 `DECLARE dbaro_b1 VARCHAR2(32767) := ?; BEGIN <원래 블록> ? := dbaro_b1; END;`로 감싸 변수마다 한 번 넣고 블록이 바꾼 값을 OUT 파라미터로 돌려받는다(같은 변수가 여러 번 나와도 하나로 묶인다). 정하지 않은 `:이름`은 실행 전에 알린다. `SET HEADING OFF`·`SET PAGESIZE 0`은 표의 머리글을 뺀다. 스크립트 오류는 SQL*Plus처럼 기본이 계속(`WHENEVER SQLERROR EXIT`면 남은 스크립트를 모두 멈춘다) `-f`와 파이프로 넘긴 스크립트도 같은 길이라 SQL*Plus 명령과 `@@`를 쓸 수 있다
- 명령줄(`dbaro-cli`, `app/src/cli`): GUI와 같은 연결 목록(`connections.json`), 저장한 비밀번호, 실행 기록(`history.jsonl`, F8에도 보임)을 쓰는 콘솔 프로그램. GUI가 떠 있어도 따로 돌고, `--gui`(대화형은 `\gui`)로 그 연결의 GUI 창을 연다(단일 실행으로 떠 있는 DBaro에 넘어감). SQL은 인자(`-e`), 파일(`-f`), 파이프로 넘긴 표준 입력에서 받고, 없으면 대화형(문장은 `;`, PL/SQL 블록은 `/` 줄, DESC는 줄 끝). 결과는 표(한글을 두 칸으로 세어 줄을 맞춤, `core/TextTable`)·CSV·TSV·JSON(내보내기와 같은 `ResultExport`), 행 수·시간은 표 형식이 아니면 표준 오류로 보내 결과만 파일로 받을 수 있다. 비밀번호는 저장한 것 → `DBARO_PASSWORD` → 보이지 않게 입력. Windows 콘솔은 UTF-16 API로 읽고 써서 한글이 깨지지 않고, 파이프는 UTF-8. Ctrl+C는 실행 중인 문장을 취소한다. 끝 코드 0 성공, 1 실패한 문장, 2 사용법·연결 오류
- 테마(`app/src/ui/Theme`): OS와 상관없이 같은 모습이 되도록 Fusion 스타일에 자체 색상을 입힌다. 기본은 다크 그레이, View → Theme에서 라이트로 바꿀 수 있고 선택은 저장된다. Windows 제목 표시줄도 테마 밝기를 따른다
- 아이콘(`app/resources/icons`): 16×16 SVG. `#ICON` 색은 그릴 때마다 현재 테마의 아이콘 색으로 바뀐다

## SQL 에디터

`QPlainTextEdit`를 확장한 자체 컴포넌트(`app/src/ui/SqlEditor`)다.
KTextEditor는 KDE 프레임워크 의존이 무겁고, QScintilla는 GPL이라 쓰지 않았다.
에디터 API를 DBaro가 갖고 있으므로, 나중에 렌더링까지 자체 구현으로 바꿔도 다른 코드는 바뀌지 않는다.

- 문장 분리(`app/src/core/SqlSplitter`)는 직접 작성한 스캐너다. 문자열·인용 식별자·주석 안의 구분자를 무시하고, 방언별 규칙(Oracle `/` 줄과 PL/SQL 블록, q'[...]', MySQL `DELIMITER`, PostgreSQL `$$`, Oracle·PostgreSQL의 줄 단위 `DESC`)을 따른다.
- 세로 커서는 줄마다 `QTextCursor`를 하나씩 두는 방식이다. 조합이 끝난 한글은 모든 커서에 들어가고, 조합 중인 글자는 주 커서에만 보인다.
- Qt 라이선스: LGPLv3 동적 링크 유지 또는 상용 라이선스
