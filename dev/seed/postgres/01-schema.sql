-- DBaro 개발용 샘플 스키마 (PostgreSQL). 공식 이미지가 처음 기동할 때 POSTGRES_USER로 POSTGRES_DB에서 실행한다.
-- 모델은 dev/seed/oracle/schema.sql 과 같다. 다른 스키마(hr)도 하나 만든다.

CREATE TABLE dept (
    dept_id   integer     CONSTRAINT dept_pk PRIMARY KEY,
    dept_name varchar(50) NOT NULL,
    location  varchar(50)
);
COMMENT ON TABLE dept IS '부서';
COMMENT ON COLUMN dept.dept_name IS '부서 이름';

CREATE TABLE emp (
    emp_id     integer       CONSTRAINT emp_pk PRIMARY KEY,
    emp_name   varchar(50)   NOT NULL,
    email      varchar(100)  CONSTRAINT emp_email_uk UNIQUE,
    dept_id    integer       CONSTRAINT emp_dept_fk REFERENCES dept,
    mgr_id     integer       CONSTRAINT emp_mgr_fk REFERENCES emp,
    job        varchar(20),
    salary     numeric(10, 2),
    hired_on   date          NOT NULL DEFAULT current_date,
    updated_on timestamp
);
CREATE INDEX emp_dept_ix ON emp (dept_id);
CREATE INDEX emp_mgr_ix ON emp (mgr_id);
COMMENT ON TABLE emp IS '사원';
COMMENT ON COLUMN emp.emp_name IS '사원 이름';
COMMENT ON COLUMN emp.mgr_id IS '관리자 사원 번호';

CREATE TABLE product (
    product_id   integer       CONSTRAINT product_pk PRIMARY KEY,
    product_name varchar(100)  NOT NULL,
    category     varchar(30),
    price        numeric(10, 2)
);
COMMENT ON TABLE product IS '상품';

CREATE TABLE orders (
    order_id   bigint        CONSTRAINT orders_pk PRIMARY KEY,
    emp_id     integer       CONSTRAINT orders_emp_fk REFERENCES emp,
    ordered_on date          NOT NULL,
    status     varchar(10)   NOT NULL CONSTRAINT orders_status_ck CHECK (status IN ('NEW', 'DONE', 'CANCEL')),
    amount     numeric(12, 2)
);
CREATE INDEX orders_emp_ix ON orders (emp_id);
CREATE INDEX orders_status_ix ON orders (status);
CREATE INDEX orders_date_ix ON orders (ordered_on);
COMMENT ON TABLE orders IS '주문';

CREATE TABLE order_item (
    order_id   bigint         CONSTRAINT order_item_order_fk REFERENCES orders,
    line_no    smallint,
    product_id integer        NOT NULL CONSTRAINT order_item_product_fk REFERENCES product,
    qty        integer        NOT NULL,
    price      numeric(10, 2) NOT NULL,
    CONSTRAINT order_item_pk PRIMARY KEY (order_id, line_no)
);
CREATE INDEX order_item_product_ix ON order_item (product_id);
COMMENT ON TABLE order_item IS '주문 품목';

CREATE TABLE shipment (
    shipment_id bigint   CONSTRAINT shipment_pk PRIMARY KEY,
    order_id    bigint   NOT NULL,
    line_no     smallint NOT NULL,
    shipped_on  date,
    CONSTRAINT shipment_item_fk FOREIGN KEY (order_id, line_no) REFERENCES order_item (order_id, line_no)
);
COMMENT ON TABLE shipment IS '배송 (주문 품목을 두 컬럼 FK로 참조)';

CREATE SEQUENCE order_seq START WITH 100001;

CREATE TABLE sales_hist (
    sale_id    bigint  NOT NULL,
    sold_on    date    NOT NULL,
    product_id integer,
    amount     numeric(12, 2)
) PARTITION BY RANGE (sold_on);
CREATE TABLE sales_hist_2023 PARTITION OF sales_hist FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
CREATE TABLE sales_hist_2024 PARTITION OF sales_hist FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE sales_hist_2025 PARTITION OF sales_hist FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE TABLE sales_hist_rest PARTITION OF sales_hist DEFAULT;
CREATE INDEX sales_hist_date_ix ON sales_hist (sold_on);
COMMENT ON TABLE sales_hist IS '판매 이력 (연도별 파티션)';

CREATE TABLE data_types (
    id            integer CONSTRAINT data_types_pk PRIMARY KEY,
    c_varchar     varchar(100),
    c_text        text,
    c_char        char(5),
    c_smallint    smallint,
    c_bigint      bigint,
    c_numeric     numeric(12, 4),
    c_real        real,
    c_double      double precision,
    c_boolean     boolean,
    c_date        date,
    c_time        time,
    c_timestamp   timestamp(6),
    c_timestamptz timestamptz,
    c_interval    interval,
    c_bytea       bytea,
    c_uuid        uuid,
    c_json        json,
    c_jsonb       jsonb,
    c_int_array   integer[],
    c_inet        inet
);
COMMENT ON TABLE data_types IS '여러 자료형 값 보기 확인용';

INSERT INTO dept VALUES (10, '영업', '서울'), (20, '개발', '판교'), (30, '인사', '서울'), (40, '재무', '부산'), (50, '물류', '인천');

INSERT INTO emp (emp_id, emp_name, email, dept_id, job, salary, hired_on)
SELECT n,
       '사원' || lpad(n::text, 3, '0'),
       'emp' || n || '@example.com',
       ((n - 1) % 5 + 1) * 10,
       CASE WHEN n = 1 THEN 'CEO' WHEN n <= 5 THEN 'MANAGER' ELSE 'STAFF' END,
       3000 + (n * 37) % 50 * 100,
       date '2015-01-01' + n * 30
FROM generate_series(1, 50) n;
UPDATE emp SET mgr_id = CASE WHEN emp_id <= 5 THEN 1 ELSE emp_id % 4 + 2 END WHERE emp_id > 1;

INSERT INTO product
SELECT n,
       '상품 ' || n,
       CASE n % 4 WHEN 0 THEN '전자' WHEN 1 THEN '도서' WHEN 2 THEN '의류' ELSE '식품' END,
       1000 + (n * 53) % 100 * 500
FROM generate_series(1, 200) n;

INSERT INTO orders
SELECT n,
       n % 50 + 1,
       date '2024-01-01' + (n % 700)::integer,
       CASE WHEN n % 1000 = 0 THEN 'NEW' WHEN n % 100 = 1 THEN 'CANCEL' ELSE 'DONE' END,
       (n % 997) * 10.5
FROM generate_series(1, 100000) n;

INSERT INTO order_item
SELECT o.order_id, l.line_no, (o.order_id * 7 + l.line_no) % 200 + 1, o.order_id % 5 + 1, 1000 + o.order_id % 50 * 100
FROM orders o CROSS JOIN generate_series(1, 2) l(line_no);

INSERT INTO shipment
SELECT order_id, order_id, 1, ordered_on + 2 FROM orders WHERE order_id <= 1000 AND status = 'DONE';

INSERT INTO sales_hist
SELECT n, date '2023-01-01' + (n % 1095)::integer, n % 200 + 1, (n % 991) * 7
FROM generate_series(1, 30000) n;

INSERT INTO data_types VALUES (
    1, '가나다 abc', '긴 글 text', 'AB', 32767, 9223372036854775807, 1234.5678, 3.14, 3.14159265358979, true,
    '2024-05-06', '07:08:09', '2024-05-06 07:08:09.123456', '2024-05-06 07:08:09.123456+09', '1 day 02:03:04.5',
    '\xdeadbeef', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', '{"name": "DBaro", "tags": [1, 2]}', '{"name": "DBaro", "tags": [1, 2]}',
    '{1,2,3}', '192.168.0.1/24');
INSERT INTO data_types (id) VALUES (2);
INSERT INTO data_types (id, c_varchar, c_text, c_numeric, c_boolean) VALUES (3, repeat('x', 100), repeat('a', 4000) || repeat('b', 4000), -0.0001, false);

CREATE VIEW emp_dept_v AS
SELECT e.emp_id, e.emp_name, d.dept_name, m.emp_name AS mgr_name
FROM emp e
JOIN dept d ON d.dept_id = e.dept_id
LEFT JOIN emp m ON m.emp_id = e.mgr_id;

CREATE FUNCTION emp_count(p_dept_id integer) RETURNS bigint
LANGUAGE sql STABLE
AS $$
    SELECT count(*) FROM emp WHERE dept_id = p_dept_id
$$;

CREATE PROCEDURE raise_salary(p_dept_id integer, p_percent numeric)
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE emp SET salary = salary * (1 + p_percent / 100) WHERE dept_id = p_dept_id;
END;
$$;

CREATE FUNCTION emp_touch() RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    NEW.updated_on := now();
    RETURN NEW;
END;
$$;

CREATE TRIGGER emp_touch_trg BEFORE UPDATE ON emp FOR EACH ROW EXECUTE FUNCTION emp_touch();

-- 다른 스키마
CREATE SCHEMA hr;
CREATE TABLE hr.region (
    region_id   integer     CONSTRAINT region_pk PRIMARY KEY,
    region_name varchar(50) NOT NULL
);
COMMENT ON TABLE hr.region IS '지역';
CREATE TABLE hr.office (
    office_id   integer     CONSTRAINT office_pk PRIMARY KEY,
    region_id   integer     NOT NULL CONSTRAINT office_region_fk REFERENCES hr.region,
    office_name varchar(50) NOT NULL
);
COMMENT ON TABLE hr.office IS '사무소';
INSERT INTO hr.region VALUES (1, '수도권'), (2, '영남'), (3, '호남');
INSERT INTO hr.office VALUES (1, 1, '서울 본사'), (2, 1, '판교 연구소'), (3, 2, '부산 지사'), (4, 3, '광주 지사');

ANALYZE;
