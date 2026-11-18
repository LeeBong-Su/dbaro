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
constexpr int kFormatVersion = 1;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
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

bool WorkspaceStore::load(Workspace *workspace, QString *error) const
{
    *workspace = {};
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
    for (const QJsonValue &value : root.value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject object = value.toObject();
        WorkspaceTab tab;
        tab.title = object.value(QStringLiteral("title")).toString();
        tab.fixedTitle = object.value(QStringLiteral("fixedTitle")).toBool();
        tab.connectionId = object.value(QStringLiteral("connectionId")).toString();
        tab.sql = object.value(QStringLiteral("sql")).toString();
        tab.cursor = object.value(QStringLiteral("cursor")).toInt();
        tab.filePath = object.value(QStringLiteral("filePath")).toString();
        tab.modified = object.value(QStringLiteral("modified")).toBool();
        tab.format.encoding = TextFile::fromKey(object.value(QStringLiteral("encoding")).toString());
        tab.format.bom = object.value(QStringLiteral("bom")).toBool();
        tab.format.crlf = object.value(QStringLiteral("crlf")).toBool();
        tab.ddlKey = object.value(QStringLiteral("ddlKey")).toString();
        workspace->tabs.append(tab);
    }
    workspace->current = root.value(QStringLiteral("current")).toInt();
    return true;
}

bool WorkspaceStore::save(const Workspace &workspace, QString *error) const
{
    QJsonArray tabs;
    for (const WorkspaceTab &tab : workspace.tabs) {
        QJsonObject object;
        object.insert(QStringLiteral("title"), tab.title);
        if (tab.fixedTitle)
            object.insert(QStringLiteral("fixedTitle"), true);
        if (!tab.connectionId.isEmpty())
            object.insert(QStringLiteral("connectionId"), tab.connectionId);
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
        tabs.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("current"), workspace.current);
    root.insert(QStringLiteral("tabs"), tabs);

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
