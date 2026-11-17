# DBaro 아키텍처

DBaro는 Qt 6(C++) 앱과 Kotlin/JVM 엔진, 두 프로세스로 동작한다.
기준은 개발 속도가 아니라 제품 완성도다.

```
[app: Qt 6 / C++]   SQL 에디터 · 결과 그리드 · 객체 탐색기 · SQL 파서와 자동완성 · 메타데이터 캐시
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

| DB | 드라이버 | 라이선스 | 정책 |
|---|---|---|---|
| Oracle | ojdbc11 | Oracle 무료 이용 약관(FUTC) | 내려받기 |
| MariaDB | MariaDB Connector/J | LGPL 2.1 | 내려받기 |
| MySQL | MySQL Connector/J | GPLv2 + FOSS 예외 | 내려받기 (함께 배포하지 않음) |
| PostgreSQL | pgJDBC | BSD-2 | 내려받기 |
| Tibero | `tibero7-jdbc.jar` | 벤더 독점 | 사용자가 jar 경로 지정 |

## 실행 흐름

1. 쿼리 탭에서 실행하면 앱이 실행 전 안전 확인을 한다 (`app/src/core/SqlSafety`).
   - 읽기 전용 연결에서는 데이터·구조를 바꾸는 문장을 막는다.
   - WHERE 없는 UPDATE·DELETE, 운영 연결의 DROP·TRUNCATE는 확인 대화상자를 띄운다.
2. 탭에 세션이 없으면 비밀번호(OS 보안 저장소 또는 입력)로 엔진에 연결한다. 탭마다 별도 연결이다.
3. 문장을 차례로 실행하고, 오류가 나면 멈추고 에디터 커서를 오류 위치로 옮긴다.
4. 결과는 500행씩 받는다. 표를 끝까지 스크롤하면 다음 페이지를 가져온다.
5. 수동 커밋 모드에서 DML을 실행하면 "커밋 안 된 변경"으로 표시한다. 탭을 닫거나 연결을 바꾸거나 앱을 끌 때 확인을 받고, 끊을 때는 롤백한다.

메시지 형식과 메서드는 `../protocol/README.md`에 있다.

## 연결 정보 저장

| 항목 | 위치 | 비고 |
|---|---|---|
| 연결 목록 | OS 앱 데이터 폴더의 `connections.json` (Windows: `%APPDATA%\DBaro\DBaro\connections.json`) | 비밀번호는 넣지 않는다 |
| 비밀번호 | OS 보안 저장소. Windows 자격 증명 관리자, 이름 `DBaro/connection/<연결 id>` | macOS 키체인, Linux libsecret은 아직 없음. 그 OS에서는 접속할 때마다 묻는다 |

연결마다 환경(개발, 테스트, 스테이징, 운영)을 지정한다. 환경 색상은 툴바, 탐색기, 쿼리 탭 위쪽 띠에 표시되고, 운영 연결은 상태 표시줄에 `PRODUCTION`으로 나온다.

## 성능 기준

목표 수치는 `../bench/README.md`에 있다. CI에서 매번 측정한다.

## 아직 정하지 않은 것

- 결과가 아주 클 때의 전송 형식 (지금은 CBOR 행 배열. 병목이 확인되면 Arrow IPC 등 검토)
- 자동완성용 파서 방식: tree-sitter 기반 또는 직접 작성 (입력 중인 잘못된 SQL을 견디고, 바뀐 부분만 다시 분석해야 함)

## 화면과 테마

배치는 IntelliJ 계열 IDE의 사용 방식을 참고하되, 아이콘과 색상은 DBaro 자체로 만든다.

- 메인 툴바: 왼쪽은 연결 선택, 오른쪽은 실행·중지·트랜잭션
- 창 왼쪽 끝의 도구 창 막대: 도구 창(Database Explorer 등)을 열고 닫는다
- 쿼리 탭: 닫기 버튼은 선택된 탭과 마우스를 올린 탭에만 보이고, 가운데 클릭으로도 닫힌다
- 테마(`app/src/ui/Theme`): OS와 상관없이 같은 모습이 되도록 Fusion 스타일에 자체 색상을 입힌다. 기본은 다크 그레이, View → Theme에서 라이트로 바꿀 수 있고 선택은 저장된다. Windows 제목 표시줄도 테마 밝기를 따른다
- 아이콘(`app/resources/icons`): 16×16 SVG. `#ICON` 색은 그릴 때마다 현재 테마의 아이콘 색으로 바뀐다

## SQL 에디터

`QPlainTextEdit`를 확장한 자체 컴포넌트(`app/src/ui/SqlEditor`)다.
KTextEditor는 KDE 프레임워크 의존이 무겁고, QScintilla는 GPL이라 쓰지 않았다.
에디터 API를 DBaro가 갖고 있으므로, 나중에 렌더링까지 자체 구현으로 바꿔도 다른 코드는 바뀌지 않는다.

- 문장 분리(`app/src/core/SqlSplitter`)는 직접 작성한 스캐너다. 문자열·인용 식별자·주석 안의 구분자를 무시하고, 방언별 규칙(Oracle `/` 줄과 PL/SQL 블록, q'[...]', MySQL `DELIMITER`, PostgreSQL `$$`)을 따른다.
- 세로 커서는 줄마다 `QTextCursor`를 하나씩 두는 방식이다. 조합이 끝난 한글은 모든 커서에 들어가고, 조합 중인 글자는 주 커서에만 보인다.
- Qt 라이선스: LGPLv3 동적 링크 유지 또는 상용 라이선스
