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
| `connect` | 연결 설정 | `sessionId`, 제품·드라이버 정보, `autoCommit` |
| `disconnect` | `sessionId` | |
| `execute` | `sessionId`, `sql`, `pageSize`(기본 500), `queryTimeoutSec` | 아래 실행 결과 |
| `fetch` | `sessionId`, `cursorId`, `count` | `rows`, `hasMore` |
| `closeCursor` | `sessionId`, `cursorId` | |
| `cancel` | `sessionId` | 실행 중인 문장을 바로 취소 (다른 요청을 기다리지 않음) |
| `commit` / `rollback` | `sessionId` | |
| `setAutoCommit` | `sessionId`, `autoCommit` | |

연결 설정: `type`(oracle, tibero, mariadb, mysql, postgres), `url`, `user`, `password`, `driverPath`?, `driverClass`?, `autoCommit`, `readOnly`, `connectTimeoutSec`, `initSql`(문장 배열).

실행 결과:

- 결과 집합: `kind: "resultSet"`, `columns: [{name, typeName, numeric}]`, `rows: [[값...]]`, `hasMore`, `cursorId`(남은 행이 있을 때)
- 그 밖: `kind: "update"`, `updateCount`
- 공통: `elapsedMs`, `warnings`(SQLWarning, PostgreSQL NOTICE 등), `output`(Oracle·Tibero의 DBMS_OUTPUT)

값: 정수 → 정수, 실수 → double, 불리언, NULL, NUMERIC/DECIMAL → 문자열(정밀도 유지), 날짜·시간 → `yyyy-MM-dd HH:mm:ss[.f]` 문자열, 바이너리 → 바이트열(최대 64KB), 긴 문자열·CLOB → 최대 64K 글자.

## 동시성

- 엔진은 읽기 스레드 하나가 요청을 받아 나눠 준다.
- 세션마다 전용 스레드가 있어서 한 세션의 작업은 순서대로, 세션끼리는 동시에 실행된다.
- `cancel`은 읽기 스레드에서 바로 `Statement.cancel()`을 부른다.

## 이후 검토

결과가 아주 큰 경우 CBOR 행 배열이 병목이 되면 Arrow IPC 같은 컬럼 단위 형식을 검토한다.
