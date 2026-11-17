#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QString>

enum class DbType { Oracle, Tibero, MariaDB, MySQL, PostgreSQL };

// 연결이 어떤 환경인지. 색상 표시와 안전장치(운영 DB 확인 등)에 쓴다.
enum class Environment { None, Development, Test, Staging, Production };

// 연결 하나의 설정. 비밀번호는 여기에 두지 않고 SecretStore에 따로 저장한다.
struct ConnectionProfile
{
    QString id;
    QString name;
    DbType type = DbType::Oracle;
    QString host = QStringLiteral("localhost");
    int port = 1521;
    bool oracleUseSid = false; // Oracle만: true면 SID, false면 서비스명으로 접속
    QString database;          // 서비스명, SID, DB 이름 (DB 종류에 따라 의미가 다름)
    QString user;
    bool savePassword = true;
    Environment environment = Environment::None;
    bool readOnly = false;
    int connectTimeoutSec = 10;
    int queryTimeoutSec = 0; // 0이면 제한 없음
    QString initSql;         // 접속 직후 실행할 SQL
    QString driverPath;      // 비우면 드라이버 자동 다운로드. Tibero는 필수

    static ConnectionProfile create(DbType type);

    QJsonObject toJson() const;
    static ConnectionProfile fromJson(const QJsonObject &json);
};

namespace Db {
QList<DbType> allTypes();
QString typeName(DbType type);
QString typeKey(DbType type); // JSON 저장용 고정 키
DbType typeFromKey(const QString &key, DbType fallback = DbType::Oracle);
int defaultPort(DbType type);
QString databaseLabel(DbType type, bool oracleUseSid);
QString databasePlaceholder(DbType type, bool oracleUseSid);
bool databaseRequired(DbType type);
bool requiresDriverPath(DbType type);
QString initSqlHint(DbType type);
QString jdbcUrl(const ConnectionProfile &profile);
} // namespace Db

namespace Env {
QList<Environment> all();
QString name(Environment env);
QString key(Environment env);
Environment fromKey(const QString &key);
QColor color(Environment env); // None이면 유효하지 않은 색
} // namespace Env
