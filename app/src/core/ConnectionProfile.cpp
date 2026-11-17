#include "ConnectionProfile.h"

#include <QCoreApplication>
#include <QUuid>

namespace {
QString trDb(const char *text)
{
    return QCoreApplication::translate("Db", text);
}
} // namespace

ConnectionProfile ConnectionProfile::create(DbType type)
{
    ConnectionProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.type = type;
    profile.port = Db::defaultPort(type);
    return profile;
}

QJsonObject ConnectionProfile::toJson() const
{
    return QJsonObject{
        {"id", id},
        {"name", name},
        {"type", Db::typeKey(type)},
        {"host", host},
        {"port", port},
        {"oracleUseSid", oracleUseSid},
        {"database", database},
        {"user", user},
        {"savePassword", savePassword},
        {"environment", Env::key(environment)},
        {"readOnly", readOnly},
        {"connectTimeoutSec", connectTimeoutSec},
        {"queryTimeoutSec", queryTimeoutSec},
        {"initSql", initSql},
        {"driverPath", driverPath},
    };
}

ConnectionProfile ConnectionProfile::fromJson(const QJsonObject &json)
{
    ConnectionProfile profile;
    profile.id = json.value("id").toString();
    if (profile.id.isEmpty())
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.name = json.value("name").toString();
    profile.type = Db::typeFromKey(json.value("type").toString());
    profile.host = json.value("host").toString(QStringLiteral("localhost"));
    profile.port = json.value("port").toInt(Db::defaultPort(profile.type));
    profile.oracleUseSid = json.value("oracleUseSid").toBool(false);
    profile.database = json.value("database").toString();
    profile.user = json.value("user").toString();
    profile.savePassword = json.value("savePassword").toBool(true);
    profile.environment = Env::fromKey(json.value("environment").toString());
    profile.readOnly = json.value("readOnly").toBool(false);
    profile.connectTimeoutSec = json.value("connectTimeoutSec").toInt(10);
    profile.queryTimeoutSec = json.value("queryTimeoutSec").toInt(0);
    profile.initSql = json.value("initSql").toString();
    profile.driverPath = json.value("driverPath").toString();
    return profile;
}

QList<DbType> Db::allTypes()
{
    return {DbType::Oracle, DbType::Tibero, DbType::MariaDB, DbType::MySQL, DbType::PostgreSQL};
}

QString Db::typeName(DbType type)
{
    switch (type) {
    case DbType::Oracle: return QStringLiteral("Oracle");
    case DbType::Tibero: return QStringLiteral("Tibero");
    case DbType::MariaDB: return QStringLiteral("MariaDB");
    case DbType::MySQL: return QStringLiteral("MySQL");
    case DbType::PostgreSQL: return QStringLiteral("PostgreSQL");
    }
    return {};
}

QString Db::typeKey(DbType type)
{
    switch (type) {
    case DbType::Oracle: return QStringLiteral("oracle");
    case DbType::Tibero: return QStringLiteral("tibero");
    case DbType::MariaDB: return QStringLiteral("mariadb");
    case DbType::MySQL: return QStringLiteral("mysql");
    case DbType::PostgreSQL: return QStringLiteral("postgres");
    }
    return {};
}

DbType Db::typeFromKey(const QString &key, DbType fallback)
{
    for (DbType type : allTypes()) {
        if (typeKey(type) == key)
            return type;
    }
    return fallback;
}

int Db::defaultPort(DbType type)
{
    switch (type) {
    case DbType::Oracle: return 1521;
    case DbType::Tibero: return 8629;
    case DbType::MariaDB: return 3306;
    case DbType::MySQL: return 3306;
    case DbType::PostgreSQL: return 5432;
    }
    return 0;
}

QString Db::databaseLabel(DbType type, bool oracleUseSid)
{
    switch (type) {
    case DbType::Oracle: return oracleUseSid ? trDb("SID") : trDb("Service name");
    case DbType::Tibero: return trDb("DB name");
    default: return trDb("Database");
    }
}

QString Db::databasePlaceholder(DbType type, bool oracleUseSid)
{
    switch (type) {
    case DbType::Oracle: return oracleUseSid ? trDb("e.g. ORCL") : trDb("e.g. ORCLPDB1");
    case DbType::Tibero: return trDb("e.g. tibero");
    case DbType::PostgreSQL: return trDb("e.g. postgres");
    default: return trDb("Optional");
    }
}

bool Db::databaseRequired(DbType type)
{
    return type != DbType::MariaDB && type != DbType::MySQL;
}

bool Db::requiresDriverPath(DbType type)
{
    return type == DbType::Tibero;
}

QString Db::initSqlHint(DbType type)
{
    switch (type) {
    case DbType::Oracle:
    case DbType::Tibero:
        return QStringLiteral("ALTER SESSION SET NLS_DATE_FORMAT = 'YYYY-MM-DD HH24:MI:SS';");
    case DbType::MariaDB:
    case DbType::MySQL:
        return QStringLiteral("SET time_zone = '+09:00';");
    case DbType::PostgreSQL:
        return QStringLiteral("SET search_path TO public;");
    }
    return {};
}

QString Db::jdbcUrl(const ConnectionProfile &profile)
{
    const QString hostPort = QStringLiteral("%1:%2").arg(profile.host).arg(profile.port);
    switch (profile.type) {
    case DbType::Oracle:
        return profile.oracleUseSid
                   ? QStringLiteral("jdbc:oracle:thin:@%1:%2").arg(hostPort, profile.database)
                   : QStringLiteral("jdbc:oracle:thin:@//%1/%2").arg(hostPort, profile.database);
    case DbType::Tibero:
        return QStringLiteral("jdbc:tibero:thin:@%1:%2").arg(hostPort, profile.database);
    case DbType::MariaDB:
        return QStringLiteral("jdbc:mariadb://%1/%2").arg(hostPort, profile.database);
    case DbType::MySQL:
        return QStringLiteral("jdbc:mysql://%1/%2").arg(hostPort, profile.database);
    case DbType::PostgreSQL:
        return QStringLiteral("jdbc:postgresql://%1/%2").arg(hostPort, profile.database);
    }
    return {};
}

QList<Environment> Env::all()
{
    return {Environment::None, Environment::Development, Environment::Test, Environment::Staging,
            Environment::Production};
}

QString Env::name(Environment env)
{
    switch (env) {
    case Environment::None: return trDb("None");
    case Environment::Development: return trDb("Development");
    case Environment::Test: return trDb("Test");
    case Environment::Staging: return trDb("Staging");
    case Environment::Production: return trDb("Production");
    }
    return {};
}

QString Env::key(Environment env)
{
    switch (env) {
    case Environment::None: return QStringLiteral("none");
    case Environment::Development: return QStringLiteral("development");
    case Environment::Test: return QStringLiteral("test");
    case Environment::Staging: return QStringLiteral("staging");
    case Environment::Production: return QStringLiteral("production");
    }
    return {};
}

Environment Env::fromKey(const QString &key)
{
    for (Environment env : all()) {
        if (Env::key(env) == key)
            return env;
    }
    return Environment::None;
}

QColor Env::color(Environment env)
{
    switch (env) {
    case Environment::None: return {};
    case Environment::Development: return QColor(0x2e, 0x7d, 0x32);
    case Environment::Test: return QColor(0x15, 0x65, 0xc0);
    case Environment::Staging: return QColor(0xef, 0x6c, 0x00);
    case Environment::Production: return QColor(0xc6, 0x28, 0x28);
    }
    return {};
}
