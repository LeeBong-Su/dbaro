# engine — 드라이버 호스트 (Kotlin/JVM)

모든 DB 연결을 맡는 별도 프로세스다. 앱(`../app`)이 띄우고 표준 입출력으로 통신한다.
별도 프로세스라서 드라이버가 멈추거나 JVM이 GC로 잠깐 서도 앱 화면은 계속 반응한다.

| 패키지 | 역할 |
|---|---|
| `dialect/` | DB 방언 계열별 처리. `oracle`(Oracle·Tibero), `mysql`(MySQL·MariaDB), `postgres` |
| `driver/` | JDBC jar 동적 로딩, 첫 연결 때 드라이버 내려받기, 사용자 지정 jar(Tibero) |
| `exec/` | 실행·취소, 결과 페이지 단위 가져오기, 트랜잭션 |
| `meta/` | 스키마·테이블·컬럼 등 메타데이터 조회 |
| `ipc/` | 앱과의 통신 |

JDBC 드라이버는 빌드 의존성에 넣지 않는다. 라이선스 정책은 `../docs/architecture.md`에 있다.

## 빌드

JDK 21과 Gradle이 필요하다. Gradle Wrapper는 아직 없으므로 Gradle 설치 후 이 폴더에서 한 번 만든다.

```bash
gradle wrapper
./gradlew run
```

GraalVM native-image는 쓰지 않는다. 실행 중에 드라이버 jar를 불러오는 구조와 맞지 않는다.
기동 시간은 AppCDS로 줄인다.
