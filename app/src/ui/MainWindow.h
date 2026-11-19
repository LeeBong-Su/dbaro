#pragma once

#include "core/ConnectionProfile.h"
#include "core/QueryHistory.h"
#include "core/SqlBinds.h"
#include "core/WorkspaceStore.h"

#include <QCborMap>
#include <QList>
#include <QMainWindow>
#include <QPointer>

#include <functional>

class AppController;
class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;
class QTimer;
class QToolButton;
class ObjectExplorer;
class QueryTab;
struct SqlStatement;

// 연결 하나의 창 (IntelliJ식 배치). 연결마다 창이 따로 열리고, 창 안의 쿼리 탭은 모두 그 연결을 쓴다.
// - 위: 메인 툴바. 왼쪽은 연결 선택 창·새 탭, 오른쪽은 실행·중지·트랜잭션
// - 왼쪽 끝: 도구 창 막대 (Database Explorer: 이 연결의 스키마·객체)
// - 가운데: 쿼리 탭 (에디터 + 결과, 탭마다 별도 DB 세션)
// - 아래: 상태 표시줄
// 창 제목은 연결 이름이고, 창 테두리는 연결의 환경 색이다 (운영은 빨강, Windows 11).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(AppController *app, const QString &connectionId, QWidget *parent = nullptr);
    ~MainWindow() override;

    QString connectionId() const { return m_connectionId; }
    const ConnectionProfile *profile() const;
    // 이 연결의 크기·위치를 기억하고 있는지 (없으면 새 창을 조금 비켜 놓는다)
    bool hasSavedGeometry() const { return m_hasSavedGeometry; }

    // SQL 파일을 연다. 이미 열린 파일이면 그 탭으로 옮겨 간다. 연 파일 수를 돌려준다.
    int openFiles(const QStringList &paths);
    // 탭을 저장한다. path가 비어 있으면 탭의 파일에 (파일이 없으면 저장 대화상자). 저장했으면 true.
    bool saveTab(QueryTab *tab, const QString &path = {});
    // 열린 탭(SQL, 파일)을 작업 공간 파일에 남긴다. 편집하면 잠시 뒤 저절로 불린다.
    void saveWorkspace();
    // 실행 기록 (F8)
    const QueryHistory &history() const;
    void showHistory();
    // 연결 설정이 바뀌었을 때 (이름, 환경 색, DB 종류)
    void refreshConnection();

signals:
    void windowClosed();

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void createActions();
    void createDocks();
    void createMenus();
    void createToolBar();
    void createToolWindowStripe();
    void createStatusBar();
    void restoreLayout();
    bool restoreWorkspace(); // 남겨 둔 탭을 다시 연다. 하나라도 열었으면 true
    void scheduleWorkspaceSave();
    void openFileDialog();
    bool saveTabAs(QueryTab *tab);
    void updateFileTabTitle(QueryTab *tab);
    bool confirmCloseUnsaved(QueryTab *tab);

    QueryTab *currentQueryTab() const;
    QList<QueryTab *> queryTabs() const;
    QueryTab *addQueryTab();
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
    // 탭의 세션을 이 창의 연결에 붙인 뒤(필요하면 비밀번호를 묻고 접속) work를 부른다.
    void withConnectedSession(QueryTab *tab, std::function<void()> work);
    bool requireConnection(QueryTab *tab);
    bool confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements);
    bool confirmDiscardTransaction(QueryTab *tab, const QString &action);

    void applyConnection(QueryTab *tab);
    void applyWindowColor();
    void updateConnectionStatus();
    void updateActionStates();
    void updateCursorPosition(int line, int column);
    void updateTransactionLabel();

    static constexpr int kFrameWidth = 4; // 창 안쪽 환경 색 띠 두께 (논리 픽셀)

    AppController *m_app = nullptr;
    QString m_connectionId;
    QColor m_frameColor; // 연결의 환경 색 (환경이 없으면 유효하지 않음 → 띠 없음)
    bool m_hasSavedGeometry = false;
    QTimer *m_workspaceTimer = nullptr;
    bool m_restoringWorkspace = false;

    QTabWidget *m_tabs = nullptr;
    QDockWidget *m_explorerDock = nullptr;
    ObjectExplorer *m_explorer = nullptr;
    QToolButton *m_transactionButton = nullptr;

    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_openFileAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_closeWindowAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_replaceAction = nullptr;
    QAction *m_findNextAction = nullptr;
    QAction *m_findPreviousAction = nullptr;
    QAction *m_formatAction = nullptr;
    QAction *m_historyAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_quitAction = nullptr;
    QAction *m_newConnectionAction = nullptr;
    QAction *m_editConnectionAction = nullptr;
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
};
