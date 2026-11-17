#pragma once

#include "core/ConnectionStore.h"

#include <QMainWindow>

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QTabWidget;
class ObjectExplorer;
class QueryTab;

// 메인 창. 왼쪽 객체 탐색기, 가운데 쿼리 탭(에디터 + 결과), 위쪽 툴바, 아래 상태 표시줄.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createDocks();
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void restoreLayout();
    void loadConnections();

    QueryTab *currentQueryTab() const;
    QueryTab *addQueryTab();
    void closeQueryTab(int index);
    void executeCurrent(bool wholeScript);

    void openConnectionDialog(bool startNew);
    void refreshConnectionViews();
    void setTabConnection(QueryTab *tab, const QString &connectionId);
    void syncConnectionComboToCurrentTab();
    void updateConnectionStatus();
    void updateCursorPosition(int line, int column);
    void updateTransactionLabel();

    ConnectionStore m_store;

    QTabWidget *m_tabs = nullptr;
    QDockWidget *m_explorerDock = nullptr;
    ObjectExplorer *m_explorer = nullptr;
    QComboBox *m_connectionCombo = nullptr;
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
    QAction *m_aboutAction = nullptr;

    QLabel *m_connectionLabel = nullptr;
    QLabel *m_transactionLabel = nullptr;
    QLabel *m_cursorLabel = nullptr;
    QLabel *m_encodingLabel = nullptr;

    int m_tabCounter = 0;
};
