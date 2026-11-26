#pragma once

#include "ErdView.h"
#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"
#include "core/ResultExport.h"

#include <QCborMap>
#include <QHash>
#include <QWidget>

#include <functional>

class BusyOverlay;
class EngineClient;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QuerySession;
struct EngineError;

// 리버스 엔지니어링 탭: 스키마 하나(또는 고른 테이블)의 ERD, 테이블 정의, 스키마 DDL.
// 자기 세션을 따로 연다(자동 커밋). 모델은 열 때 엔진 reverseModel로 한 번에 읽고,
// DDL(schemaDdl)은 DDL 페이지를 처음 볼 때 만든다 (오래 걸리면 Cancel).
// 내보내기: ERD는 PNG·SVG, 테이블 정의는 엑셀·CSV·Markdown, DDL은 .sql 파일이나 새 쿼리 탭.
class ReverseTab : public QWidget
{
    Q_OBJECT

public:
    enum class Page { Diagram, Tables, Ddl };

    ReverseTab(EngineClient *engine, const ConnectionProfile &profile, const QString &schema, const QStringList &tables,
               bool includeViews, QWidget *parent = nullptr);
    ~ReverseTab() override;

    void start(const ConnectSpec &spec);
    void showPage(Page page);
    void reload(); // 모델을 다시 읽는다 (DDL도 다음에 볼 때 다시 만든다)
    void generateDdl();

    QString schema() const { return m_schema; }
    bool isLoaded() const { return m_loaded; }
    bool isDdlLoaded() const { return m_ddlLoaded; }
    QString statusText() const;
    ErdView *erd() const { return m_erd; }
    QTableWidget *tableList() const { return m_tableList; }
    QTableWidget *columnsTable() const { return m_columns; }
    QPlainTextEdit *ddlEditor() const { return m_ddl; }
    QuerySession *session() const { return m_session; }

    // 테이블 정의서 (테이블마다 컬럼 한 줄씩)
    QByteArray tableDefinition(ExportOptions::Format format) const;
    void selectTable(const QString &name);

signals:
    void openSqlRequested(const QString &sql, const QString &title);

private:
    QWidget *buildDiagramPage();
    QWidget *buildTablesPage();
    QWidget *buildDdlPage();
    void connectThen(std::function<void()> work, std::function<void()> failed);
    void showModel(const QCborMap &result);
    void showTableDetails(const QString &name);
    void applyTableFilter();
    void exportImage();
    void exportDefinition();
    void saveDdl();
    void setStatus(const QString &text, bool error = false);
    QCborMap tableModel(const QString &name) const;

    QuerySession *m_session = nullptr;
    ConnectionProfile m_profile;
    ConnectSpec m_spec;
    QString m_schema;
    QStringList m_only;
    bool m_includeViews = true;
    bool m_loaded = false;
    bool m_loading = false;
    bool m_ddlLoaded = false;
    bool m_ddlLoading = false;
    QCborMap m_model;
    QHash<QString, int> m_tableIndex; // 이름 → m_model tables 위치

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
    QTabWidget *m_pages = nullptr;

    // ERD
    ErdView *m_erd = nullptr;
    QLineEdit *m_find = nullptr;
    QComboBox *m_detail = nullptr;
    QLabel *m_zoom = nullptr;
    BusyOverlay *m_erdBusy = nullptr;

    // 테이블 정의
    QLineEdit *m_tableFilter = nullptr;
    QTableWidget *m_tableList = nullptr;
    QLabel *m_tableTitle = nullptr;
    QTableWidget *m_columns = nullptr;
    QTableWidget *m_indexes = nullptr;
    QTableWidget *m_foreignKeys = nullptr;

    // DDL
    QLabel *m_ddlInfo = nullptr;
    QPlainTextEdit *m_ddl = nullptr;
    BusyOverlay *m_ddlBusy = nullptr;
    QPushButton *m_saveDdl = nullptr;
    QPushButton *m_openDdl = nullptr;
};
