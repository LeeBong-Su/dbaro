#pragma once

#include "core/SqlDialect.h"

#include <QCborMap>
#include <QPointer>
#include <QWidget>

class MetadataService;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;

// 테이블 상세 탭. 컬럼, 인덱스, 외래 키, 이 테이블을 가리키는 외래 키, DDL을 탭으로 보여 준다.
// 메타데이터 세션에서 읽으므로 쿼리 탭이 실행 중이어도 된다. Select Rows·Import Data로 이어 갈 수 있다.
class TableDetailTab : public QWidget
{
    Q_OBJECT

public:
    TableDetailTab(MetadataService *metadata, const QString &connectionId, const QString &schema, const QString &name,
                   bool schemaQuoted, bool nameQuoted, SqlDialect dialect, QWidget *parent = nullptr,
                   const QString &currentSchema = {}); // currentSchema: 스키마 없이 쓴 이름을 찾을 스키마 (탭에서 바꾼 것)

    QString connectionId() const { return m_connectionId; }
    QString schema() const { return m_schema; }
    QString name() const { return m_name; }
    bool isLoaded() const { return m_loaded; }
    QString errorText() const { return m_error; }
    QTabWidget *pages() const { return m_pages; }
    void refresh();

signals:
    void loaded(const QString &title); // 실제 이름(대소문자)을 알았을 때
    void selectRowsRequested(const QString &schema, const QString &name);
    void importRequested(const QString &schema, const QString &name);

private:
    void showTable(const QCborMap &table);
    void loadDdl();

    QPointer<MetadataService> m_metadata;
    QString m_connectionId;
    QString m_schema;
    QString m_name;
    QString m_currentSchema;
    bool m_schemaQuoted = false;
    bool m_nameQuoted = false;
    SqlDialect m_dialect;
    bool m_loaded = false;
    bool m_ddlLoaded = false;
    QString m_error;

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QTabWidget *m_pages = nullptr;
    QTableWidget *m_columns = nullptr;
    QTableWidget *m_indexes = nullptr;
    QTableWidget *m_foreignKeys = nullptr;
    QTableWidget *m_referencedBy = nullptr;
    QPlainTextEdit *m_ddl = nullptr;
};
