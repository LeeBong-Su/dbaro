#include "ConnectSpec.h"

#include "SqlSplitter.h"

#include <QCborArray>

QCborMap ConnectSpec::toCbor() const
{
    QCborMap map;
    map.insert(QStringLiteral("type"), type);
    map.insert(QStringLiteral("url"), url);
    map.insert(QStringLiteral("user"), user);
    map.insert(QStringLiteral("password"), password);
    if (!driverPath.isEmpty())
        map.insert(QStringLiteral("driverPath"), driverPath);
    if (!driverClass.isEmpty())
        map.insert(QStringLiteral("driverClass"), driverClass);
    map.insert(QStringLiteral("autoCommit"), autoCommit);
    map.insert(QStringLiteral("readOnly"), readOnly);
    map.insert(QStringLiteral("connectTimeoutSec"), connectTimeoutSec);
    map.insert(QStringLiteral("initSql"), QCborArray::fromStringList(initSql));
    return map;
}

ConnectSpec ConnectSpec::fromProfile(const ConnectionProfile &profile, const QString &password, bool autoCommit)
{
    ConnectSpec spec;
    spec.type = Db::typeKey(profile.type);
    spec.url = Db::jdbcUrl(profile);
    spec.user = profile.user;
    spec.password = password;
    spec.driverPath = profile.driverPath;
    spec.autoCommit = autoCommit;
    spec.readOnly = profile.readOnly;
    spec.connectTimeoutSec = profile.connectTimeoutSec;
    for (const SqlStatement &statement : SqlSplitter::split(profile.initSql, sqlDialectFor(profile.type)))
        spec.initSql << statement.text;
    return spec;
}
