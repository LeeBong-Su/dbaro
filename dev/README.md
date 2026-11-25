# dev — 개발·테스트용 DB

Docker로 Oracle, MariaDB, MySQL, PostgreSQL을 띄운다. 처음 띄울 때 샘플 스키마(`seed/`)가 자동으로 들어간다.
Tibero는 따로 설치한다.

## 시작

```bash
cp dev/.env.example dev/.env      # 비밀번호를 바꿔 둔다 (.env는 git에 올라가지 않는다)
docker compose -f dev/docker-compose.yml up -d
```

Oracle은 처음에 DB를 만들고 시드를 넣느라 1~2분 걸린다. `docker compose -f dev/docker-compose.yml logs oracle`에
`DATABASE IS READY TO USE!`가 보이면 준비된 것이다. 나머지는 수십 초면 된다.

샘플을 처음 상태로 다시 만들려면 볼륨까지 지운 뒤 다시 띄운다.

```bash
docker compose -f dev/docker-compose.yml down -v
docker compose -f dev/docker-compose.yml up -d
```

WSL 안의 Docker를 쓰는데 사용자가 `docker` 그룹에 없으면 root로 실행한다.

```powershell
wsl -u root -- docker compose -f /mnt/c/<저장소 경로>/dev/docker-compose.yml --env-file /mnt/c/<저장소 경로>/dev/.env up -d
```

## 접속 정보

| DB | 호스트:포트 | DB / 서비스 | 사용자 | 다른 스키마 |
|---|---|---|---|---|
| Oracle Free | localhost:1521 | 서비스명 `FREEPDB1` | `.env`의 `DB_USER` | `DBARO_HR` |
| MariaDB 11.4 | localhost:3306 | `dbaro` | `.env`의 `DB_USER` | `dbaro_hr` (데이터베이스) |
| MySQL 8.4 | localhost:3307 | `dbaro` | `.env`의 `DB_USER` | `dbaro_hr` (데이터베이스) |
| PostgreSQL 17 | localhost:5432 | `dbaro` | `.env`의 `DB_USER` | `hr` |

MySQL은 MariaDB와 포트가 겹치지 않게 3307로 열었다.
PostgreSQL에는 두 번째 데이터베이스 `dbaro_archive`(스키마 `archive`, 테이블 `closed_order`)도 있다.
쿼리 탭에서 DB를 바꿔 볼 때 쓴다. 그 전에 띄운 컨테이너에는 `seed/postgres/02-archive-db.sql` 첫머리의 명령으로 만든다.
DBaro에서는 위 값으로 연결을 만들고 환경을 Development로 두면 된다.

## 샘플 스키마

네 DB 모두 같은 모델이다 (`seed/oracle/schema.sql`, `seed/postgres/01-schema.sql`, `seed/mysql/sql/01-schema.sql`).

| 객체 | 내용 | 확인하는 것 |
|---|---|---|
| `dept`, `emp` | 부서 5, 사원 50. `emp.mgr_id`는 `emp`를 가리킨다 | FK JOIN 자동완성(자기 참조), 한글 값·코멘트 |
| `product`, `orders`, `order_item` | 상품 200, 주문 10만, 주문 품목 20만 | 실행 계획, 오래 걸리는 쿼리와 취소 |
| `shipment` | `(order_id, line_no)` 두 컬럼 FK로 `order_item`을 가리킨다 | 여러 컬럼 FK |
| `sales_hist` | 연도별 범위 파티션 3만 행 | 파티션 DDL, Oracle 계획의 Pstart·Pstop |
| `data_types` | DB마다 여러 자료형 (CLOB·JSON·BIT·BIGINT UNSIGNED·YEAR·배열 등), NULL 행, 8000자 값 | 값 보기 |
| `emp_dept_v`, `emp_count`, `raise_salary`, `emp_touch_trg` | 뷰, 함수, 프로시저, 트리거 (Oracle은 `order_seq` 시퀀스와 `order_api` 패키지도) | 탐색기, DDL |
| 다른 스키마의 `region`, `office` | 앱 사용자에게 권한을 준다 | 스키마 바꾸기, 다른 스키마 자동완성 |

`orders.status`는 일부러 치우쳤다 (DONE 98.9%, CANCEL 1%, NEW 0.1%). Oracle은 히스토그램 없이 통계를 모아서
`WHERE status = 'NEW'`의 예상 행 수(약 3만)와 실제 행 수(100)가 크게 다르다. 실제 실행 계획(Alt+X)에서 칠해지는지 볼 때 쓴다.

Oracle 앱 사용자는 실제 실행 계획에 필요한 V$ 뷰를 읽도록 `SELECT_CATALOG_ROLE`을 받는다.

## 엔진 통합 테스트

`dev/.env`가 있고 위 DB가 떠 있으면 엔진 테스트가 실제 DB로도 돈다 (`engine/src/test/kotlin/dbaro/engine/RealDbTest.kt`).
DB마다 메타데이터, 실행·페이지·값 변환, 바인드, 취소, DESC, 예상·실제 계획(DML 되돌림, 크게 빗나간 예상), DDL, 테이블 정보,
계획 객체 정보, 스키마 바꾸기를 확인한다. 떠 있지 않은 DB는 건너뛴다.

```bash
cd engine
./gradlew test --tests '*RealDbTest*'
```

결과(계획, DDL 등)는 `engine/build/real-db/<종류>.txt`에도 남는다. `DBARO_REAL_DB=off`면 항상 건너뛴다.

## Tibero

공식 Docker 이미지가 없어서 TmaxTibero 테크넷에서 데모 라이선스를 받아 VM이나 별도 서버에 설치한다.

JDBC 드라이버는 Tibero 클라이언트의 `client/lib/jar/` 폴더에 있다.
`dev/drivers/` 에 복사해 두면 된다. 이 폴더는 git에 올라가지 않는다 (벤더 독점 드라이버라 재배포 불가).
