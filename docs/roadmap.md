# 개선 계획

해야 할 일과 진행 상황. 끝낸 항목은 `[x]`로 표시하고 어떻게 했는지 한 줄 남긴다.

## 우선개선

- [x] 연결을 고르면 바로 DB에 연결
  연결 창을 열면(연결 선택 창, 점프 목록, 시작할 때 다시 열기) 지금 탭이 곧바로 접속하고 탐색기도 펼친다. 다른 탭은 고를 때 접속, 접속 중에 실행하면 접속 뒤 실행. 비밀번호 입력을 취소하거나 실패하면 다시 실행할 때까지 묻지 않는다.
- [x] Oracle 스키마 바꾸기 (목록에서 고르기)
  탭 위 연결 정보 막대의 Schema 목록 → 엔진 `setSchema`(Oracle·Tibero `ALTER SESSION SET CURRENT_SCHEMA`, PostgreSQL `search_path`, MySQL `USE`). 직접 실행한 문장도 반영, 탭마다 기억해서 다시 접속·재시작해도 적용. 자동완성·DDL·테이블 상세·계획 객체 정보도 그 스키마 기준(`currentSchema`).
- [x] CLI 함께 쓰기 (GUI와 CLI 병행)
  `dbaro-cli`: GUI와 같은 연결·비밀번호·실행 기록. 인자·파일·파이프·대화형(`\schema`, `\format`, `\gui` 등), 표(한글 폭 맞춤)·CSV·TSV·JSON, `-o` 파일, Ctrl+C 취소, `--gui`로 GUI 창 열기. 배포 스크립트에 포함.
- [x] 두 번 클릭한 단어와 같은 단어를 모두 표시
  찾기로 찾은 것처럼 같은 단어(대소문자 무시, 단어 단위, `$`·Oracle `#` 포함)를 칠하고 상태 표시줄에 개수. 선택이 바뀌거나 Esc면 지운다.
- [x] Log 창 비우기
  Log·Output 오른쪽 클릭 → Clear.
- [x] 탐색기에서 쿼리 탭으로 끌어다 놓으면 이름이 들어가게
  놓은 자리에 `스키마.이름`(필요하면 따옴표)이 들어가고 에디터로 초점이 옮겨 간다.
- [x] 탐색기에서 여러 객체를 함께 골라 끌어다 놓거나 이름 복사
  Ctrl·Shift+클릭, 트리 순서로 `, `로 잇는다. Ctrl+C, 오른쪽 클릭 Copy N Names·Insert N Names into Editor.
- [x] 예상 실행 계획 트리에서 오퍼레이션을 누르면 쓰인 객체 정보(테이블·인덱스)를 별도 창으로
  엔진이 계획 단계마다 객체를 알려 주고(`object`), `planObject`가 인덱스→테이블, 인덱스·컬럼·통계를 준다. 쓴 인덱스는 칠하고 그 컬럼은 굵게. 창은 하나를 계속 쓰고 Table Details로 이어 간다.
- [x] 예상 실행 계획 트리의 줄 높이를 조금 더 크게
  줄 위아래 여백 4px.
- [x] CLI를 GUI 안에서 탭으로
  Alt+F12(File → New CLI Tab, 탐색기 연결 오른쪽 클릭). `dbaro-cli` 대화형과 같은 코드(`CliApp`, 비동기로 바꿈)를 GUI 엔진의 새 세션으로 돌린다. 터미널처럼 프롬프트 뒤에 바로 입력(지난 출력은 고칠 수 없음), 입력 기록, 여러 줄 붙여 넣기, Ctrl+C 취소, 형식·Stop·Clear, 실행 기록(F8)·위험 문장 확인·커밋 안 된 변경 확인은 쿼리 탭과 같다.

- [x] 탭 뒤로 가기 (Ctrl+Shift+Tab)
  창 단축키 Next Tab(Ctrl+Tab, Ctrl+PgDn)·Previous Tab(Ctrl+Shift+Tab, Ctrl+PgUp). 에디터가 Ctrl+Shift+Tab을 내어쓰기로 먹던 것을 고쳤다.
- [x] SQL 탭·CLI에서 Ctrl+마우스 휠로 글자 크기
  에디터·CLI 따로, 모든 탭에 바로 적용하고 다음 실행에도 유지.

- [x] CLI 탭: Ctrl+드래그 사각형 선택·복사, 지웠다 쓴 글자가 파란색이 되던 것, 커서는 입력 자리에만, 자르지 않고 모두 출력
  Alt+드래그도 사각형 선택. 창 안에서는 모든 행·긴 값 그대로(`\max`, `\width`로 제한), 명령줄 프로그램에 `--max-width`.

- [x] CLI에서 DBMS별 SQL 파일 실행
  DBMS별 스크립트 폴더(Database → Script Folders..., CLI 탭 Scripts, `\scripts`), `@파일명`·`@@`·`START`와 인자(`&1`), SQL*Plus 명령(PROMPT, SET ECHO/DEFINE, WHENEVER SQLERROR, DEFINE, EXEC, EXIT ...), 치환 변수, 오류 위치 `파일:줄`. MySQL `DELIMITER`. 명령줄 `-f`도 같다.
- [x] CLI에서 `\G`로 끝내면 세로(레코드) 보기 (MySQL처럼)
- [x] SQL*Plus로 쓰던 운영 스크립트 호환
  SET 줄여 쓰기(`SET HEAD`), `SET HEADING OFF`·`PAGESIZE 0`, `ACCEPT ... PROMPT/DEFAULT/HIDE`, `VARIABLE`·`PRINT`·`:바인드`(엔진 위치 바인드, PL/SQL은 OUT으로 돌려받음), 스크립트 오류는 기본 계속(`WHENEVER SQLERROR EXIT`, `--stop-on-error`).

- [x] 오래 걸리는 쿼리·F4 DDL·테이블 상세를 기다리는 동안 SQL 탭에 표시
  0.4초가 넘으면 결과 영역에 카드(돌아가는 원호, 하는 일, 문장, 흐르는 시간, Cancel. 결과는 흐리게, Log·Output은 오른쪽 위에 작게), 탭 아이콘에 돌아가는 원호, F4는 에디터 오른쪽 위 알림.

- [x] 연결 선택 창 목록: 컬럼별 정렬, 너비·순서 바꾸기, 그 상태 저장
  머리글을 누르면 오름 → 내림 → 최근 순(대소문자 무시, 숫자는 크기대로, 환경은 개발 → 운영 순). 같은 값은 최근 순서 유지. 끌어서 순서·너비 변경(이름 컬럼도), 머리글 오른쪽 클릭 Reset Columns. 설정 `launcher/columns`에 저장, 시작할 때는 정렬과 상관없이 마지막에 연 연결을 고른다.

- [x] PostgreSQL 쿼리 탭에서 데이터베이스 바꾸기
  연결 정보 막대의 Database 목록(엔진 `metaDatabases`). 세션 안에서는 못 바꿔서 그 탭만 그 DB로 다시 접속한다(커밋 안 된 변경은 먼저 확인). 탭마다 기억해서 다시 켜도 유지, 자동완성·스키마 목록·DDL·테이블 상세는 그 탭의 DB, 탐색기·새 탭·CLI 탭은 지금 탭의 DB를 따른다. 개발용 두 번째 DB `dbaro_archive`(`dev/seed/postgres/02-archive-db.sql`).

- [x] 세션·락 모니터, 세션 종료
  Database → Session Monitor(Ctrl+Alt+M)·Lock Waits. 모니터 탭이 자기 세션으로 세션 목록(대기, 막힌·막는 세션 색, 실행 중·마지막 SQL, 트랜잭션 시간)과 락 대기 트리·잡힌 락을 5초마다 읽는다. Kill Session·Cancel Statement(확인, 운영 경고, 읽기 전용이면 막음). Oracle `V$SESSION`, PostgreSQL `pg_stat_activity`·`pg_blocking_pids`, MySQL `data_lock_waits`, MariaDB `INNODB_LOCK_WAITS`. 권한이 모자라면 무엇이 필요한지 알린다. Tibero는 같은 이름의 뷰로 맞췄지만 확인하지 못했다.
- [x] 테이블스페이스·DB 크기 사용량
  Database → Space Usage. 테이블스페이스 사용률 막대(할당 대비, 자동 확장이면 최대 대비도), DB·스키마 크기, 큰 테이블·세그먼트 50개. Oracle `DBA_DATA_FILES`·`DBA_FREE_SPACE`·`DBA_SEGMENTS`, PostgreSQL `pg_database_size`·`pg_total_relation_size`, MySQL `information_schema.FILES`·`TABLES`.

## 2차개선

- [x] 리버스 엔지니어링 (DB 구조 → 모델·ERD·DDL 스크립트)
  Database → Reverse Engineer, 탐색기 스키마 오른쪽 클릭. 스키마·테이블을 고르면 탭에 ERD(PK·FK 표시, 외래 키 선, 가리켜지는 테이블을 왼쪽에 두는 자동 배치, 끌어 옮기기, Ctrl+휠 확대, 컬럼 전부·키만·이름만, PNG·SVG 내보내기는 흰 바탕), 테이블 정의(컬럼·키·참조·코멘트·인덱스·FK, 엑셀·CSV·Markdown 테이블 정의서), 스키마 DDL(외래 키 순서, 저장·쿼리 탭으로). 모델은 스키마 단위 시스템 뷰로 한 번에 읽어 빠르다. 남은 것: Oracle DDL은 DBMS_METADATA를 객체마다 여러 번 불러 객체당 1초쯤 걸린다(한 번에 받아 오기), ERD 배치 저장.
- [x] 외래 키(FK) 기반 JOIN 자동 완성 (`JOIN dept d ON ` 뒤에 `d.DEPT_ID = e.DEPT_ID`, `JOIN ` 뒤에 `DEPT d ON ...` 처럼 이어진 테이블까지. 양쪽 방향·여러 컬럼 키)
- [x] 이름 바꾸기 리팩토링: 테이블·컬럼 이름을 바꿀 때 그 객체를 참조하는 SQL 파일·뷰·프로시저를 찾아 함께 고친다
  탐색기 테이블·컬럼 오른쪽 클릭 → Rename. 새 이름을 넣고 Find References: DB 객체(엔진 `renameImpact`: Oracle `ALL_DEPENDENCIES`·시노님, PostgreSQL 함수 본문(뷰·제약 조건은 PostgreSQL이 스스로), MySQL 뷰·루틴·트리거), 스크립트 폴더의 SQL 파일, 열린 쿼리 탭. 찾기는 `SqlRename`(주석은 건너뛰고 문자열 안은 기본 끔, 따옴표 이름은 대소문자 그대로, 다른 스키마로 한정한 이름은 제외, 컬럼은 그 테이블을 쓰는 문장 안에서만·별칭(`FROM emp e`)·NEW·OLD로 한정하면 확실, 다른 한정자면 끔). 곳마다 체크하고 전·후 줄 미리보기. Apply: 파일은 바로 쓰고(인코딩 유지, 찾은 뒤 바뀐 파일은 건너뜀), 탭은 한 번의 편집(Ctrl+Z), DB는 이름 바꾸는 문장 + 다시 만드는 객체를 새 쿼리 탭에 연다(실행해야 바뀐다). MariaDB는 `CREATE OR REPLACE`, MySQL은 지우고 다시 만들며 원래 정의를 주석으로 남긴다. Oracle·PostgreSQL·MySQL·MariaDB에서 바꾸고 되돌리는 왕복 확인.
- [x] AI 에이전트형 DB 도구 (AI 모델 연결)
  Database → AI Assistant(Ctrl+Alt+I), 오른쪽 도크. Claude(Opus 5.5 기본, Sonnet·Haiku 5.5)와 대화하며 AI가 도구로 이 DB를 직접 살핀다: 스키마·테이블 목록, 테이블 구조(컬럼·키·참조·인덱스·코멘트), DDL, 읽기 전용 조회(문장 하나, 따로 연 읽기 전용 세션, 기본 100행, WITH 안의 DELETE·SELECT INTO·FOR UPDATE도 막음), 예상 실행 계획. 데이터·구조를 바꾸는 SQL은 실행하지 않고 제안만 해서 사용자가 쿼리 탭에서 연다. 빠른 작업: 에디터 SQL 설명(Explain SQL)·성능 검토(Tune SQL)·마지막 오류 고치기(Fix Error). 처음 쓸 때 무엇이 전송되는지 안내하고 동의를 받는다. 운영 DB는 조회마다 승인, 결과 값을 보내지 않는 설정(컬럼 이름·행 수만). API 키는 자격 증명 관리자, 회사 게이트웨이 주소 지정 가능. 가짜 API 서버로 스트리밍·도구 루프 단위 테스트(`dbaro_ai_tests`), Oracle·PostgreSQL·MySQL에서 도구 확인. 실제 API 호출은 키가 있어야 확인할 수 있다.
  Claude 구독으로 쓰기: Database → Use with Claude Code... 로 `dbaro-cli mcp`(MCP 서버)를 사용자의 Claude Code·Claude Desktop에 등록하면 API 키 없이 같은 도구를 쓴다(연결 고르기, 운영 연결은 조회 막음, 바꾸는 SQL은 `open_in_dbaro`로 DBaro 새 쿼리 탭에). 프로토콜·도구 단위 테스트, 표준 입출력으로 Oracle·PostgreSQL·MySQL 도구 확인. 실제 Claude Code에 등록해 쓰는 것은 확인하지 않았다.
  Open Claude Code(Ctrl+Alt+C, AI 도우미의 Claude Code 버튼): 지금 연결·스키마를 알려 준 채로 사용자의 Claude Code를 새 터미널에서 연다(작업 폴더는 스크립트 폴더). AI 도우미의 Ask를 Claude Code로 두면 Explain·Tune·Fix Error와 입력란의 질문이 그 터미널의 첫 질문으로 열린다(API 키 없이). 써 보고 부족하면 DBaro 안의 범용 터미널 탭(ConPTY + VT 에뮬레이터)을 검토한다.
- [x] 실제 실행 계획 (실행해서 얻은 계획과 통계), 단축키 Alt+X: Oracle `V$SQL_PLAN_STATISTICS_ALL`·`DISPLAY_CURSOR ADVANCED ALLSTATS LAST`, PostgreSQL `EXPLAIN ANALYZE`, MySQL `EXPLAIN ANALYZE`, MariaDB `ANALYZE`. DML은 실행 후 되돌림, 예상과 크게 다른 행 수는 칠함, Oracle은 ADVANCED ALLSTATS LAST 글을 먼저 보임. Tibero는 아직

- [x] 실제 DB(Oracle Free, PostgreSQL, MySQL, MariaDB 컨테이너)로 확인하는 환경
  `dev/docker-compose.yml`을 띄우면 샘플 스키마(`dev/seed`: FK·두 컬럼 FK·자기 참조, 10만 행, 파티션, 여러 자료형, 뷰·루틴·트리거, 다른 스키마)가 들어가고, `dev/.env`가 있으면 엔진 테스트가 네 DB로도 돈다(`RealDbTest`). 확인하며 고친 것: Oracle 실제 계획 OMem·1Mem·Used-Mem이 1024배로 나오던 것(바이트였음), MySQL·MariaDB 계획의 테이블이 별칭으로 나와 객체 정보가 안 열리던 것, BIGINT UNSIGNED 최댓값·BIT(n)·YEAR 값, PostgreSQL 분할 테이블 DDL(PARTITION BY·파티션)과 프로시저 종류, Oracle DDL에 제약 인덱스가 따로 나와 다시 실행하면 오류 나던 것, PostgreSQL 자동완성에 인덱스가 테이블처럼 나오던 것, PostgreSQL 분할 테이블의 인덱스가 탐색기 객체 목록에 테이블로 나오던 것(JDBC가 relkind 'I'에 종류를 주지 않음). 앱 화면(자동완성 FK JOIN, 실행 중 표시·취소, Alt+X, 결과 편집)도 네 DB에서 확인했다.

## 그 밖의 후보

- 스키마 비교 (개발 ↔ 운영 DDL 차이)
- MSI 다시 빌드해서 설치 확인 (작업 표시줄 고정 아이콘·점프 목록)
- 코드 서명 인증서 마련 (스크립트는 `DBARO_SIGN_ARGS`로 서명할 준비가 돼 있다. 스마트 앱 제어가 켜진 PC는 서명이 없으면 실행을 막는다)
- 한국어 화면
- macOS·Linux 빌드
