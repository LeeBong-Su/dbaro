# dev — 개발·테스트용 DB

Docker로 Oracle, MariaDB, MySQL, PostgreSQL을 띄운다. Tibero는 따로 설치한다.

## 시작

```bash
cp dev/.env.example dev/.env
docker compose -f dev/docker-compose.yml up -d
```

## 접속 정보

| DB | 호스트:포트 | DB / 서비스 | 사용자 |
|---|---|---|---|
| Oracle Free | localhost:1521 | 서비스명 `FREEPDB1` | `.env`의 `DB_USER` |
| MariaDB 11.4 | localhost:3306 | `dbaro` | `.env`의 `DB_USER` |
| MySQL 8.4 | localhost:3307 | `dbaro` | `.env`의 `DB_USER` |
| PostgreSQL 17 | localhost:5432 | `dbaro` | `.env`의 `DB_USER` |

MySQL은 MariaDB와 포트가 겹치지 않게 3307로 열었다.

## Tibero

공식 Docker 이미지가 없어서 TmaxTibero 테크넷에서 데모 라이선스를 받아 VM이나 별도 서버에 설치한다.

JDBC 드라이버는 Tibero 클라이언트의 `client/lib/jar/` 폴더에 있다.
`dev/drivers/` 에 복사해 두면 된다. 이 폴더는 git에 올라가지 않는다 (벤더 독점 드라이버라 재배포 불가).
