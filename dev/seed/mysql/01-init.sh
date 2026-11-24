#!/bin/bash
# DBaro 개발용 MySQL·MariaDB 초기화. 공식 이미지가 처음 기동할 때 한 번 실행한다.
# Windows 폴더를 마운트하면 실행 권한이 있어서 source 되지 않고 따로 실행되므로 이미지의 함수(docker_process_sql)는 쓰지 않는다.
# 1) root로 다른 데이터베이스(dbaro_hr)를 만들어 앱 사용자에게 권한을 준다.
# 2) 앱 사용자로 접속해서 sql/ 의 스크립트를 실행한다 (뷰·루틴·트리거의 DEFINER가 앱 사용자가 되게).
# 하위 폴더(sql/)의 파일은 이미지가 직접 실행하지 않는다.

seed_user="${MARIADB_USER:-${MYSQL_USER:-}}"
seed_password="${MARIADB_PASSWORD:-${MYSQL_PASSWORD:-}}"
seed_database="${MARIADB_DATABASE:-${MYSQL_DATABASE:-}}"
seed_root_password="${MARIADB_ROOT_PASSWORD:-${MYSQL_ROOT_PASSWORD:-}}"
seed_client="$(command -v mariadb || command -v mysql)"

echo "DBaro seed: dbaro_hr database for ${seed_user}"
MYSQL_PWD="${seed_root_password}" "${seed_client}" --protocol=socket -uroot <<EOSQL || echo "DBaro seed: root part failed"
CREATE DATABASE IF NOT EXISTS dbaro_hr;
GRANT ALL ON dbaro_hr.* TO '${seed_user}'@'%';
-- 바이너리 로그가 켜진 MySQL에서 SUPER 없는 사용자도 함수·트리거를 만들 수 있게 (이 초기화 동안만)
SET GLOBAL log_bin_trust_function_creators = 1;
EOSQL

for seed_file in /docker-entrypoint-initdb.d/sql/*.sql; do
    echo "DBaro seed: ${seed_file}"
    MYSQL_PWD="${seed_password}" "${seed_client}" --protocol=socket \
        --default-character-set=utf8mb4 -u"${seed_user}" "${seed_database}" < "${seed_file}" \
        || echo "DBaro seed: ${seed_file} failed"
done
