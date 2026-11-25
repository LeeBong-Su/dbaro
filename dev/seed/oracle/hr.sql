-- 다른 스키마(DBARO_HR). 스키마 바꾸기와 다른 스키마 객체 보기를 확인하는 데 쓴다.
-- SYSDBA로 FREEPDB1에서 실행한다. &1 = 권한을 받을 앱 사용자.
CREATE USER dbaro_hr NO AUTHENTICATION DEFAULT TABLESPACE users QUOTA UNLIMITED ON users;

CREATE TABLE dbaro_hr.region (
    region_id   NUMBER(4)    CONSTRAINT region_pk PRIMARY KEY,
    region_name VARCHAR2(50) NOT NULL
);
COMMENT ON TABLE dbaro_hr.region IS '지역';

CREATE TABLE dbaro_hr.office (
    office_id   NUMBER(4)    CONSTRAINT office_pk PRIMARY KEY,
    region_id   NUMBER(4)    NOT NULL CONSTRAINT office_region_fk REFERENCES dbaro_hr.region,
    office_name VARCHAR2(50) NOT NULL
);
COMMENT ON TABLE dbaro_hr.office IS '사무소';

INSERT INTO dbaro_hr.region VALUES (1, '수도권');
INSERT INTO dbaro_hr.region VALUES (2, '영남');
INSERT INTO dbaro_hr.region VALUES (3, '호남');
INSERT INTO dbaro_hr.office VALUES (1, 1, '서울 본사');
INSERT INTO dbaro_hr.office VALUES (2, 1, '판교 연구소');
INSERT INTO dbaro_hr.office VALUES (3, 2, '부산 지사');
INSERT INTO dbaro_hr.office VALUES (4, 3, '광주 지사');
COMMIT;

GRANT SELECT, INSERT, UPDATE, DELETE ON dbaro_hr.region TO &1;
GRANT SELECT, INSERT, UPDATE, DELETE ON dbaro_hr.office TO &1;
