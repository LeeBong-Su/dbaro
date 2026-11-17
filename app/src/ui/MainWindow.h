#pragma once

#include "core/ConnectionStore.h"

#include <QHash>
#include <QMainWindow>

class EngineClient;
class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QTabWidget;
class QToolButton;
class ObjectExplorer;
class QueryTab;
struct SqlStatement;

// 메인 창 (IntelliJ식 배치).
// - 위: 메인 툴바. 왼쪽은 연결 선택, 오른쪽은 실행·중지·트랜잭션
// - 왼쪽 끝: 도구 창 막대 (Database Explorer 열기/닫기)
// - 가운데: 쿼리 탭 (에디터 + 결과, 탭마다 별도 DB 세션)
// - 아래: 상태 표시줄
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createDocks();
    void createMenus();
    void createToolBar();
    void createToolWindowStripe();
    void createStatusBar();
    void restoreLayout();
    void loadConnections();

    QueryTab *currentQueryTab() const;
    QList<QueryTab *> queryTabs() const;
    QueryTab *addQueryTab();
    void closeQueryTab(int index);

    void executeCurrent(bool wholeScript);
    bool confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements);
    bool confirmDiscardTransaction(QueryTab *tab, const QString &action);
    bool passwordFor(const ConnectionProfile &profile, QString *password);

    void openConnectionDialog(bool startNew);
    void refreshConnectionViews();
    void chooseConnection(QueryTab *tab, const QString &connectionId);
    void setTabConnection(QueryTab *tab, const QString &connectionId);
    void syncConnectionComboToCurrentTab();
    void updateConnectionStatus();
    void updateActionStates();
    void updateCursorPosition(int line, int column);
    void updateTransactionLabel();

    ConnectionStore m_store;
    EngineClient *m_engine = nullptr;
    QHash<QString, QString> m_sessionPasswords; // 저장하지 않은 비밀번호는 앱이 켜져 있는 동안만 기억한다

    QTabWidget *m_tabs = nullptr;
    QDockWidget *m_explorerDock = nullptr;
    ObjectExplorer *m_explorer = nullptr;
    QComboBox *m_connectionCombo = nullptr;
    QToolButton *m_transactionButton = nullptr;
    bool m_syncingCombo = false;

    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_quitAction = nullptr;
    QAction *m_newConnectionAction = nullptr;
    QAction *m_manageConnectionsAction = nullptr;
    QAction *m_executeAction = nullptr;
    QAction *m_executeScriptAction = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_commitAction = nullptr;
    QAction *m_rollbackAction = nullptr;
    QAction *m_autoCommitAction = nullptr;
    QAction *m_darkThemeAction = nullptr;
    QAction *m_lightThemeAction = nullptr;
    QAction *m_aboutAction = nullptr;

    QLabel *m_connectionLabel = nullptr;
    QLabel *m_transactionLabel = nullptr;
    QLabel *m_cursorLabel = nullptr;
    QLabel *m_encodingLabel = nullptr;

    int m_tabCounter = 0;
};
