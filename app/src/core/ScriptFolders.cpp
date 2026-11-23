#include "ScriptFolders.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>

namespace {

QString key(DbType type)
{
    return QStringLiteral("scripts/") + Db::typeKey(type);
}

} // namespace

QString ScriptFolders::folder(DbType type)
{
    return QSettings().value(key(type)).toString();
}

void ScriptFolders::setFolder(DbType type, const QString &path)
{
    QSettings settings;
    if (path.trimmed().isEmpty())
        settings.remove(key(type));
    else
        settings.setValue(key(type), QDir::cleanPath(path.trimmed()));
}

QString ScriptFolders::resolve(const QString &name, const QStringList &folders)
{
    if (name.isEmpty())
        return {};
    QStringList candidates{name};
    if (QFileInfo(name).suffix().isEmpty())
        candidates << name + QStringLiteral(".sql");
    for (const QString &candidate : std::as_const(candidates)) {
        if (QDir::isAbsolutePath(candidate)) {
            if (QFileInfo(candidate).isFile())
                return QDir::cleanPath(candidate);
            continue;
        }
        for (const QString &folder : folders) {
            if (folder.isEmpty())
                continue;
            const QString path = QDir(folder).filePath(candidate);
            if (QFileInfo(path).isFile())
                return QDir::cleanPath(path);
        }
    }
    return {};
}

QStringList ScriptFolders::list(const QString &folder, int limit)
{
    QStringList files;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return files;
    const QDir base(folder);
    QDirIterator it(folder, {QStringLiteral("*.sql")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && files.size() < limit)
        files << base.relativeFilePath(it.next());
    files.sort(Qt::CaseInsensitive);
    return files;
}
