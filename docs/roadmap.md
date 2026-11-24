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

## 2차개선

- [ ] 리버스 엔지니어링 (DB 구조 → 모델·ERD·DDL 스크립트)
- [x] 외래 키(FK) 기반 JOIN 자동 완성 (`JOIN dept d ON ` 뒤에 `d.DEPT_ID = e.DEPT_ID`, `JOIN ` 뒤에 `DEPT d ON ...` 처럼 이어진 테이블까지. 양쪽 방향·여러 컬럼 키)
- [ ] 이름 바꾸기 리팩토링: 테이블·컬럼 이름을 바꿀 때 그 객체를 참조하는 SQL 파일·뷰·프로시저를 찾아 함께 고친다
- [ ] AI 에이전트형 DB 도구 (AI 모델 연결)
- [x] 실제 실행 계획 (실행해서 얻은 계획과 통계), 단축키 Alt+X: Oracle `V$SQL_PLAN_STATISTICS_ALL`·`DISPLAY_CURSOR ADVANCED ALLSTATS LAST`, PostgreSQL `EXPLAIN ANALYZE`, MySQL `EXPLAIN ANALYZE`, MariaDB `ANALYZE`. DML은 실행 후 되돌림, 예상과 크게 다른 행 수는 칠함, Oracle은 ADVANCED ALLSTATS LAST 글을 먼저 보임. Tibero는 아직

## 그 밖의 후보

- 세션·락 모니터, 세션 종료 (V$SESSION, pg_stat_activity, processlist)
- 테이블스페이스·DB 크기 사용량
- 스키마 비교 (개발 ↔ 운영 DDL 차이)
- 실제 DB(Oracle Free, PostgreSQL, MariaDB 컨테이너)로 확인하는 환경
- MSI 다시 빌드해서 설치 확인 (작업 표시줄 고정 아이콘·점프 목록)
- 한국어 화면
- macOS·Linux 빌드
