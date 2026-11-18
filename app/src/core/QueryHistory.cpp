#include "QueryHistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QJsonObject toJson(const HistoryEntry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("time"), entry.time.toUTC().toString(Qt::ISODateWithMs));
    if (!entry.connectionId.isEmpty())
        object.insert(QStringLiteral("connectionId"), entry.connectionId);
    if (!entry.connectionName.isEmpty())
        object.insert(QStringLiteral("connection"), entry.connectionName);
    object.insert(QStringLiteral("sql"), entry.sql);
    object.insert(QStringLiteral("ok"), entry.ok);
    object.insert(QStringLiteral("elapsedMs"), entry.elapsedMs);
    if (entry.rows >= 0)
        object.insert(QStringLiteral("rows"), entry.rows);
    if (!entry.error.isEmpty())
        object.insert(QStringLiteral("error"), entry.error);
    if (entry.count > 1)
        object.insert(QStringLiteral("count"), entry.count);
    return object;
}

HistoryEntry fromJson(const QJsonObject &object)
{
    HistoryEntry entry;
    entry.time = QDateTime::fromString(object.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs).toLocalTime();
    entry.connectionId = object.value(QStringLiteral("connectionId")).toString();
    entry.connectionName = object.value(QStringLiteral("connection")).toString();
    entry.sql = object.value(QStringLiteral("sql")).toString();
    entry.ok = object.value(QStringLiteral("ok")).toBool(true);
    entry.elapsedMs = object.value(QStringLiteral("elapsedMs")).toInteger();
    entry.rows = object.value(QStringLiteral("rows")).toInteger(-1);
    entry.error = object.value(QStringLiteral("error")).toString();
    entry.count = qMax(1, object.value(QStringLiteral("count")).toInt(1));
    return entry;
}

// 바로 앞 항목과 같은 연결·같은 문장이면 합친다.
void append(QList<HistoryEntry> &entries, const HistoryEntry &entry)
{
    if (!entries.isEmpty()) {
        HistoryEntry &last = entries.last();
        if (last.connectionId == entry.connectionId && last.sql == entry.sql) {
            const int count = last.count + entry.count;
            last = entry;
            last.count = count;
            return;
        }
    }
    entries.append(entry);
}

} // namespace

QueryHistory::QueryHistory()
    : QueryHistory(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/history.jsonl"))
{
}

QueryHistory::QueryHistory(const QString &filePath)
    : m_filePath(filePath)
{
}

QString QueryHistory::redact(const QString &sql)
{
    static const QList<QPair<QRegularExpression, QString>> rules{
        // Oracle·Tibero·MySQL: IDENTIFIED BY pw / IDENTIFIED WITH plugin BY 'pw'
        {QRegularExpression(QStringLiteral("(\\bIDENTIFIED\\s+(?:WITH\\s+\\S+\\s+)?BY\\s+)(\"(?:[^\"]|\"\")*\"|'(?:[^']|'')*'|[^\\s;]+)"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1***")},
        // PostgreSQL: CREATE/ALTER ROLE ... PASSWORD 'pw'
        {QRegularExpression(QStringLiteral("(\\bPASSWORD\\s+)('(?:[^']|'')*')"), QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1'***'")},
        // MySQL: SET PASSWORD [FOR user] = 'pw'
        {QRegularExpression(QStringLiteral("(\\bSET\\s+PASSWORD\\b[^=;]*=\\s*)('(?:[^']|'')*'|[^\\s;]+)"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("\\1'***'")},
    };
    QString text = sql;
    for (const auto &[pattern, replacement] : rules)
        text.replace(pattern, replacement);
    return text;
}

void QueryHistory::ensureLoaded() const
{
    if (m_loaded)
        return;
    m_loaded = true;
    m_entries.clear();
    m_fileLines = 0;
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        ++m_fileLines;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject())
            append(m_entries, fromJson(document.object()));
    }
    if (m_entries.size() > kMaxEntries)
        m_entries.remove(0, m_entries.size() - kMaxEntries);
}

void QueryHistory::add(HistoryEntry entry)
{
    ensureLoaded();
    entry.sql = redact(entry.sql.trimmed());
    if (entry.sql.isEmpty())
        return;
    if (!entry.time.isValid())
        entry.time = QDateTime::currentDateTime();
    append(m_entries, entry);

    // 파일에는 덧붙이기만 한다. 줄이 너무 많아지면 합친 목록으로 다시 쓴다.
    if (m_entries.size() > kMaxEntries)
        m_entries.removeFirst();
    if (m_fileLines + 1 > kMaxEntries + kMaxEntries / 5) {
        rewrite();
        return;
    }
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QFile file(m_filePath);
    if (file.open(QIODevice::Append)) {
        file.write(QJsonDocument(toJson(entry)).toJson(QJsonDocument::Compact) + '\n');
        ++m_fileLines;
    }
}

void QueryHistory::rewrite() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    for (const HistoryEntry &entry : std::as_const(m_entries))
        file.write(QJsonDocument(toJson(entry)).toJson(QJsonDocument::Compact) + '\n');
    if (file.commit())
        m_fileLines = int(m_entries.size());
}

QList<HistoryEntry> QueryHistory::entries() const
{
    ensureLoaded();
    return m_entries;
}

bool QueryHistory::clear(QString *error)
{
    m_entries.clear();
    m_loaded = true;
    m_fileLines = 0;
    QFile file(m_filePath);
    if (file.exists() && !file.remove()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
