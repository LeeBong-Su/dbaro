#pragma once

#include "core/ConnectionProfile.h"
#include "core/SqlDialect.h"
#include "core/TextFile.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class BusyBadge;
class ConnectionBar;
class EngineClient;
class FindBar;
class MetadataService;
class QSplitter;
class QuerySession;
class ResultPanel;
class SqlEditor;
struct EngineError;
struct SqlStatement;

// 쿼리 탭 하나. 위는 SQL 에디터, 아래는 결과 패널이다. 탭마다 DB 세션(연결)을 따로 가진다.
class QueryTab : public QWidget
{
    Q_OBJECT

public:
    explicit QueryTab(EngineClient *engine, QWidget *parent = nullptr);
    ~QueryTab() override;

    SqlEditor *editor() const { return m_editor; }
    FindBar *findBar() const { return m_findBar; }
    ResultPanel *resultPanel() const { return m_results; }
    QuerySession *session() const { return m_session; }

    // 이 탭이 쓰는 연결의 id. 비어 있으면 연결을 고르지 않은 상태다.
    QString connectionId() const { return m_connectionId; }
    void setConnectionId(const QString &id);

    // 자동완성이 쓸 메타데이터 (Explorer와 같은 캐시)
    void setMetadataService(MetadataService *metadata);

    // 연결의 DB 종류에 맞춰 하이라이트와 문장 분리 규칙을 바꾼다.
    void setDialect(SqlDialect dialect);
    SqlDialect dialect() const { return m_dialect; }

    // 이 탭 세션의 지금 스키마 (Oracle·Tibero CURRENT_SCHEMA, PostgreSQL search_path 첫 스키마,
    // MySQL·MariaDB 데이터베이스). 접속 전이거나 모르면 빈 값
    QString currentSchema() const;
    // 사용자가 고른 스키마 (연결 정보 막대, 탐색기, 직접 실행한 ALTER SESSION·USE·SET search_path).
    // 다시 접속하거나 앱을 다시 켜면 이 스키마로 바꾼다. 비어 있으면 접속 설정의 기본값
    QString preferredSchema() const { return m_preferredSchema; }
    void setPreferredSchema(const QString &schema) { m_preferredSchema = schema; }

    // "USE x" 문장이면 x를 돌려준다 (MySQL·MariaDB). 아니면 빈 문자열.
    static QString useTarget(const QString &sql);

    // 이 탭이 연 SQL 파일 (없으면 빈 값)과 그 파일의 인코딩·줄 끝 형식
    QString filePath() const { return m_filePath; }
    TextFileFormat fileFormat() const { return m_fileFormat; }
    void setFile(const QString &path, const TextFileFormat &format);
    // 파일 탭이고 저장하지 않은 변경이 있으면 true
    bool hasUnsavedFile() const;

    // SQL*Plus DEFINE: &&name으로 한 번 정한 치환 변수 값 (키는 BindVariable::key). 이 탭에서 다시 묻지 않는다.
    const QHash<QString, QString> &defines() const { return m_defines; }
    void setDefine(const QString &key, const QString &value) { m_defines.insert(key, value); }

    // 탭 맨 위 연결 정보 표시줄에 보여 줄 연결 (없으면 nullptr)
    void setConnectionProfile(const ConnectionProfile *profile);
    ConnectionBar *connectionBar() const { return m_connectionBar; }


    // 이 탭에서 기다리는 일(F4 DDL 등)을 에디터 오른쪽 위에 보인다. begin이 돌려준 번호로 end
    int beginActivity(const QString &text);
    void endActivity(int ticket);
    BusyBadge *activityBadge() const { return m_badge; }
signals:
    void cursorPositionChanged(int line, int column);
    void schemaSwitchRequested(const QString &schema); // 연결 정보 막대에서 다른 스키마를 골랐다

private:
    void showError(const SqlStatement &statement, const EngineError &error);
    void prepareEditing();
    void applyEdits(const QStringList &statements);
    void onStatementSucceeded(const SqlStatement &statement);
    void onSchemaChanged(const QString &schema);
    void loadSchemaList();
    void updateConnectionBar();

    QString m_connectionId;
    QString m_filePath;
    QHash<QString, QString> m_defines;
    TextFileFormat m_fileFormat;
    QPointer<MetadataService> m_metadata;
    QString m_preferredSchema;
    bool m_wasConnected = false;
    SqlDialect m_dialect = SqlDialect::Generic;
    ConnectionBar *m_connectionBar = nullptr;
    SqlEditor *m_editor = nullptr;
    FindBar *m_findBar = nullptr;
    ResultPanel *m_results = nullptr;
    QSplitter *m_splitter = nullptr;
    QuerySession *m_session = nullptr;
    BusyBadge *m_badge = nullptr;
};
