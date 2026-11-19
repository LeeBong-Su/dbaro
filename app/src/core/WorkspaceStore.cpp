#include "WorkspaceStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr int kFormatVersion = 2;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

WorkspaceTab tabFromJson(const QJsonObject &object)
{
    WorkspaceTab tab;
    tab.title = object.value(QStringLiteral("title")).toString();
    tab.fixedTitle = object.value(QStringLiteral("fixedTitle")).toBool();
    tab.sql = object.value(QStringLiteral("sql")).toString();
    tab.cursor = object.value(QStringLiteral("cursor")).toInt();
    tab.filePath = object.value(QStringLiteral("filePath")).toString();
    tab.modified = object.value(QStringLiteral("modified")).toBool();
    tab.format.encoding = TextFile::fromKey(object.value(QStringLiteral("encoding")).toString());
    tab.format.bom = object.value(QStringLiteral("bom")).toBool();
    tab.format.crlf = object.value(QStringLiteral("crlf")).toBool();
    tab.ddlKey = object.value(QStringLiteral("ddlKey")).toString();
    return tab;
}

QJsonObject tabToJson(const WorkspaceTab &tab)
{
    QJsonObject object;
    object.insert(QStringLiteral("title"), tab.title);
    if (tab.fixedTitle)
        object.insert(QStringLiteral("fixedTitle"), true);
    if (!tab.sql.isEmpty())
        object.insert(QStringLiteral("sql"), tab.sql);
    if (tab.cursor > 0)
        object.insert(QStringLiteral("cursor"), tab.cursor);
    if (!tab.filePath.isEmpty()) {
        object.insert(QStringLiteral("filePath"), tab.filePath);
        object.insert(QStringLiteral("modified"), tab.modified);
        object.insert(QStringLiteral("encoding"), TextFile::toKey(tab.format.encoding));
        object.insert(QStringLiteral("bom"), tab.format.bom);
        object.insert(QStringLiteral("crlf"), tab.format.crlf);
    }
    if (!tab.ddlKey.isEmpty())
        object.insert(QStringLiteral("ddlKey"), tab.ddlKey);
    return object;
}
} // namespace

WorkspaceStore::WorkspaceStore()
    : WorkspaceStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/workspace.json"))
{
}

WorkspaceStore::WorkspaceStore(const QString &filePath)
    : m_filePath(filePath)
{
}

bool WorkspaceStore::loadAll(QHash<QString, Workspace> *workspaces, QString *error) const
{
    workspaces->clear();
    QFile file(m_filePath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        setError(error, parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();

    if (root.contains(QStringLiteral("connections"))) {
        const QJsonObject connections = root.value(QStringLiteral("connections")).toObject();
        for (auto it = connections.constBegin(); it != connections.constEnd(); ++it) {
            const QJsonObject object = it.value().toObject();
            Workspace workspace;
            for (const QJsonValue &tab : object.value(QStringLiteral("tabs")).toArray())
                workspace.tabs.append(tabFromJson(tab.toObject()));
            workspace.current = object.value(QStringLiteral("current")).toInt();
            workspaces->insert(it.key(), workspace);
        }
        return true;
    }

    // 예전 형식: 창 하나의 탭 목록, 탭마다 연결. 연결별로 나누고 연결 없는 탭은 버린다.
    for (const QJsonValue &value : root.value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString connectionId = object.value(QStringLiteral("connectionId")).toString();
        if (connectionId.isEmpty())
            continue;
        WorkspaceTab tab = tabFromJson(object);
        if (!tab.fixedTitle && tab.filePath.isEmpty())
            tab.title.clear(); // 예전 제목은 연결 이름이었다
        (*workspaces)[connectionId].tabs.append(tab);
    }
    return true;
}

bool WorkspaceStore::saveAll(const QHash<QString, Workspace> &workspaces, QString *error) const
{
    QJsonObject connections;
    for (auto it = workspaces.constBegin(); it != workspaces.constEnd(); ++it) {
        if (it.value().tabs.isEmpty())
            continue;
        QJsonArray tabs;
        for (const WorkspaceTab &tab : it.value().tabs)
            tabs.append(tabToJson(tab));
        QJsonObject object;
        object.insert(QStringLiteral("current"), it.value().current);
        object.insert(QStringLiteral("tabs"), tabs);
        connections.insert(it.key(), object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("connections"), connections);

    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool WorkspaceStore::load(const QString &connectionId, Workspace *workspace, QString *error) const
{
    QHash<QString, Workspace> all;
    if (!loadAll(&all, error)) {
        *workspace = {};
        return false;
    }
    *workspace = all.value(connectionId);
    return true;
}

bool WorkspaceStore::save(const QString &connectionId, const Workspace &workspace, QString *error) const
{
    QHash<QString, Workspace> all;
    loadAll(&all); // 읽을 수 없으면 이 연결만이라도 남긴다
    all.insert(connectionId, workspace);
    return saveAll(all, error);
}

bool WorkspaceStore::remove(const QString &connectionId, QString *error) const
{
    QHash<QString, Workspace> all;
    if (!loadAll(&all, error))
        return false;
    if (all.remove(connectionId) == 0)
        return true;
    return saveAll(all, error);
}
