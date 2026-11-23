# engine — 드라이버 호스트 (Kotlin/JVM)

모든 DB 연결을 맡는 별도 프로세스다. 앱(`../app`)이 띄우고 표준 입출력으로 통신한다 (`../protocol/README.md`).
별도 프로세스라서 드라이버가 멈추거나 JVM이 GC로 잠깐 서도 앱 화면은 계속 반응한다.

| 패키지 | 역할 |
|---|---|
| `ipc/` | CBOR 인코딩, 메시지 프레임, 요청 처리 서버 |
| `driver/` | JDBC 드라이버 동적 로딩, Maven Central에서 내려받기(SHA-1 확인), 사용자 지정 jar(Tibero) |
| `exec/` | 연결, 실행·취소, 결과 페이지, 트랜잭션, DBMS_OUTPUT, 값 변환, 오류 위치, DDL·DESC·실행 계획, 테이블 정보, 계획 객체 정보(`PlanObject`), 스키마 바꾸기 |
| `dialect/` | DB 방언 계열별 처리 (이후 메타데이터 조회에 쓴다) |

## 드라이버

| 종류 | 드라이버 | 얻는 방법 |
|---|---|---|
| oracle | `com.oracle.database.jdbc:ojdbc11:23.4.0.24.05` | 첫 연결 때 내려받음 |
| mariadb | `org.mariadb.jdbc:mariadb-java-client:3.4.1` | 첫 연결 때 내려받음 |
| mysql | `com.mysql:mysql-connector-j:8.4.0` | 첫 연결 때 내려받음 (GPL이라 함께 배포하지 않음) |
| postgres | `org.postgresql:postgresql:42.7.4` | 첫 연결 때 내려받음 |
| tibero | `com.tmax.tibero.jdbc.TbDriver` | 연결 설정에서 jar 지정 (벤더 독점) |

내려받은 jar는 앱이 넘겨 준 폴더(`--driver-dir`, 기본 `%APPDATA%\DBaro\DBaro\drivers`)에 저장한다.
Windows에서는 OS 인증서 저장소로 HTTPS를 검증해서 회사 HTTPS 검사 프록시 환경에서도 받아진다.

## 빌드와 실행

JDK 21이 필요하다. Gradle은 따로 설치하지 않는다. Wrapper(`gradlew`)가 Gradle 8.14.3을 내려받아 쓴다.
Gradle 8.14는 JDK 24보다 새로운 JDK에서 실행되지 않는다. `JAVA_HOME`이 JDK 21을 가리키게 둔다.

```bash
./gradlew test          # 단위 테스트 (H2 내장 DB로 실행·페이지·취소·트랜잭션 확인)
./gradlew installDist   # 앱이 쓰는 실행 파일: build/install/dbaro-engine/lib/*.jar
```

앱은 개발 중에 저장소의 `engine/build/install/dbaro-engine`을 자동으로 찾는다. 다른 위치면 `DBARO_ENGINE_HOME`을 지정한다.

GraalVM native-image는 쓰지 않는다. 실행 중에 드라이버 jar를 불러오는 구조와 맞지 않는다.
기동 시간은 AppCDS로 줄인다.
