#pragma once

#include "core/ConnectionStore.h"
#include "core/QueryHistory.h"
#include "core/SqlBinds.h"
#include "core/WorkspaceStore.h"

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>

#include <functional>

class EngineClient;
class MetadataService;
class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QTabWidget;
class QTimer;
class QToolButton;
class ObjectExplorer;
class QueryTab;
struct SqlStatement;

// 메인 창 (IntelliJ식 배치).
// - 위: 메인 툴바. 왼쪽은 연결 선택, 오른쪽은 실행·중지·트랜잭션
// - 왼쪽 끝: 도구 창 막대 (Database Explorer 열기/닫기)
// - 가운데: 쿼리 탭 (에디터 + 결과, 탭마다 별도 DB 세션). 탭은 연결마다 따로 두고,
//   툴바나 탐색기에서 연결을 바꾸면 그 연결로 마지막에 쓰던 탭으로 옮겨 간다.
// - 아래: 상태 표시줄
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // SQL 파일을 연다. 이미 열린 파일이면 그 탭으로 옮겨 간다. 연 파일 수를 돌려준다.
    int openFiles(const QStringList &paths);
    // 탭을 저장한다. path가 비어 있으면 탭의 파일에 (파일이 없으면 저장 대화상자). 저장했으면 true.
    bool saveTab(QueryTab *tab, const QString &path = {});
    // 열린 탭(SQL, 파일, 연결)을 작업 공간 파일에 남긴다. 편집하면 잠시 뒤 저절로 불린다.
    void saveWorkspace();
    // 실행 기록 (F8)
    const QueryHistory &history() const { return m_history; }
    void showHistory();

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
    bool restoreWorkspace(); // 남겨 둔 탭을 다시 연다. 하나라도 열었으면 true
    void scheduleWorkspaceSave();
    void openFileDialog();
    bool saveTabAs(QueryTab *tab);
    void updateFileTabTitle(QueryTab *tab);
    bool confirmCloseUnsaved(QueryTab *tab);

    QueryTab *currentQueryTab() const;
    QList<QueryTab *> queryTabs() const;
    QueryTab *addQueryTab(); // 지금 탭과 같은 연결로 새 탭
    QueryTab *addQueryTabFor(const QString &connectionId);
    void closeQueryTab(int index);
    void openObject(const QString &connectionId, const QString &schema, const QString &name);

    void executeCurrent(bool wholeScript);
    // 바인드 변수가 있으면 값을 물어 문장에 넣는다. 취소하면 false.
    bool applyBindVariables(QueryTab *tab, QList<SqlStatement> *statements);
    void insertSql(QueryTab *tab, const QString &sql);
    void formatCurrent(); // 선택 영역(없으면 커서가 있는 문장)을 정렬한다
    void explainCurrent();
    void useSchema(const QString &connectionId, const QString &schema);
    void showDdlAtCursor();
    void requestDdl(const QString &connectionId, const QString &schema, const QString &name, bool schemaQuoted,
                    bool nameQuoted, QueryTab *source);
    void openDdlTab(const QString &connectionId, const QCborMap &result);
    // 탭의 세션을 그 탭의 연결에 붙인 뒤(필요하면 비밀번호를 묻고 접속) work를 부른다.
    void withConnectedSession(QueryTab *tab, std::function<void()> work);
    bool requireConnection(QueryTab *tab);
    bool confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements);
    bool confirmDiscardTransaction(QueryTab *tab, const QString &action);
    bool passwordFor(const ConnectionProfile &profile, QString *password, bool interactive = true);

    void openConnectionDialog(bool startNew, const QString &selectId = {});
    void refreshConnectionViews();
    void activateConnection(const QString &connectionId);
    QueryTab *recentTabFor(const QString &connectionId) const;
    void updateTabTitle(QueryTab *tab);
    QString uniqueTabTitle(const QString &base, const QueryTab *except) const;
    void setTabConnection(QueryTab *tab, const QString &connectionId);
    void syncConnectionComboToCurrentTab();
    void updateConnectionStatus();
    void updateActionStates();
    void updateCursorPosition(int line, int column);
    void updateTransactionLabel();

    ConnectionStore m_store;
    WorkspaceStore m_workspaceStore;
    QueryHistory m_history;
    QHash<QString, SqlBinds::Value> m_bindValues; // 바인드 변수마다 마지막에 넣은 값 (앱이 켜져 있는 동안)
    QTimer *m_workspaceTimer = nullptr;
    bool m_restoringWorkspace = false;
    EngineClient *m_engine = nullptr;
    MetadataService *m_metadata = nullptr; // Explorer와 자동완성이 함께 쓰는 메타데이터 세션·캐시
    QHash<QString, QString> m_sessionPasswords; // 저장하지 않은 비밀번호는 앱이 켜져 있는 동안만 기억한다

    QTabWidget *m_tabs = nullptr;
    QDockWidget *m_explorerDock = nullptr;
    ObjectExplorer *m_explorer = nullptr;
    QComboBox *m_connectionCombo = nullptr;
    QToolButton *m_transactionButton = nullptr;
    bool m_syncingCombo = false;

    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_openFileAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_replaceAction = nullptr;
    QAction *m_findNextAction = nullptr;
    QAction *m_findPreviousAction = nullptr;
    QAction *m_formatAction = nullptr;
    QAction *m_historyAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_quitAction = nullptr;
    QAction *m_newConnectionAction = nullptr;
    QAction *m_manageConnectionsAction = nullptr;
    QAction *m_executeAction = nullptr;
    QAction *m_executeScriptAction = nullptr;
    QAction *m_explainAction = nullptr;
    QAction *m_ddlAction = nullptr;
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
    QList<QPointer<QueryTab>> m_tabHistory; // 최근에 쓴 탭이 앞
};
