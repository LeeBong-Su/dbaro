-- DBaro 개발용 샘플 스키마 (MySQL·MariaDB 함께 쓴다). 01-init.sh가 앱 사용자로 접속해서 실행한다.
-- 모델은 dev/seed/oracle/schema.sql 과 같다. 다른 데이터베이스(dbaro_hr)에도 테이블을 만든다.
-- 행은 0~9 숫자 표를 교차 조인해서 만든다 (MySQL 재귀 CTE의 1000번 제한을 피한다).

CREATE TABLE dept (
    dept_id   INT         PRIMARY KEY,
    dept_name VARCHAR(50) NOT NULL COMMENT '부서 이름',
    location  VARCHAR(50)
) COMMENT '부서';

CREATE TABLE emp (
    emp_id     INT           PRIMARY KEY,
    emp_name   VARCHAR(50)   NOT NULL COMMENT '사원 이름',
    email      VARCHAR(100),
    dept_id    INT,
    mgr_id     INT           COMMENT '관리자 사원 번호',
    job        VARCHAR(20),
    salary     DECIMAL(10, 2),
    hired_on   DATE          NOT NULL,
    updated_on DATETIME,
    CONSTRAINT emp_email_uk UNIQUE (email),
    CONSTRAINT emp_dept_fk FOREIGN KEY (dept_id) REFERENCES dept (dept_id),
    CONSTRAINT emp_mgr_fk FOREIGN KEY (mgr_id) REFERENCES emp (emp_id)
) COMMENT '사원';

CREATE TABLE product (
    product_id   INT            PRIMARY KEY,
    product_name VARCHAR(100)   NOT NULL,
    category     VARCHAR(30),
    price        DECIMAL(10, 2)
) COMMENT '상품';

CREATE TABLE orders (
    order_id   BIGINT         PRIMARY KEY,
    emp_id     INT,
    ordered_on DATE           NOT NULL,
    status     VARCHAR(10)    NOT NULL,
    amount     DECIMAL(12, 2),
    CONSTRAINT orders_emp_fk FOREIGN KEY (emp_id) REFERENCES emp (emp_id),
    CONSTRAINT orders_status_ck CHECK (status IN ('NEW', 'DONE', 'CANCEL'))
) COMMENT '주문';
CREATE INDEX orders_status_ix ON orders (status);
CREATE INDEX orders_date_ix ON orders (ordered_on);

CREATE TABLE order_item (
    order_id   BIGINT,
    line_no    SMALLINT,
    product_id INT            NOT NULL,
    qty        INT            NOT NULL,
    price      DECIMAL(10, 2) NOT NULL,
    PRIMARY KEY (order_id, line_no),
    CONSTRAINT order_item_order_fk FOREIGN KEY (order_id) REFERENCES orders (order_id),
    CONSTRAINT order_item_product_fk FOREIGN KEY (product_id) REFERENCES product (product_id)
) COMMENT '주문 품목';

CREATE TABLE shipment (
    shipment_id BIGINT   PRIMARY KEY,
    order_id    BIGINT   NOT NULL,
    line_no     SMALLINT NOT NULL,
    shipped_on  DATE,
    CONSTRAINT shipment_item_fk FOREIGN KEY (order_id, line_no) REFERENCES order_item (order_id, line_no)
) COMMENT '배송 (주문 품목을 두 컬럼 FK로 참조)';

CREATE TABLE sales_hist (
    sale_id    BIGINT  NOT NULL,
    sold_on    DATE    NOT NULL,
    product_id INT,
    amount     DECIMAL(12, 2),
    PRIMARY KEY (sale_id, sold_on),
    KEY sales_hist_date_ix (sold_on)
) COMMENT '판매 이력 (연도별 파티션)'
PARTITION BY RANGE COLUMNS (sold_on) (
    PARTITION p2023 VALUES LESS THAN ('2024-01-01'),
    PARTITION p2024 VALUES LESS THAN ('2025-01-01'),
    PARTITION p2025 VALUES LESS THAN ('2026-01-01'),
    PARTITION pmax VALUES LESS THAN (MAXVALUE)
);

CREATE TABLE data_types (
    id          INT PRIMARY KEY,
    c_varchar   VARCHAR(100),
    c_text      TEXT,
    c_char      CHAR(5),
    c_tinyint   TINYINT,
    c_bigint    BIGINT UNSIGNED,
    c_decimal   DECIMAL(12, 4),
    c_float     FLOAT,
    c_double    DOUBLE,
    c_bit       BIT(8),
    c_boolean   BOOLEAN,
    c_date      DATE,
    c_time      TIME,
    c_datetime  DATETIME(6),
    c_timestamp TIMESTAMP NULL,
    c_year      YEAR,
    c_blob      BLOB,
    c_varbinary VARBINARY(16),
    c_json      JSON,
    c_enum      ENUM('A', 'B', 'C'),
    c_set       SET('x', 'y', 'z')
) COMMENT '여러 자료형 값 보기 확인용';

CREATE TABLE seed_digits (d INT PRIMARY KEY);
INSERT INTO seed_digits VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);

INSERT INTO dept VALUES (10, '영업', '서울'), (20, '개발', '판교'), (30, '인사', '서울'), (40, '재무', '부산'), (50, '물류', '인천');

INSERT INTO emp (emp_id, emp_name, email, dept_id, job, salary, hired_on)
SELECT n,
       CONCAT('사원', LPAD(n, 3, '0')),
       CONCAT('emp', n, '@example.com'),
       ((n - 1) % 5 + 1) * 10,
       CASE WHEN n = 1 THEN 'CEO' WHEN n <= 5 THEN 'MANAGER' ELSE 'STAFF' END,
       3000 + (n * 37) % 50 * 100,
       DATE_ADD('2015-01-01', INTERVAL n * 30 DAY)
FROM (SELECT a.d + b.d * 10 + 1 AS n FROM seed_digits a CROSS JOIN seed_digits b) s
WHERE n <= 50;
UPDATE emp SET mgr_id = CASE WHEN emp_id <= 5 THEN 1 ELSE emp_id % 4 + 2 END WHERE emp_id > 1;

INSERT INTO product
SELECT n,
       CONCAT('상품 ', n),
       CASE n % 4 WHEN 0 THEN '전자' WHEN 1 THEN '도서' WHEN 2 THEN '의류' ELSE '식품' END,
       1000 + (n * 53) % 100 * 500
FROM (SELECT a.d + b.d * 10 + c.d * 100 + 1 AS n FROM seed_digits a CROSS JOIN seed_digits b CROSS JOIN seed_digits c) s
WHERE n <= 200;

INSERT INTO orders
SELECT n,
       n % 50 + 1,
       DATE_ADD('2024-01-01', INTERVAL n % 700 DAY),
       CASE WHEN n % 1000 = 0 THEN 'NEW' WHEN n % 100 = 1 THEN 'CANCEL' ELSE 'DONE' END,
       (n % 997) * 10.5
FROM (SELECT a.d + b.d * 10 + c.d * 100 + e.d * 1000 + f.d * 10000 + 1 AS n
      FROM seed_digits a CROSS JOIN seed_digits b CROSS JOIN seed_digits c CROSS JOIN seed_digits e CROSS JOIN seed_digits f) s;

INSERT INTO order_item
SELECT o.order_id, l.line_no, (o.order_id * 7 + l.line_no) % 200 + 1, o.order_id % 5 + 1, 1000 + o.order_id % 50 * 100
FROM orders o CROSS JOIN (SELECT 1 AS line_no UNION ALL SELECT 2) l;

INSERT INTO shipment
SELECT order_id, order_id, 1, DATE_ADD(ordered_on, INTERVAL 2 DAY) FROM orders WHERE order_id <= 1000 AND status = 'DONE';

INSERT INTO sales_hist
SELECT n, DATE_ADD('2023-01-01', INTERVAL n % 1095 DAY), n % 200 + 1, (n % 991) * 7
FROM (SELECT a.d + b.d * 10 + c.d * 100 + e.d * 1000 + f.d * 10000 + 1 AS n
      FROM seed_digits a CROSS JOIN seed_digits b CROSS JOIN seed_digits c CROSS JOIN seed_digits e CROSS JOIN seed_digits f) s
WHERE n <= 30000;

DROP TABLE seed_digits;

INSERT INTO data_types VALUES (
    1, '가나다 abc', '긴 글 text', 'AB', 127, 18446744073709551615, 1234.5678, 3.14, 3.14159265358979, b'10100101', TRUE,
    '2024-05-06', '07:08:09', '2024-05-06 07:08:09.123456', '2024-05-06 07:08:09', 2024,
    X'DEADBEEF', X'00FF10', '{"name": "DBaro", "tags": [1, 2]}', 'B', 'x,z');
INSERT INTO data_types (id) VALUES (2);
INSERT INTO data_types (id, c_varchar, c_text, c_decimal, c_boolean) VALUES (3, REPEAT('x', 100), CONCAT(REPEAT('a', 4000), REPEAT('b', 4000)), -0.0001, FALSE);

CREATE VIEW emp_dept_v AS
SELECT e.emp_id, e.emp_name, d.dept_name, m.emp_name AS mgr_name
FROM emp e
JOIN dept d ON d.dept_id = e.dept_id
LEFT JOIN emp m ON m.emp_id = e.mgr_id;

DELIMITER $$

CREATE FUNCTION emp_count(p_dept_id INT) RETURNS INT
READS SQL DATA
BEGIN
    DECLARE v_count INT;
    SELECT COUNT(*) INTO v_count FROM emp WHERE dept_id = p_dept_id;
    RETURN v_count;
END$$

CREATE PROCEDURE raise_salary(IN p_dept_id INT, IN p_percent DECIMAL(5, 2))
BEGIN
    UPDATE emp SET salary = salary * (1 + p_percent / 100) WHERE dept_id = p_dept_id;
END$$

CREATE TRIGGER emp_touch_trg BEFORE UPDATE ON emp FOR EACH ROW
BEGIN
    SET NEW.updated_on = NOW();
END$$

DELIMITER ;

-- 다른 데이터베이스
CREATE TABLE dbaro_hr.region (
    region_id   INT         PRIMARY KEY,
    region_name VARCHAR(50) NOT NULL
) COMMENT '지역';
CREATE TABLE dbaro_hr.office (
    office_id   INT         PRIMARY KEY,
    region_id   INT         NOT NULL,
    office_name VARCHAR(50) NOT NULL,
    CONSTRAINT office_region_fk FOREIGN KEY (region_id) REFERENCES dbaro_hr.region (region_id)
) COMMENT '사무소';
INSERT INTO dbaro_hr.region VALUES (1, '수도권'), (2, '영남'), (3, '호남');
INSERT INTO dbaro_hr.office VALUES (1, 1, '서울 본사'), (2, 1, '판교 연구소'), (3, 2, '부산 지사'), (4, 3, '광주 지사');

ANALYZE TABLE dept, emp, product, orders, order_item, shipment, sales_hist, data_types;
