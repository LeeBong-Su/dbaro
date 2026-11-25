#pragma once

#include "core/ConnectionProfile.h"
#include "core/QueryHistory.h"
#include "core/SqlBinds.h"
#include "core/WorkspaceStore.h"

#include <QCborMap>
#include <QElapsedTimer>
#include <QHash>
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
class ConsoleTab;
class ObjectExplorer;
class PlanObjectWindow;
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
    // 새 쿼리 탭. 지금 보고 있는 DB(탐색기와 같은 DB)로 접속한다
    QueryTab *addQueryTab();
    QueryTab *addQueryTabOn(const QString &database); // 비우면 접속 설정의 DB
    // metadataId(연결 + DB)의 DB에 붙은 지금 쿼리 탭. 지금 탭이 다른 DB면 그 DB로 새 탭을 연다
    QueryTab *queryTabFor(const QString &metadataId);
    // CLI 탭 (dbaro-cli 대화형을 창 안에서). 지금 쿼리 탭의 스키마로 시작한다.
    ConsoleTab *openConsole();
    void closeQueryTab(int index);
    void selectAdjacentTab(int step); // 다음(+1)·이전(-1) 탭. 끝에서는 반대쪽 끝으로
    void openObject(const QString &metadataId, const QString &schema, const QString &name);

    void executeCurrent(bool wholeScript);
    // 바인드 변수가 있으면 값을 물어 문장에 넣는다. 취소하면 false.
    bool applyBindVariables(QueryTab *tab, QList<SqlStatement> *statements);
    void insertSql(QueryTab *tab, const QString &sql);
    void formatCurrent(); // 선택 영역(없으면 커서가 있는 문장)을 정렬한다
    void explainCurrent(bool actual);
    void useSchema(const QString &metadataId, const QString &schema); // 탐색기에서: 지금 탭의 스키마를 바꾼다
    void switchSchema(QueryTab *tab, const QString &schema);
    // PostgreSQL: 탭이 접속하는 DB를 바꾼다 (세션 안에서 못 바꿔서 그 DB로 다시 접속한다)
    void switchDatabase(QueryTab *tab, const QString &database);
    void showDdlAtCursor();
    void showTableDetailsAtCursor();
    void openTableDetails(const QString &metadataId, const QString &schema, const QString &name, bool schemaQuoted,
                          bool nameQuoted, const QString &currentSchema = {});
    // 엑셀·CSV 불러오기. table이 비면 새 테이블
    void importData(const QString &metadataId, const QString &schema, const QString &table);
    void runImport(QueryTab *tab, const QStringList &statements, bool stopOnError, const QString &target);
    void requestDdl(const QString &connectionId, const QString &schema, const QString &name, bool schemaQuoted,
                    bool nameQuoted, QueryTab *source);
    void openDdlTab(const QString &connectionId, const QCborMap &result);
    // 실행 계획 단계가 읽는 객체(테이블·인덱스) 정보를 별도 창에 (창은 하나를 계속 쓴다)
    void showPlanObject(QueryTab *tab, const QCborMap &object);
    // 탭의 세션을 이 창의 연결에 붙인 뒤(필요하면 비밀번호를 묻고 접속) work를 부른다.
    void withConnectedSession(QueryTab *tab, std::function<void()> work);
    // 창을 열거나 탭을 고르면 그 탭을 바로 접속한다 (탐색기도 펼친다). 비밀번호 입력을 취소했거나
    // 접속에 실패하면 다시 실행할 때까지 저절로 접속하지 않는다.
    void connectCurrentTab();
    bool requireConnection(QueryTab *tab);
    bool confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements);
    bool confirmDiscardTransaction(QueryTab *tab, const QString &action);

    void applyConnection(QueryTab *tab);
    // 이 연결을 database로 접속하는 사본 (id는 그대로, 비밀번호를 같은 id로 찾는다). 비우면 접속 설정 그대로
    ConnectionProfile profileOn(const QString &database) const;
    // 지금 보고 있는 DB의 메타데이터 id (지금 쿼리 탭의 DB. 탐색기도 이 DB를 보인다)
    QString activeMetadataId() const;
    // 탐색기를 지금 보고 있는 DB로 맞춘다 (펼쳐 있었으면 다시 펼친다). force면 같은 DB여도 다시 만든다
    void syncExplorerDatabase(bool force = false);
    void applyWindowColor();
    void updateConnectionStatus();
    void updateActionStates();
    void updateCursorPosition(int line, int column);
    void updateTransactionLabel();

    static constexpr int kFrameWidth = 4; // 창 안쪽 환경 색 띠 두께 (논리 픽셀)

    AppController *m_app = nullptr;
    QString m_connectionId;
    QString m_activeDatabase; // 지금 보고 있는 DB (지금 쿼리 탭의 DB, 비면 접속 설정의 DB)
    QString m_explorerId;     // 탐색기가 보이는 메타데이터 id
    QString m_planObjectSource; // 객체 정보 창이 보이는 객체의 메타데이터 id
    QColor m_frameColor; // 연결의 환경 색 (환경이 없으면 유효하지 않음 → 띠 없음)
    bool m_hasSavedGeometry = false;
    QTimer *m_workspaceTimer = nullptr;
    bool m_restoringWorkspace = false;
    bool m_shown = false;              // 한 번이라도 화면에 나왔는지 (그 뒤부터 탭을 고르면 바로 접속)
    bool m_autoConnectBlocked = false; // 비밀번호 입력 취소·접속 실패 뒤에는 저절로 접속하지 않는다

    QTabWidget *m_tabs = nullptr;
    QDockWidget *m_explorerDock = nullptr;
    ObjectExplorer *m_explorer = nullptr;
    QPointer<PlanObjectWindow> m_planObjectWindow;
    QToolButton *m_transactionButton = nullptr;

    QAction *m_newTabAction = nullptr;
    QAction *m_consoleAction = nullptr;
    QAction *m_scriptFoldersAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_nextTabAction = nullptr;
    QAction *m_previousTabAction = nullptr;
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
    QAction *m_explainActualAction = nullptr;
    // 실행 중인 탭: 탭 아이콘 자리에 돌아가는 원호 (오래 걸릴 때만)
    QTimer *m_busyTabTimer = nullptr;
    int m_busyTabAngle = 0;
    QHash<QueryTab *, QElapsedTimer> m_busyTabs;
    void updateBusyTab(QueryTab *tab);
    void animateBusyTabs();
    QAction *m_ddlAction = nullptr;
    QAction *m_tableDetailsAction = nullptr;
    QAction *m_importAction = nullptr;
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
