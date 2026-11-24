-- DBaro 개발용 샘플 스키마 (Oracle). 앱 사용자로 접속해서 실행한다 (init.sh).
-- 다른 DB의 샘플(dev/seed/postgres, dev/seed/mysql)과 같은 모델:
--   dept ─< emp (mgr_id → emp 자기 참조) ─< orders ─< order_item >─ product
--   order_item ─< shipment (두 컬럼 FK)
--   orders.status 는 일부러 치우쳤다 (DONE 98.9%, CANCEL 1%, NEW 0.1%). 히스토그램 없이 통계를 모아서
--   WHERE status = 'NEW' 의 예상 행 수(약 3만)와 실제 행 수(100)가 크게 다르다 (실제 실행 계획에서 칠해지는지 확인).
SET DEFINE OFF
SET FEEDBACK OFF

PROMPT tables
CREATE TABLE dept (
    dept_id   NUMBER(4)    CONSTRAINT dept_pk PRIMARY KEY,
    dept_name VARCHAR2(50) NOT NULL,
    location  VARCHAR2(50)
);
COMMENT ON TABLE dept IS '부서';
COMMENT ON COLUMN dept.dept_name IS '부서 이름';

CREATE TABLE emp (
    emp_id     NUMBER(6)     CONSTRAINT emp_pk PRIMARY KEY,
    emp_name   VARCHAR2(50)  NOT NULL,
    email      VARCHAR2(100) CONSTRAINT emp_email_uk UNIQUE,
    dept_id    NUMBER(4)     CONSTRAINT emp_dept_fk REFERENCES dept,
    mgr_id     NUMBER(6)     CONSTRAINT emp_mgr_fk REFERENCES emp,
    job        VARCHAR2(20),
    salary     NUMBER(10, 2),
    hired_on   DATE          DEFAULT SYSDATE NOT NULL,
    updated_on DATE
);
CREATE INDEX emp_dept_ix ON emp (dept_id);
CREATE INDEX emp_mgr_ix ON emp (mgr_id);
COMMENT ON TABLE emp IS '사원';
COMMENT ON COLUMN emp.emp_name IS '사원 이름';
COMMENT ON COLUMN emp.mgr_id IS '관리자 사원 번호';

CREATE TABLE product (
    product_id   NUMBER(6)     CONSTRAINT product_pk PRIMARY KEY,
    product_name VARCHAR2(100) NOT NULL,
    category     VARCHAR2(30),
    price        NUMBER(10, 2)
);
COMMENT ON TABLE product IS '상품';

CREATE TABLE orders (
    order_id   NUMBER(10)   CONSTRAINT orders_pk PRIMARY KEY,
    emp_id     NUMBER(6)    CONSTRAINT orders_emp_fk REFERENCES emp,
    ordered_on DATE         NOT NULL,
    status     VARCHAR2(10) NOT NULL CONSTRAINT orders_status_ck CHECK (status IN ('NEW', 'DONE', 'CANCEL')),
    amount     NUMBER(12, 2)
);
CREATE INDEX orders_emp_ix ON orders (emp_id);
CREATE INDEX orders_status_ix ON orders (status);
CREATE INDEX orders_date_ix ON orders (ordered_on);
COMMENT ON TABLE orders IS '주문';

CREATE TABLE order_item (
    order_id   NUMBER(10)    CONSTRAINT order_item_order_fk REFERENCES orders,
    line_no    NUMBER(3),
    product_id NUMBER(6)     NOT NULL CONSTRAINT order_item_product_fk REFERENCES product,
    qty        NUMBER(5)     NOT NULL,
    price      NUMBER(10, 2) NOT NULL,
    CONSTRAINT order_item_pk PRIMARY KEY (order_id, line_no)
);
CREATE INDEX order_item_product_ix ON order_item (product_id);
COMMENT ON TABLE order_item IS '주문 품목';

CREATE TABLE shipment (
    shipment_id NUMBER(10) CONSTRAINT shipment_pk PRIMARY KEY,
    order_id    NUMBER(10) NOT NULL,
    line_no     NUMBER(3)  NOT NULL,
    shipped_on  DATE,
    CONSTRAINT shipment_item_fk FOREIGN KEY (order_id, line_no) REFERENCES order_item (order_id, line_no)
);
COMMENT ON TABLE shipment IS '배송 (주문 품목을 두 컬럼 FK로 참조)';

CREATE SEQUENCE order_seq START WITH 100001;

CREATE TABLE sales_hist (
    sale_id    NUMBER(10) NOT NULL,
    sold_on    DATE       NOT NULL,
    product_id NUMBER(6),
    amount     NUMBER(12, 2)
)
PARTITION BY RANGE (sold_on) (
    PARTITION p2023 VALUES LESS THAN (DATE '2024-01-01'),
    PARTITION p2024 VALUES LESS THAN (DATE '2025-01-01'),
    PARTITION p2025 VALUES LESS THAN (DATE '2026-01-01'),
    PARTITION pmax VALUES LESS THAN (MAXVALUE)
);
CREATE INDEX sales_hist_date_ix ON sales_hist (sold_on) LOCAL;
COMMENT ON TABLE sales_hist IS '판매 이력 (연도별 파티션)';

CREATE TABLE data_types (
    id             NUMBER(4) CONSTRAINT data_types_pk PRIMARY KEY,
    c_varchar      VARCHAR2(100),
    c_nvarchar     NVARCHAR2(50),
    c_char         CHAR(5),
    c_number       NUMBER,
    c_integer      NUMBER(38),
    c_decimal      NUMBER(12, 4),
    c_double       BINARY_DOUBLE,
    c_date         DATE,
    c_timestamp    TIMESTAMP(6),
    c_timestamp_tz TIMESTAMP(6) WITH TIME ZONE,
    c_interval     INTERVAL DAY TO SECOND,
    c_clob         CLOB,
    c_blob         BLOB,
    c_raw          RAW(16),
    c_json         JSON,
    c_boolean      BOOLEAN
);
COMMENT ON TABLE data_types IS '여러 자료형 값 보기 확인용';

PROMPT data
INSERT INTO dept VALUES (10, '영업', '서울');
INSERT INTO dept VALUES (20, '개발', '판교');
INSERT INTO dept VALUES (30, '인사', '서울');
INSERT INTO dept VALUES (40, '재무', '부산');
INSERT INTO dept VALUES (50, '물류', '인천');

INSERT INTO emp (emp_id, emp_name, email, dept_id, job, salary, hired_on)
SELECT LEVEL,
       '사원' || LPAD(LEVEL, 3, '0'),
       'emp' || LEVEL || '@example.com',
       (MOD(LEVEL - 1, 5) + 1) * 10,
       CASE WHEN LEVEL = 1 THEN 'CEO' WHEN LEVEL <= 5 THEN 'MANAGER' ELSE 'STAFF' END,
       3000 + MOD(LEVEL * 37, 50) * 100,
       DATE '2015-01-01' + LEVEL * 30
FROM dual CONNECT BY LEVEL <= 50;
UPDATE emp SET mgr_id = CASE WHEN emp_id <= 5 THEN 1 ELSE MOD(emp_id, 4) + 2 END WHERE emp_id > 1;

INSERT INTO product
SELECT LEVEL,
       '상품 ' || LEVEL,
       CASE MOD(LEVEL, 4) WHEN 0 THEN '전자' WHEN 1 THEN '도서' WHEN 2 THEN '의류' ELSE '식품' END,
       1000 + MOD(LEVEL * 53, 100) * 500
FROM dual CONNECT BY LEVEL <= 200;

INSERT INTO orders
SELECT LEVEL,
       MOD(LEVEL, 50) + 1,
       DATE '2024-01-01' + MOD(LEVEL, 700),
       CASE WHEN MOD(LEVEL, 1000) = 0 THEN 'NEW' WHEN MOD(LEVEL, 100) = 1 THEN 'CANCEL' ELSE 'DONE' END,
       MOD(LEVEL, 997) * 10.5
FROM dual CONNECT BY LEVEL <= 100000;

INSERT INTO order_item
SELECT o.order_id, l.line_no, MOD(o.order_id * 7 + l.line_no, 200) + 1, MOD(o.order_id, 5) + 1, 1000 + MOD(o.order_id, 50) * 100
FROM orders o CROSS JOIN (SELECT 1 AS line_no FROM dual UNION ALL SELECT 2 FROM dual) l;

INSERT INTO shipment
SELECT order_id, order_id, 1, ordered_on + 2 FROM orders WHERE order_id <= 1000 AND status = 'DONE';

INSERT INTO sales_hist
SELECT LEVEL, DATE '2023-01-01' + MOD(LEVEL, 1095), MOD(LEVEL, 200) + 1, MOD(LEVEL, 991) * 7
FROM dual CONNECT BY LEVEL <= 30000;

INSERT INTO data_types VALUES (
    1, '가나다 abc', N'유니코드', 'AB', 12345.678, 12345678901234567890, 1234.5678, 3.14159265358979,
    TIMESTAMP '2024-05-06 07:08:09', TIMESTAMP '2024-05-06 07:08:09.123456',
    TIMESTAMP '2024-05-06 07:08:09.123456 +09:00', INTERVAL '1 02:03:04.5' DAY TO SECOND,
    TO_CLOB('짧은 CLOB'), HEXTORAW('DEADBEEF'), HEXTORAW('00FF10'), JSON('{"name": "DBaro", "tags": [1, 2]}'), TRUE);
INSERT INTO data_types (id) VALUES (2);
INSERT INTO data_types (id, c_varchar, c_clob, c_number, c_boolean)
VALUES (3, RPAD('x', 100, 'x'), TO_CLOB(RPAD('a', 4000, 'a')) || TO_CLOB(RPAD('b', 4000, 'b')), -0.000001, FALSE);
COMMIT;

PROMPT views and code
CREATE OR REPLACE VIEW emp_dept_v AS
SELECT e.emp_id, e.emp_name, d.dept_name, m.emp_name AS mgr_name
FROM emp e
JOIN dept d ON d.dept_id = e.dept_id
LEFT JOIN emp m ON m.emp_id = e.mgr_id;

CREATE OR REPLACE FUNCTION emp_count(p_dept_id IN NUMBER) RETURN NUMBER IS
    v_count NUMBER;
BEGIN
    SELECT COUNT(*) INTO v_count FROM emp WHERE dept_id = p_dept_id;
    RETURN v_count;
END;
/

CREATE OR REPLACE PROCEDURE raise_salary(p_dept_id IN NUMBER, p_percent IN NUMBER) IS
BEGIN
    UPDATE emp SET salary = salary * (1 + p_percent / 100) WHERE dept_id = p_dept_id;
END;
/

CREATE OR REPLACE TRIGGER emp_touch_trg
BEFORE UPDATE ON emp FOR EACH ROW
BEGIN
    :NEW.updated_on := SYSDATE;
END;
/

CREATE OR REPLACE PACKAGE order_api AS
    FUNCTION total(p_order_id IN NUMBER) RETURN NUMBER;
END order_api;
/

CREATE OR REPLACE PACKAGE BODY order_api AS
    FUNCTION total(p_order_id IN NUMBER) RETURN NUMBER IS
        v_total NUMBER;
    BEGIN
        SELECT SUM(qty * price) INTO v_total FROM order_item WHERE order_id = p_order_id;
        RETURN v_total;
    END total;
END order_api;
/

PROMPT statistics (no histograms)
BEGIN
    DBMS_STATS.GATHER_SCHEMA_STATS(USER, method_opt => 'FOR ALL COLUMNS SIZE 1');
END;
/

SELECT 'DBaro seed done: ' || COUNT(*) || ' tables' AS result FROM user_tables;
EXIT
