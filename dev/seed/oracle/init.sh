#!/bin/bash
# DBaro 개발용 Oracle 초기화. gvenzl/oracle-free가 DB를 처음 만들 때 한 번 실행한다.
# 1) 앱 사용자(APP_USER)에게 실제 실행 계획(Alt+X)에 필요한 V$ 조회 권한을 주고 다른 스키마(DBARO_HR)를 만든다 (SYSDBA).
# 2) 앱 사용자로 접속해서 샘플 스키마(schema.sql)를 만든다.
# 실행 권한이 없으면 이미지의 진입 스크립트(set -euo pipefail)가 source 하므로 실패해도 멈추지 않게 || 를 붙인다.

export NLS_LANG=AMERICAN_AMERICA.AL32UTF8

echo "DBaro seed: privileges and the DBARO_HR schema"
sqlplus -s / as sysdba <<EOF || echo "DBaro seed: SYSDBA part failed"
ALTER SESSION SET CONTAINER = FREEPDB1;
GRANT SELECT_CATALOG_ROLE TO ${APP_USER};
GRANT CREATE VIEW, CREATE SYNONYM, CREATE MATERIALIZED VIEW TO ${APP_USER};
@/seed/hr.sql ${APP_USER}
EXIT
EOF

echo "DBaro seed: sample schema"
sqlplus -s -L "${APP_USER}/\"${APP_USER_PASSWORD}\"@//localhost:1521/FREEPDB1" @/seed/schema.sql </dev/null \
    || echo "DBaro seed: schema.sql failed"
