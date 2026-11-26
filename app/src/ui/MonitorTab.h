#pragma once

#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"

#include <QCborMap>
#include <QHash>
#include <QWidget>

#include <functional>

class EngineClient;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QuerySession;
struct EngineError;

// 모니터 탭: 세션, 락 대기, 용량(테이블스페이스·DB·큰 객체).
// 자기 DB 세션을 따로 열어(자동 커밋) 쿼리 탭이 실행 중이어도 본다. 세션·락은 정한 간격마다 다시 읽고
// (탭이 보일 때만), 용량은 열 때와 Refresh를 누를 때 읽는다.
// 세션 종료·문장 취소는 확인을 받고 하며, 읽기 전용 연결에서는 막는다 (엔진 Monitor: protocol/README.md).
class MonitorTab : public QWidget
{
    Q_OBJECT

public:
    enum class Page { Sessions, Locks, Space };

    MonitorTab(EngineClient *engine, const ConnectionProfile &profile, QWidget *parent = nullptr);
    ~MonitorTab() override;

    // 접속하고 지금 페이지를 읽는다. 끊기면 Refresh 때 같은 정보로 다시 접속한다.
    void start(const ConnectSpec &spec);
    void showPage(Page page);
    Page page() const;
    void refresh(); // 지금 페이지를 다시 읽는다

    QuerySession *session() const { return m_session; }
    QTableWidget *sessionsTable() const { return m_sessions; }
    QTreeWidget *waitsTree() const { return m_waits; }
    QTableWidget *heldTable() const { return m_held; }
    QTableWidget *tablespacesTable() const { return m_tablespaces; }
    QTableWidget *databasesTable() const { return m_databases; }
    QTableWidget *objectsTable() const { return m_objects; }
    QString statusText() const;
    QString selectedSessionId() const;
    void selectSession(const QString &id);

signals:
    void openSqlRequested(const QString &sql); // 고른 세션의 SQL을 새 쿼리 탭에

protected:
    void showEvent(QShowEvent *event) override;

private:
    QWidget *buildSessionsPage();
    QWidget *buildLocksPage();
    QWidget *buildSpacePage();
    void connectThen(std::function<void()> work, std::function<void()> failed = {});
    void load(Page page);
    void showSessions(const QCborMap &result);
    void showLocks(const QCborMap &result);
    void showSpace(const QCborMap &result);
    void showError(Page page, const EngineError &error);
    void applySessionFilter();
    void updateSessionDetails();
    void updateButtons();
    void kill(const QString &id, bool cancelQuery);
    QString lockSelectionId() const;
    void setStatus(const QString &text, bool error = false);
    void setAutoRefresh(int seconds);

    QuerySession *m_session = nullptr;
    ConnectionProfile m_profile;
    ConnectSpec m_spec;
    bool m_started = false;
    bool m_loading[3] = {false, false, false};
    bool m_spaceLoaded = false;
    QHash<QString, QCborMap> m_sessionData; // 세션 id → 엔진이 준 값

    QPushButton *m_refresh = nullptr;
    QComboBox *m_autoRefresh = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_timer = nullptr;
    QTabWidget *m_pages = nullptr;

    // 세션
    QLineEdit *m_filter = nullptr;
    QCheckBox *m_activeOnly = nullptr;
    QCheckBox *m_showBackground = nullptr;
    QLabel *m_sessionCount = nullptr;
    QLabel *m_sessionNotes = nullptr;
    QTableWidget *m_sessions = nullptr;
    QPlainTextEdit *m_sessionSql = nullptr;
    QPushButton *m_kill = nullptr;
    QPushButton *m_cancelQuery = nullptr;
    QPushButton *m_openSql = nullptr;

    // 락
    QLabel *m_locksSummary = nullptr;
    QTreeWidget *m_waits = nullptr;
    QTableWidget *m_held = nullptr;
    QLabel *m_locksError = nullptr;
    QPushButton *m_killLocker = nullptr;

    // 용량
    QLabel *m_spaceSummary = nullptr;
    QLabel *m_tablespacesTitle = nullptr;
    QLabel *m_databasesTitle = nullptr;
    QLabel *m_objectsTitle = nullptr;
    QTableWidget *m_tablespaces = nullptr;
    QTableWidget *m_databases = nullptr;
    QTableWidget *m_objects = nullptr;
};
