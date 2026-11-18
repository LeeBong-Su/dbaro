#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// 실행한 문장 하나의 기록.
struct HistoryEntry
{
    QDateTime time;          // 마지막으로 실행한 때
    QString connectionId;
    QString connectionName;
    QString sql;             // 실행한 그대로 (바인드 변수는 값을 넣기 전). 비밀번호는 가린다
    bool ok = true;
    qint64 elapsedMs = 0;
    qint64 rows = -1;        // 가져온 행 수 또는 영향 받은 행 수 (모르면 -1)
    QString error;
    int count = 1;           // 같은 연결에서 같은 문장을 연달아 실행한 횟수
};

// 실행 기록. 앱 데이터 폴더의 history.jsonl에 한 줄에 하나씩 덧붙이고, 최근 kMaxEntries개만 남긴다.
// 같은 연결에서 같은 문장을 연달아 실행하면 한 항목으로 합친다.
class QueryHistory
{
public:
    static constexpr int kMaxEntries = 10000;

    QueryHistory();
    explicit QueryHistory(const QString &filePath);

    QString filePath() const { return m_filePath; }
    void add(HistoryEntry entry);
    QList<HistoryEntry> entries() const; // 오래된 것이 앞
    bool clear(QString *error = nullptr);

    // ALTER USER ... IDENTIFIED BY, PASSWORD '...' 같은 비밀번호를 *** 로 가린다.
    static QString redact(const QString &sql);

private:
    void ensureLoaded() const;
    void rewrite() const;

    QString m_filePath;
    mutable bool m_loaded = false;
    mutable QList<HistoryEntry> m_entries;
    mutable int m_fileLines = 0; // 파일의 줄 수 (합치기 전). 너무 커지면 다시 쓴다
};
