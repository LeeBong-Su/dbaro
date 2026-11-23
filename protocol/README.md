# protocol — 앱과 엔진 사이 통신 규약

## 전송 통로: 표준 입출력

앱(`app/src/ipc/EngineClient`)이 `QProcess`로 엔진을 띄우고 엔진의 표준 입력·출력으로 메시지를 주고받는다.
VS Code와 언어 서버(LSP)가 쓰는 방식과 같다.

- 포트를 열지 않으므로 방화벽과 보안 문제가 없다.
- Windows에서 Qt의 `QLocalSocket`은 named pipe를 쓰고, Java의 로컬 소켓은 AF_UNIX라 서로 바로 맞지 않는다.
  표준 입출력은 모든 OS에서 같은 방식으로 동작한다.
- 엔진의 표준 출력은 프로토콜 전용이다. 로그는 표준 오류로 보낸다. 엔진은 시작할 때 `System.out`을 표준 오류로 돌려서 드라이버가 출력해도 프로토콜이 깨지지 않게 한다.
- 앱이 표준 입력을 닫으면 엔진은 열린 세션을 롤백·종료하고 끝난다.

## 메시지 형식

메시지 하나 = **4바이트 길이(빅엔디언) + CBOR(RFC 8949) 맵**.
CBOR는 JSON처럼 다루기 쉬운 바이너리 형식이다. Qt에 기본으로 들어 있고(`QCborValue`), 엔진 쪽은 필요한 부분만 직접 구현했다(`engine/.../ipc/Cbor.kt`).

```
요청: { id: 정수, method: 문자열, params: 맵 }
응답: { id, result: 맵 }  또는  { id, error: { message, sqlState?, vendorCode?, position?, line? } }
```

- 응답은 요청 순서와 다르게 올 수 있다. `id`로 짝을 맞춘다.
- `error.position`은 실행한 문장 안의 0부터 시작하는 글자 위치(PostgreSQL, Oracle), `error.line`은 1부터 시작하는 줄(MySQL, MariaDB)이다. 앱은 이 위치로 에디터 커서를 옮긴다.

## 메서드

| 메서드 | params | result |
|---|---|---|
| `hello` | | `version`, `java` |
| `test` | 연결 설정 (아래) | `product`, `productVersion`, `driverName`, `driverVersion`, `elapsedMs` |
| `connect` | 연결 설정 | `sessionId`, 제품·드라이버 정보, `autoCommit`, `schema`(지금 스키마), `schemaError`(`schema`로 바꾸지 못했을 때. 접속은 그대로) |
| `disconnect` | `sessionId` | |
| `execute` | `sessionId`, `sql`, `pageSize`(기본 500), `queryTimeoutSec`, `binds`?: `[{value, type, out}]` | 아래 실행 결과. `binds`가 있으면 `sql`의 `?` 자리에 차례로 넣는다(`type`: VARCHAR, NUMERIC, CLOB). `out`이 있으면 CallableStatement로 실행해 그 자리를 OUT 파라미터로 받고, 결과 표 없이 `outBinds: [값...]`(OUT이 아닌 자리는 null)를 돌려준다 (CLI의 VARIABLE·EXEC) |
| `fetch` | `sessionId`, `cursorId`, `count` | `rows`, `hasMore`, `elapsedMs`(이번 페이지를 읽은 시간) |
| `explain` | `sessionId`, `sql` | 예상 실행 계획 (문장은 실행하지 않음). 아래 실행 계획 |
| `ddl` | `sessionId`, `schema`(없으면 `currentSchema`, 그것도 없으면 현재 스키마), `name`, `schemaQuoted`, `nameQuoted`, `currentSchema`(쿼리 탭에서 바꾼 스키마) | `schema`, `name`, `type`, `script`. 아래 DDL |
| `tableInfo` | `sessionId`, `schema`(없으면 `currentSchema`, 그것도 없으면 현재 스키마), `name`, `schemaQuoted`, `nameQuoted`, `currentSchema` | `schema`, `name`, `type`, `remarks`, `columns: [{name, type, typeName, numeric, temporal, nullable, primaryKey, default, remarks}]`, `primaryKey: [컬럼]`, `indexes: [{name, unique, columns}]`, `foreignKeys: [{name, columns, refSchema, refTable, refColumns}]`, `referencedBy: [{name, schema, table, columns, refColumns}]`. 이름은 DB 규칙대로 찾고(Oracle·Tibero는 시노님도 따라감) JDBC 메타데이터로 읽는다 |
| `executeBatch` | `sessionId`, `statements: [문장]`, `stopOnError`(기본 true) | `executed`, `failed`, `skipped`, `affected`, `stoppedAt`, `cancelled`, `errors: [{index, message}]`(최대 100), `elapsedMs`. 문장을 하나씩 실행한다(실패 지점을 정확히 알리려고 JDBC 배치는 쓰지 않음). `cancel`을 받으면 다음 문장 앞에서 멈춘다. PostgreSQL 수동 커밋에서 계속하기면 문장마다 세이브포인트로 트랜잭션을 지킨다 |
| `planObject` | `sessionId`, `name`, `schema`?, `type`?(TABLE, INDEX ...), `index`?(쓴 인덱스), `currentSchema`? | `table`(`tableInfo` 결과), `index`, `objectType`, `tableStats: [[이름, 값]...]`, `indexStats: {인덱스: [[이름, 값]...]}`. 실행 계획 단계의 객체 정보. INDEX면 그 인덱스의 테이블을 찾는다(Oracle·Tibero `ALL_INDEXES`, PostgreSQL `pg_index`, MySQL `STATISTICS`). 통계는 DB에 있는 것만 |
| `currentSchema` | `sessionId` | `schema`: 그 세션의 지금 스키마 (Oracle·Tibero `CURRENT_SCHEMA`, PostgreSQL `current_schema()`, MySQL `DATABASE()`) |
| `setSchema` | `sessionId`, `schema`(정확한 이름) | `schema`(바꾼 뒤), `sql`(실행한 문장). Oracle·Tibero `ALTER SESSION SET CURRENT_SCHEMA`, PostgreSQL `SET search_path TO 스키마, public`, MySQL·MariaDB `USE`, 그 밖은 JDBC `setSchema` |
| `closeCursor` | `sessionId`, `cursorId` | |
| `cancel` | `sessionId` | 실행 중인 문장을 바로 취소 (다른 요청을 기다리지 않음) |
| `commit` / `rollback` | `sessionId` | |
| `setAutoCommit` | `sessionId`, `autoCommit` | |
| `metaSchemas` | `sessionId` | `names`(정렬된 스키마 이름, MySQL·MariaDB는 데이터베이스), `current`(지금 접속한 스키마) |
| `metaObjects` | `sessionId`, `schema` | `objects`: `name`, `type`(드라이버가 주는 TABLE, VIEW, ... 와 FUNCTION, PROCEDURE), `remarks` / `truncated`(2만 개 초과) |
| `metaColumns` | `sessionId`, `schema`, `table` | `columns`: `name`, `type`(예: `VARCHAR2(40)`, `NUMBER(10,2)`), `nullable`, `primaryKey`, `default`, `remarks` |

연결 설정: `type`(oracle, tibero, mariadb, mysql, postgres), `url`, `user`, `password`, `driverPath`?, `driverClass`?, `autoCommit`, `readOnly`, `connectTimeoutSec`, `initSql`(문장 배열), `schema`?(접속 직후 `setSchema`처럼 바꿀 스키마. PostgreSQL 수동 커밋이면 바로 커밋해서 롤백해도 돌아가지 않게 한다).

실행 결과:

- 결과 집합: `kind: "resultSet"`, `columns: [{name, typeName, numeric}]`, `rows: [[값...]]`, `hasMore`, `cursorId`(남은 행이 있을 때)
- 그 밖: `kind: "update"`, `updateCount`
- 공통: `elapsedMs`(전체), `executeMs`(문장 실행까지. 나머지는 첫 페이지를 읽은 시간), `warnings`(SQLWarning, PostgreSQL NOTICE 등), `output`(Oracle·Tibero의 DBMS_OUTPUT)

DESC (`execute`의 `sql`이 `DESC[RIBE] [schema.]object`일 때): SQL*Plus·psql의 클라이언트 명령이라 Oracle·Tibero·PostgreSQL 서버는 모른다. 엔진이 서버로 보내지 않고 딕셔너리를 조회해 결과 집합(`hasMore: false`, 커서 없음)으로 돌려준다. MySQL·MariaDB의 DESC는 서버 SQL이라 그대로 보낸다.

- Oracle·Tibero: 테이블·뷰·구체화 뷰는 `Name`, `Null?`, `Type`(SQL*Plus 표기, 예: `VARCHAR2(20 CHAR)`, `NUMBER(9,2)`), `Comment` (`ALL_TAB_COLUMNS`, `ALL_COL_COMMENTS`). 프로시저·함수는 `Argument`, `Type`, `In/Out`(반환값은 `RETURN`), `Default?`, 패키지는 앞에 `Subprogram`, `Overload`를 붙인다 (`ALL_ARGUMENTS`). 객체 타입은 속성(`ALL_TYPE_ATTRS`), 컬렉션 타입은 요소 타입(`ALL_COLL_TYPES`). 스키마가 없으면 현재 스키마 → 자기 시노님 → PUBLIC 시노님 순서로 찾고 시노님은 끝까지 따라간다 (DB 링크 시노님은 지원하지 않음)
- PostgreSQL: 테이블·뷰·구체화 뷰·외부 테이블·복합 타입은 `Name`, `Null?`, `Type`(`format_type`), `Default`(IDENTITY, 생성 컬럼 포함), `Comment`. 없으면 함수·프로시저를 오버로드마다 `Function`, `Arguments`, `Returns`, `Kind`로. 스키마가 없으면 search_path에서 보이는 객체 (psql `\d`와 같음). 트랜잭션 중이면 세이브포인트 안에서 조회해 실패해도 트랜잭션이 막히지 않는다
- 딕셔너리 컬럼은 DB 버전마다 달라서 있는 컬럼만 골라 조회한다

DDL (`ddl`): 객체 본문에 코멘트, 인덱스, 트리거, 그 객체를 가리키는 시노님, 권한 부여를 구역별 주석과 함께 붙인다. 따옴표로 감싸지 않은 이름은 DB 규칙에 맞춘다(Oracle 대문자, PostgreSQL 소문자, MySQL은 대소문자 무시).

- Oracle·Tibero: `DBMS_METADATA.GET_DDL`과 `GET_DEPENDENT_DDL`(COMMENT, INDEX, TRIGGER, OBJECT_GRANT), 시노님은 `ALL_SYNONYMS`에서 만든다. 권한이 없거나 지원하지 않으면 `ALL_TAB_COLUMNS`(JDBC), `ALL_VIEWS`, `ALL_SOURCE`, `ALL_TAB_COMMENTS`, `ALL_COL_COMMENTS`, `ALL_TAB_PRIVS`로 직접 만든다. 이름이 객체가 아니면 시노님(자기 스키마 → PUBLIC)으로 보고 가리키는 객체를 보여 준다
- MySQL·MariaDB: `SHOW CREATE TABLE/VIEW/PROCEDURE/FUNCTION/TRIGGER`(루틴·트리거는 `DELIMITER $$`로 감쌈), `information_schema.TABLE_PRIVILEGES`
- PostgreSQL: 카탈로그로 직접 만든다 (컬럼·기본값·IDENTITY, 제약 조건, 인덱스, 소유자, 코멘트, 권한, 트리거, 뷰·구체화 뷰·시퀀스·함수)
- 그 밖: JDBC 메타데이터 (컬럼, 기본 키, 인덱스, 코멘트, 권한, INFORMATION_SCHEMA.SYNONYMS)

실행 계획 (`explain`):

- `columns`: 표 머리글, `nodes: [{id, parent, cells, object}]`(`parent`가 -1이면 맨 위), `text`(DB 고유 형식의 계획 글, 없을 수 있음), `elapsedMs`
- `object`: 그 단계가 읽는 객체 `{schema, name, type, index}` 또는 null. Oracle·Tibero는 `OBJECT_OWNER`·`OBJECT_NAME`·`OBJECT_TYPE`(TABLE, INDEX), PostgreSQL은 `Relation Name`(+`Index Name`) 또는 `Index Name`만(Bitmap Index Scan), MySQL은 `table`(별칭일 수 있음, `<derived2>` 같은 임시 결과는 제외)과 `key`
- Oracle·Tibero: `EXPLAIN PLAN SET STATEMENT_ID = ... FOR`로 PLAN_TABLE에 넣고 읽은 뒤 지운다. Oracle은 `DBMS_XPLAN.DISPLAY` 글도 함께
- PostgreSQL: `EXPLAIN (FORMAT JSON)`을 트리로, 기본 `EXPLAIN` 글도 함께
- MySQL·MariaDB와 그 밖: `EXPLAIN` 결과 표 그대로 (MySQL 8은 `EXPLAIN FORMAT=TREE` 글도 함께). 결과가 한 컬럼뿐이면 글로 준다

값: 정수 → 정수, 실수 → double, 불리언, NULL, NUMERIC/DECIMAL → 문자열(정밀도 유지), 날짜·시간 → `yyyy-MM-dd HH:mm:ss[.f]` 문자열, 바이너리 → 바이트열(최대 64KB), 긴 문자열·CLOB → 최대 64K 글자.

## 동시성

- 엔진은 읽기 스레드 하나가 요청을 받아 나눠 준다.
- 세션마다 전용 스레드가 있어서 한 세션의 작업은 순서대로, 세션끼리는 동시에 실행된다.
- `cancel`은 읽기 스레드에서 바로 `Statement.cancel()`을 부른다.

## 이후 검토

결과가 아주 큰 경우 CBOR 행 배열이 병목이 되면 Arrow IPC 같은 컬럼 단위 형식을 검토한다.
