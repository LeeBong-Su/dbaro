-- 같은 서버의 두 번째 데이터베이스 (PostgreSQL). 쿼리 탭에서 DB를 바꾸는 것을 확인할 때 쓴다.
-- 01-schema.sql 다음에 POSTGRES_USER로 실행된다. 이미 띄운 컨테이너에는 직접 실행한다:
--   docker compose exec postgres sh -c 'psql -U "$POSTGRES_USER" -d dbaro -f /docker-entrypoint-initdb.d/02-archive-db.sql'

CREATE DATABASE dbaro_archive;
\connect dbaro_archive

CREATE SCHEMA archive;

CREATE TABLE archive.closed_order (
    order_id   integer       CONSTRAINT closed_order_pk PRIMARY KEY,
    closed_on  date          NOT NULL,
    amount     numeric(12, 2),
    note       varchar(200)
);
COMMENT ON TABLE archive.closed_order IS '마감한 주문';

INSERT INTO archive.closed_order
SELECT n, DATE '2020-01-01' + (n % 1000), (n % 97) * 10.5, '마감 ' || n
  FROM generate_series(1, 500) AS n;

CREATE TABLE public.archive_log (
    log_id    integer      CONSTRAINT archive_log_pk PRIMARY KEY,
    logged_at timestamp    NOT NULL DEFAULT now(),
    message   varchar(200)
);
INSERT INTO public.archive_log (log_id, message) VALUES (1, '보관 DB를 만들었다');
