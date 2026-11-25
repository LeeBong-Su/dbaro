#include "MainWindow.h"
#include "MetadataService.h"

#include "AppController.h"
#include "ConnectionBar.h"
#include "ConsoleTab.h"
#include "ScriptFoldersDialog.h"
#include "FontZoom.h"
#include "cli/CliApp.h"
#include "PlanObjectWindow.h"
#include "BindDialog.h"
#include "BusyIndicator.h"
#include "EditorTabs.h"
#include "FindBar.h"
#include "HistoryDialog.h"
#include "ImportDialog.h"
#include "TableDetailTab.h"
#include "Icons.h"
#include "ObjectExplorer.h"
#include "QuerySession.h"
#include "QueryTab.h"
#include "ResultPanel.h"
#include "SqlEditor.h"
#include "Theme.h"
#include "UiHelpers.h"
#include "WindowsIntegration.h"
#include "core/ConnectSpec.h"
#include "core/SqlIdentifiers.h"
#include "core/SqlDialect.h"
#include "core/SqlFormatter.h"
#include "core/SqlSafety.h"
#include "core/TextFile.h"
#include "ipc/EngineClient.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include <memory>

namespace {
const auto kGeometryKey = QStringLiteral("mainWindow/geometry");
const auto kStateKey = QStringLiteral("mainWindow/state");
const auto kLastDirKey = QStringLiteral("files/lastDir");

QString lastFileDir()
{
    return QSettings().value(kLastDirKey, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
}

// 탭 제목으로 만든 파일 이름 (파일 이름에 쓸 수 없는 글자는 뺀다)
QString fileNameFromTitle(const QString &title)
{
    QString name = title;
    name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("query") : name;
}

QString withShortcut(const QString &text, const QKeySequence &shortcut)
{
    return QStringLiteral("%1 (%2)").arg(text, shortcut.toString(QKeySequence::NativeText));
}

QString preview(const QString &sql)
{
    QString text = sql.simplified();
    return text.size() > 120 ? text.left(117) + QStringLiteral("...") : text;
}

// 도구 창 제목 줄: 제목과 숨기기(−) 버튼. 제목 줄을 끌면 도구 창을 옮길 수 있다.
QWidget *createDockTitleBar(QDockWidget *dock)
{
    auto *bar = new QWidget(dock);
    bar->setObjectName(QStringLiteral("dockTitleBar"));
    bar->setAttribute(Qt::WA_StyledBackground);

    auto *title = new QLabel(dock->windowTitle(), bar);
    title->setObjectName(QStringLiteral("dockTitle"));

    auto *hide = new QToolButton(bar);
    hide->setIcon(Icons::get(QStringLiteral("hide")));
    hide->setIconSize(QSize(14, 14));
    hide->setAutoRaise(true);
    hide->setFocusPolicy(Qt::NoFocus);
    hide->setToolTip(QObject::tr("Hide"));
    QObject::connect(hide, &QToolButton::clicked, dock, &QDockWidget::hide);

    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 3, 4, 3);
    layout->setSpacing(2);
    layout->addWidget(title);
    layout->addStretch();
    layout->addWidget(hide);
    return bar;
}
} // namespace

MainWindow::MainWindow(AppController *app, const QString &connectionId, QWidget *parent)
    : QMainWindow(parent)
    , m_app(app)
    , m_connectionId(connectionId)
{
    setWindowTitle(QStringLiteral("DBaro"));
    resize(1280, 800);

    connect(m_app->engine(), &EngineClient::engineLog, this, [this](const QString &line) {
        // 사용자에게 의미 있는 줄(드라이버 다운로드, 오류)만 상태 표시줄에 보여 준다.
        QString text = line;
        text.remove(QStringLiteral("[engine] "));
        if (text.startsWith(QStringLiteral("Downloading")) || text.startsWith(QStringLiteral("Saved"))
            || text.startsWith(QStringLiteral("ERROR")))
            statusBar()->showMessage(text, 8000);
    });

    m_tabs = new EditorTabs(this);
    setCentralWidget(m_tabs);

    // 열린 탭은 편집이 멈추고 잠시 뒤에 남긴다 (앱이 갑자기 꺼져도 SQL을 잃지 않게).
    m_workspaceTimer = new QTimer(this);
    m_workspaceTimer->setSingleShot(true);
    m_workspaceTimer->setInterval(1500);
    connect(m_workspaceTimer, &QTimer::timeout, this, &MainWindow::saveWorkspace);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, &MainWindow::scheduleWorkspaceSave);

    createActions();
    createDocks();
    createMenus();
    createToolBar();
    createToolWindowStripe();
    createStatusBar();

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeQueryTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (QueryTab *tab = currentQueryTab()) {
            const QTextCursor cursor = tab->editor()->textCursor();
            updateCursorPosition(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
            m_encodingLabel->setText(tab->filePath().isEmpty() ? QStringLiteral("UTF-8")
                                                               : TextFile::encodingName(tab->fileFormat()));
            // 탐색기와 새 탭은 지금 탭의 DB를 따른다 (PostgreSQL에서 DB를 바꾼 탭)
            m_activeDatabase = tab->database();
            syncExplorerDatabase();
        }
        updateConnectionStatus();
        updateActionStates();
        scheduleWorkspaceSave();
        if (m_shown)
            connectCurrentTab();
    });
    connect(m_app, &AppController::connectionsChanged, this, &MainWindow::refreshConnection);

    refreshConnection();
    if (!restoreWorkspace())
        addQueryTab();
    restoreLayout();
    updateActionStates();
}

const ConnectionProfile *MainWindow::profile() const
{
    return m_app->profile(m_connectionId);
}

const QueryHistory &MainWindow::history() const
{
    return m_app->history();
}

void MainWindow::refreshConnection()
{
    const ConnectionProfile *p = profile();
    if (!p)
        return; // 지운 연결: AppController가 창을 닫는다
    setWindowTitle(QStringLiteral("%1 — DBaro").arg(p->name));
    for (QueryTab *tab : queryTabs())
        applyConnection(tab);
    if (!Db::canSwitchDatabase(p->type) || m_activeDatabase == p->database)
        m_activeDatabase.clear();
    syncExplorerDatabase(true);
    applyWindowColor();
    updateConnectionStatus();
    updateActionStates();
}

void MainWindow::applyWindowColor()
{
    // 창 테두리를 연결의 환경 색으로 (운영은 빨강). 작업 표시줄 미리보기에서도 보인다.
    // OS 테두리(1px, Windows 11)는 두께를 바꿀 수 없어서 창 안쪽 가장자리에도 같은 색 띠를 그린다.
    // 내용은 띠만큼 안쪽으로 들여서 스크롤바나 글자를 가리지 않는다.
    const ConnectionProfile *p = profile();
    m_frameColor = p ? Env::color(p->environment) : QColor();
    const int frame = m_frameColor.isValid() ? kFrameWidth : 0;
    setContentsMargins(frame, frame, frame, frame);
    WindowsIntegration::setBorderColor(this, m_frameColor);
    update();
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);
    if (!m_frameColor.isValid())
        return;
    QPainter painter(this);
    const QRect outer = rect();
    const QRect inner = contentsRect();
    painter.fillRect(QRect(outer.left(), outer.top(), outer.width(), inner.top() - outer.top()), m_frameColor);
    painter.fillRect(QRect(outer.left(), inner.bottom() + 1, outer.width(), outer.bottom() - inner.bottom()), m_frameColor);
    painter.fillRect(QRect(outer.left(), inner.top(), inner.left() - outer.left(), inner.height()), m_frameColor);
    painter.fillRect(QRect(inner.right() + 1, inner.top(), outer.right() - inner.right(), inner.height()), m_frameColor);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    applyWindowColor(); // 창 핸들이 생긴 뒤에야 칠할 수 있다
    if (!m_shown) {
        m_shown = true;
        // 창이 그려진 뒤에 접속한다 (비밀번호를 물어야 하면 창 위에 뜬다).
        QTimer::singleShot(0, this, &MainWindow::connectCurrentTab);
    }
}

MainWindow::~MainWindow()
{
    // 탭(세션)이 엔진보다 먼저 정리되도록 탭을 먼저 지운다.
    while (m_tabs->count() > 0) {
        QWidget *widget = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete widget;
    }
}

namespace {
// 탭 제목이 어디서 왔는지: 빈 값이면 "Query N", 연결 id면 그 연결 이름, kFixedTitle이면 따로 정한 제목
constexpr char kTitleSource[] = "dbaroTitleSource";
constexpr char kFixedTitle[] = "fixed";
} // namespace

void MainWindow::createActions()
{
    m_newTabAction = new QAction(Icons::get(QStringLiteral("add")), tr("New Query Tab"), this);
    m_newTabAction->setShortcut(QKeySequence::AddTab);
    m_newTabAction->setToolTip(withShortcut(tr("New query tab"), m_newTabAction->shortcut()));
    connect(m_newTabAction, &QAction::triggered, this, [this] { addQueryTab(); });

    m_consoleAction = new QAction(Icons::get(QStringLiteral("console")), tr("New CLI Tab"), this);
    m_consoleAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_F12));
    m_consoleAction->setToolTip(withShortcut(tr("New CLI tab: run SQL like dbaro-cli in this window"), m_consoleAction->shortcut()));
    connect(m_consoleAction, &QAction::triggered, this, [this] { openConsole(); });

    // 탭 옮겨 가기: 에디터·CLI·탐색기 어디에 있든 창 전체에서 쓴다
    m_nextTabAction = new QAction(tr("Next Tab"), this);
    m_nextTabAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)});
    connect(m_nextTabAction, &QAction::triggered, this, [this] { selectAdjacentTab(1); });
    m_previousTabAction = new QAction(tr("Previous Tab"), this);
    m_previousTabAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab),
                                       QKeySequence(Qt::CTRL | Qt::Key_PageUp)});
    connect(m_previousTabAction, &QAction::triggered, this, [this] { selectAdjacentTab(-1); });
    addAction(m_nextTabAction);
    addAction(m_previousTabAction);

    // Ctrl + 휠로 글자 크기를 바꾸면 상태 표시줄에 알린다 (지금 쓰는 창만)
    connect(FontZoom::instance(), &FontZoom::changed, this, [this](FontZoom::Target target, qreal pointSize) {
        if (!isActiveWindow())
            return;
        statusBar()->showMessage(target == FontZoom::Target::Editor ? tr("SQL editor font: %1 pt").arg(pointSize)
                                                                    : tr("CLI font: %1 pt").arg(pointSize),
                                 3000);
    });

    m_scriptFoldersAction = new QAction(tr("Script Folders..."), this);
    m_scriptFoldersAction->setToolTip(tr("Folders the CLI's @name looks in, one per DBMS"));
    connect(m_scriptFoldersAction, &QAction::triggered, this, [this] {
        ScriptFoldersDialog dialog(this, profile() ? profile()->type : DbType::Oracle);
        dialog.exec();
    });

    m_closeTabAction = new QAction(tr("Close Tab"), this);
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this, [this] { closeQueryTab(m_tabs->currentIndex()); });

    m_openFileAction = new QAction(tr("Open File..."), this);
    m_openFileAction->setShortcut(QKeySequence::Open);
    connect(m_openFileAction, &QAction::triggered, this, &MainWindow::openFileDialog);

    m_saveAction = new QAction(tr("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, [this] { saveTab(currentQueryTab()); });

    // 찾기/바꾸기 (지금 탭의 에디터)
    m_findAction = new QAction(tr("Find..."), this);
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->findBar()->openFind();
    });
    m_replaceAction = new QAction(tr("Replace..."), this);
    m_replaceAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::CTRL | Qt::Key_H)});
    connect(m_replaceAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->findBar()->openReplace();
    });
    m_findNextAction = new QAction(tr("Find Next"), this);
    m_findNextAction->setShortcut(QKeySequence(Qt::Key_F3));
    connect(m_findNextAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->findBar()->findNext();
    });
    m_findPreviousAction = new QAction(tr("Find Previous"), this);
    m_findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    connect(m_findPreviousAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->findBar()->findPrevious();
    });

    m_formatAction = new QAction(tr("Format SQL"), this);
    m_formatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_formatAction->setToolTip(withShortcut(tr("Line up the selected SQL (or the statement at the cursor)"),
                                            m_formatAction->shortcut()));
    connect(m_formatAction, &QAction::triggered, this, &MainWindow::formatCurrent);

    m_saveAsAction = new QAction(tr("Save As..."), this);
    m_saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(m_saveAsAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            saveTabAs(tab);
    });

    m_quitAction = new QAction(tr("Exit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setToolTip(tr("Close every DBaro window"));
    connect(m_quitAction, &QAction::triggered, m_app, &AppController::quitAll);

    m_closeWindowAction = new QAction(tr("Close Window"), this);
    m_closeWindowAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(m_closeWindowAction, &QAction::triggered, this, &QWidget::close);

    // 다른 연결은 새 창으로 연다.
    m_newConnectionAction = new QAction(Icons::get(QStringLiteral("connection-add")), tr("New Connection..."), this);
    m_newConnectionAction->setToolTip(tr("Add a connection and open it in a new window"));
    connect(m_newConnectionAction, &QAction::triggered, this, [this] { m_app->newConnection(this); });

    m_editConnectionAction = new QAction(tr("Edit Connection..."), this);
    connect(m_editConnectionAction, &QAction::triggered, this,
            [this] { m_app->editConnections(this, false, m_connectionId); });

    // 아이콘: 플러그 (Database Explorer의 원통과 헷갈리지 않게). New Connection은 플러그 + 초록 +
    m_manageConnectionsAction = new QAction(Icons::get(QStringLiteral("connections")), tr("Connections..."), this);
    m_manageConnectionsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_O));
    m_manageConnectionsAction->setToolTip(
        withShortcut(tr("Choose another connection (opens in its own window)"), m_manageConnectionsAction->shortcut()));
    connect(m_manageConnectionsAction, &QAction::triggered, m_app, &AppController::showLauncher);

    m_executeAction = new QAction(Icons::get(QStringLiteral("run")), tr("Execute"), this);
    m_executeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    m_executeAction->setToolTip(withShortcut(tr("Execute the statement at the cursor"), m_executeAction->shortcut()));
    connect(m_executeAction, &QAction::triggered, this, [this] { executeCurrent(false); });

    m_executeScriptAction = new QAction(Icons::get(QStringLiteral("run-script")), tr("Execute Script"), this);
    m_executeScriptAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_executeScriptAction->setToolTip(withShortcut(tr("Execute the whole script"), m_executeScriptAction->shortcut()));
    connect(m_executeScriptAction, &QAction::triggered, this, [this] { executeCurrent(true); });

    m_explainAction = new QAction(Icons::get(QStringLiteral("explain")), tr("Explain Plan"), this);
    m_explainAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_explainAction->setToolTip(
        withShortcut(tr("Estimated execution plan of the statement at the cursor (not executed)"), m_explainAction->shortcut()));
    connect(m_explainAction, &QAction::triggered, this, [this] { explainCurrent(false); });

    m_explainActualAction = new QAction(Icons::get(QStringLiteral("explain-actual")), tr("Actual Plan"), this);
    m_explainActualAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_X));
    m_explainActualAction->setToolTip(withShortcut(
        tr("Run the statement at the cursor and show its actual execution plan with real row counts and times.\n"
           "Changes made by INSERT, UPDATE, DELETE or MERGE are rolled back."),
        m_explainActualAction->shortcut()));
    connect(m_explainActualAction, &QAction::triggered, this, [this] { explainCurrent(true); });

    m_historyAction = new QAction(tr("Execution History..."), this);
    m_historyAction->setShortcuts({QKeySequence(Qt::Key_F8), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E)});
    m_historyAction->setToolTip(withShortcut(tr("Search the statements you ran and put one back in the editor"),
                                             m_historyAction->shortcut()));
    connect(m_historyAction, &QAction::triggered, this, &MainWindow::showHistory);

    m_exportAction = new QAction(tr("Export Results..."), this);
    m_exportAction->setToolTip(tr("Save the result as Excel, CSV, SQL INSERT, JSON or Markdown, or copy it"));
    connect(m_exportAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->resultPanel()->showExportDialog();
    });

    m_ddlAction = new QAction(tr("Show DDL"), this);
    m_ddlAction->setShortcut(QKeySequence(Qt::Key_F4));
    m_ddlAction->setToolTip(withShortcut(
        tr("DDL of the selected table or object, with comments, indexes, synonyms and grants"), m_ddlAction->shortcut()));
    connect(m_ddlAction, &QAction::triggered, this, &MainWindow::showDdlAtCursor);

    m_tableDetailsAction = new QAction(tr("Table Details"), this);
    m_tableDetailsAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F4));
    m_tableDetailsAction->setToolTip(withShortcut(
        tr("Columns, indexes, foreign keys and DDL of the selected table"), m_tableDetailsAction->shortcut()));
    connect(m_tableDetailsAction, &QAction::triggered, this, &MainWindow::showTableDetailsAtCursor);

    m_importAction = new QAction(tr("Import Data..."), this);
    m_importAction->setToolTip(tr("Load an Excel (.xlsx) or CSV file into a table"));
    connect(m_importAction, &QAction::triggered, this, [this] { importData(activeMetadataId(), QString(), QString()); });

    m_cancelAction = new QAction(Icons::get(QStringLiteral("stop")), tr("Cancel"), this);
    m_cancelAction->setToolTip(tr("Cancel the running statement"));
    connect(m_cancelAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->session()->cancel();
    });

    m_commitAction = new QAction(Icons::get(QStringLiteral("commit")), tr("Commit"), this);
    connect(m_commitAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->session()->commit();
    });
    m_rollbackAction = new QAction(Icons::get(QStringLiteral("rollback")), tr("Rollback"), this);
    connect(m_rollbackAction, &QAction::triggered, this, [this] {
        if (QueryTab *tab = currentQueryTab())
            tab->session()->rollback();
    });

    m_autoCommitAction = new QAction(tr("Auto-commit"), this);
    m_autoCommitAction->setCheckable(true);
    m_autoCommitAction->setChecked(false);
    connect(m_autoCommitAction, &QAction::toggled, this, [this](bool autoCommit) {
        for (QueryTab *tab : queryTabs())
            tab->session()->setAutoCommit(autoCommit);
        updateTransactionLabel();
        updateActionStates();
    });

    // 테마
    auto *themeGroup = new QActionGroup(this);
    m_darkThemeAction = new QAction(tr("Dark"), themeGroup);
    m_lightThemeAction = new QAction(tr("Light"), themeGroup);
    for (QAction *action : {m_darkThemeAction, m_lightThemeAction})
        action->setCheckable(true);
    (Theme::current() == Theme::Kind::Light ? m_lightThemeAction : m_darkThemeAction)->setChecked(true);
    connect(themeGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        const Theme::Kind kind = action == m_lightThemeAction ? Theme::Kind::Light : Theme::Kind::Dark;
        Theme::apply(kind);
        Theme::save(kind);
    });

    m_aboutAction = new QAction(tr("About DBaro"), this);
    connect(m_aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About DBaro"),
                           tr("DBaro %1\nCross-platform database tool.").arg(QApplication::applicationVersion()));
    });
}

void MainWindow::createDocks()
{
    // 탐색기에는 이 창의 연결만 보인다.
    m_explorer = new ObjectExplorer(m_app->metadata(), this);
    connect(m_explorer, &ObjectExplorer::newQueryRequested, this, [this] { addQueryTab(); });
    connect(m_explorer, &ObjectExplorer::consoleRequested, this, [this] { openConsole(); });
    connect(m_explorer, &ObjectExplorer::openObjectRequested, this, &MainWindow::openObject);
    connect(m_explorer, &ObjectExplorer::useSchemaRequested, this, &MainWindow::useSchema);
    connect(m_explorer, &ObjectExplorer::ddlRequested, this,
            [this](const QString &connectionId, const QString &schema, const QString &name) {
                // 탐색기의 이름은 DB에 저장된 그대로이므로 따옴표로 감싼 것처럼 대소문자를 그대로 쓴다.
                requestDdl(connectionId, schema, name, true, true, currentQueryTab());
            });
    connect(m_explorer, &ObjectExplorer::tableDetailsRequested, this,
            [this](const QString &id, const QString &schema, const QString &name) {
                openTableDetails(id, schema, name, true, true);
            });
    connect(m_explorer, &ObjectExplorer::importRequested, this,
            [this](const QString &id, const QString &schema, const QString &name) { importData(id, schema, name); });
    connect(m_explorer, &ObjectExplorer::insertTextRequested, this, [this](const QString &text) {
        if (QueryTab *tab = currentQueryTab()) {
            tab->editor()->insertPlainText(text);
            tab->editor()->setFocus();
        }
    });
    connect(m_explorer, &ObjectExplorer::editConnectionRequested, this,
            [this](const QString &id) { m_app->editConnections(this, false, Db::baseProfileId(id)); });

    m_explorerDock = new QDockWidget(tr("Database Explorer"), this);
    m_explorerDock->setObjectName(QStringLiteral("explorerDock"));
    m_explorerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_explorerDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_explorerDock->setWidget(m_explorer);
    m_explorerDock->setTitleBarWidget(createDockTitleBar(m_explorerDock));
    addDockWidget(Qt::LeftDockWidgetArea, m_explorerDock);
    resizeDocks({m_explorerDock}, {280}, Qt::Horizontal);

    QAction *toggle = m_explorerDock->toggleViewAction();
    toggle->setIcon(Icons::get(QStringLiteral("database")));
    toggle->setShortcut(QKeySequence(Qt::ALT | Qt::Key_1));
    toggle->setToolTip(withShortcut(tr("Database Explorer"), toggle->shortcut()));
}

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_newTabAction);
    fileMenu->addAction(m_consoleAction);
    fileMenu->addAction(m_openFileAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addAction(m_closeWindowAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_findAction);
    editMenu->addAction(m_replaceAction);
    editMenu->addAction(m_findNextAction);
    editMenu->addAction(m_findPreviousAction);
    editMenu->addSeparator();
    editMenu->addAction(m_formatAction);

    QMenu *databaseMenu = menuBar()->addMenu(tr("&Database"));
    databaseMenu->addAction(m_manageConnectionsAction);
    databaseMenu->addAction(m_newConnectionAction);
    databaseMenu->addAction(m_editConnectionAction);
    databaseMenu->addSeparator();
    databaseMenu->addAction(m_scriptFoldersAction);

    QMenu *queryMenu = menuBar()->addMenu(tr("&Query"));
    queryMenu->addAction(m_executeAction);
    queryMenu->addAction(m_executeScriptAction);
    queryMenu->addAction(m_explainAction);
    queryMenu->addAction(m_explainActualAction);
    queryMenu->addAction(m_ddlAction);
    queryMenu->addAction(m_historyAction);
    queryMenu->addAction(m_exportAction);
    queryMenu->addAction(m_importAction);
    queryMenu->addAction(m_tableDetailsAction);
    queryMenu->addAction(m_cancelAction);
    queryMenu->addSeparator();
    queryMenu->addAction(m_commitAction);
    queryMenu->addAction(m_rollbackAction);
    queryMenu->addAction(m_autoCommitAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_explorerDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(m_nextTabAction);
    viewMenu->addAction(m_previousTabAction);
    viewMenu->addSeparator();
    QMenu *themeMenu = viewMenu->addMenu(tr("Theme"));
    themeMenu->addAction(m_darkThemeAction);
    themeMenu->addAction(m_lightThemeAction);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createToolBar()
{
    QToolBar *toolBar = addToolBar(tr("Main"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setMovable(false);
    toolBar->setFloatable(false);
    toolBar->setIconSize(QSize(18, 18));
    toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolBar->toggleViewAction()->setVisible(false);

    // 왼쪽: 다른 연결(새 창)과 새 탭
    toolBar->addAction(m_manageConnectionsAction);
    toolBar->addAction(m_newTabAction);
    toolBar->addAction(m_consoleAction);

    // 가운데 여백: 실행 관련 버튼을 오른쪽 끝으로 민다
    auto *spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(spacer);

    // 오른쪽: 실행과 트랜잭션
    toolBar->addAction(m_executeAction);
    toolBar->addAction(m_executeScriptAction);
    toolBar->addAction(m_explainAction);
    toolBar->addAction(m_explainActualAction);
    toolBar->addAction(m_cancelAction);
    toolBar->addSeparator();

    m_transactionButton = new QToolButton(toolBar);
    m_transactionButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_transactionButton->setPopupMode(QToolButton::InstantPopup);
    m_transactionButton->setToolTip(tr("Transaction mode"));
    auto *transactionMenu = new QMenu(m_transactionButton);
    transactionMenu->addAction(m_autoCommitAction);
    m_transactionButton->setMenu(transactionMenu);
    toolBar->addWidget(m_transactionButton);

    toolBar->addAction(m_commitAction);
    toolBar->addAction(m_rollbackAction);
}

void MainWindow::createToolWindowStripe()
{
    // 창 왼쪽 끝의 세로 아이콘 막대. 누르면 해당 도구 창이 열리고 닫힌다.
    auto *stripe = new QToolBar(tr("Tool Windows"), this);
    stripe->setObjectName(QStringLiteral("toolWindowStripe"));
    stripe->setMovable(false);
    stripe->setFloatable(false);
    stripe->setIconSize(QSize(20, 20));
    stripe->setToolButtonStyle(Qt::ToolButtonIconOnly);
    stripe->toggleViewAction()->setVisible(false);
    stripe->addAction(m_explorerDock->toggleViewAction());
    addToolBar(Qt::LeftToolBarArea, stripe);
}

void MainWindow::createStatusBar()
{
    m_connectionLabel = new QLabel(this);
    m_transactionLabel = new QLabel(this);
    m_cursorLabel = new QLabel(this);
    m_encodingLabel = new QLabel(QStringLiteral("UTF-8"), this);

    for (QLabel *label : {m_connectionLabel, m_transactionLabel, m_cursorLabel, m_encodingLabel}) {
        label->setContentsMargins(8, 0, 8, 0);
        statusBar()->addPermanentWidget(label);
    }
    statusBar()->setSizeGripEnabled(false);

    updateTransactionLabel();
    updateCursorPosition(1, 1);
    statusBar()->showMessage(tr("Ready"), 3000);
}

void MainWindow::restoreLayout()
{
    // 연결마다 창 크기·위치를 기억한다. 처음 여는 연결은 마지막에 닫은 창의 배치를 따른다.
    QSettings settings;
    const QString key = QStringLiteral("windows/%1/").arg(m_connectionId);
    m_hasSavedGeometry = settings.contains(key + QStringLiteral("geometry"));
    restoreGeometry(settings.value(m_hasSavedGeometry ? key + QStringLiteral("geometry") : kGeometryKey).toByteArray());
    restoreState(settings.value(settings.contains(key + QStringLiteral("state")) ? key + QStringLiteral("state") : kStateKey)
                     .toByteArray());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    int pending = 0;
    for (QueryTab *tab : queryTabs())
        pending += tab->session()->hasPendingTransaction() ? 1 : 0;
    for (ConsoleTab *console : findChildren<ConsoleTab *>())
        pending += console->hasPendingTransaction() ? 1 : 0;
    if (pending > 0) {
        const auto answer = QMessageBox::warning(
            this, tr("Uncommitted Changes"),
            tr("%n tab(s) have uncommitted changes. They will be rolled back if you close this window.", nullptr, pending),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            event->ignore();
            return;
        }
    }

    // 저장하지 않은 파일도 묻지 않고 작업 공간에 남긴다 (다음에 열면 고친 내용 그대로 돌아온다).
    if (profile())
        saveWorkspace();
    QSettings settings;
    const QString key = QStringLiteral("windows/%1/").arg(m_connectionId);
    settings.setValue(key + QStringLiteral("geometry"), saveGeometry());
    settings.setValue(key + QStringLiteral("state"), saveState());
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kStateKey, saveState());
    QMainWindow::closeEvent(event);
    emit windowClosed();
}

void MainWindow::scheduleWorkspaceSave()
{
    if (!m_restoringWorkspace && m_workspaceTimer)
        m_workspaceTimer->start();
}

void MainWindow::saveWorkspace()
{
    m_workspaceTimer->stop();
    Workspace workspace;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<QueryTab *>(m_tabs->widget(i));
        if (!tab)
            continue;
        WorkspaceTab saved;
        saved.fixedTitle = tab->property(kTitleSource).toString() == QLatin1String(kFixedTitle);
        saved.title = m_tabs->tabText(i);
        saved.filePath = tab->filePath();
        saved.format = tab->fileFormat();
        saved.modified = tab->hasUnsavedFile();
        // 저장한 그대로인 파일은 다음에 파일에서 다시 읽는다.
        if (saved.filePath.isEmpty() || saved.modified)
            saved.sql = tab->editor()->toPlainText();
        saved.cursor = tab->editor()->textCursor().position();
        saved.ddlKey = tab->property("dbaroDdlKey").toString();
        saved.schema = tab->preferredSchema();
        saved.database = tab->database();
        workspace.tabs.append(saved);
    }
    workspace.current = m_tabs->currentIndex();
    QString error;
    if (!m_app->workspaceStore().save(m_connectionId, workspace, &error))
        statusBar()->showMessage(tr("Could not save the open tabs: %1").arg(error), 8000);
}

bool MainWindow::restoreWorkspace()
{
    Workspace workspace;
    QString error;
    if (!m_app->workspaceStore().load(m_connectionId, &workspace, &error)) {
        statusBar()->showMessage(tr("Could not restore the open tabs: %1").arg(error), 8000);
        return false;
    }

    m_restoringWorkspace = true;
    QStringList missing;
    for (const WorkspaceTab &saved : std::as_const(workspace.tabs)) {
        QString text = saved.sql;
        TextFileFormat format = saved.format;
        if (!saved.filePath.isEmpty() && !saved.modified) {
            QString readError;
            if (!TextFile::read(saved.filePath, &text, &format, &readError)) {
                missing << QDir::toNativeSeparators(saved.filePath);
                continue;
            }
        }
        QueryTab *tab = addQueryTab();
        const int index = m_tabs->indexOf(tab);
        if (!saved.filePath.isEmpty()) {
            tab->setFile(saved.filePath, format);
            tab->setProperty(kTitleSource, kFixedTitle);
        } else if (saved.fixedTitle) {
            tab->setProperty(kTitleSource, kFixedTitle);
            m_tabs->setTabText(index, saved.title);
        }
        if (!saved.ddlKey.isEmpty())
            tab->setProperty("dbaroDdlKey", saved.ddlKey);
        {
            // 아직 접속 전이다 (창이 보이면 이 DB로 접속한다). 접속 설정의 DB면 기본으로
            const ConnectionProfile *p = profile();
            const bool keep = p && Db::canSwitchDatabase(p->type) && saved.database != p->database;
            tab->setDatabase(keep ? saved.database : QString());
        }
        tab->setPreferredSchema(saved.schema);
        tab->editor()->setPlainText(text);
        tab->editor()->document()->setModified(saved.modified);
        QTextCursor cursor = tab->editor()->textCursor();
        cursor.setPosition(qBound(0, saved.cursor, int(text.size())));
        tab->editor()->setTextCursor(cursor);
        updateFileTabTitle(tab);
    }
    m_restoringWorkspace = false;

    if (!missing.isEmpty())
        statusBar()->showMessage(tr("Could not reopen: %1").arg(missing.join(QStringLiteral(", "))), 10000);
    if (m_tabs->count() == 0)
        return false;
    m_tabs->setCurrentIndex(qBound(0, workspace.current, m_tabs->count() - 1));
    if (QueryTab *tab = currentQueryTab()) {
        tab->editor()->setFocus();
        m_activeDatabase = tab->database(); // 같은 탭이면 currentChanged가 오지 않는다
    }
    syncExplorerDatabase();
    return true;
}

void MainWindow::openFileDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open SQL File"), lastFileDir(),
                                                            tr("SQL files (*.sql *.txt);;All files (*)"));
    if (paths.isEmpty())
        return;
    QSettings().setValue(kLastDirKey, QFileInfo(paths.first()).absolutePath());
    openFiles(paths);
}

int MainWindow::openFiles(const QStringList &paths)
{
    int opened = 0;
    QueryTab *last = nullptr;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        const QString absolute = info.absoluteFilePath();

        QueryTab *existing = nullptr;
        for (QueryTab *tab : queryTabs()) {
            if (!tab->filePath().isEmpty() && QFileInfo(tab->filePath()) == info) {
                existing = tab;
                break;
            }
        }
        if (existing) {
            last = existing;
            ++opened;
            continue;
        }

        QString text, error;
        TextFileFormat format;
        if (!TextFile::read(absolute, &text, &format, &error)) {
            QMessageBox::warning(this, tr("Open File"),
                                 tr("Could not open %1.\n%2").arg(QDir::toNativeSeparators(absolute), error));
            continue;
        }

        // 지금 탭이 비어 있으면 그 탭에 연다 (연결은 그대로). 아니면 같은 연결로 새 탭.
        QueryTab *tab = currentQueryTab();
        if (!tab || !tab->filePath().isEmpty() || !tab->editor()->toPlainText().trimmed().isEmpty())
            tab = addQueryTab();
        tab->setProperty("dbaroDdlKey", QVariant());
        tab->setProperty(kTitleSource, kFixedTitle);
        tab->setFile(absolute, format);
        tab->editor()->setPlainText(text);
        tab->editor()->document()->setModified(false);
        tab->editor()->moveCursor(QTextCursor::Start);
        updateFileTabTitle(tab);
        last = tab;
        ++opened;
    }
    if (last) {
        m_tabs->setCurrentWidget(last);
        last->editor()->setFocus();
        scheduleWorkspaceSave();
    }
    return opened;
}

bool MainWindow::saveTab(QueryTab *tab, const QString &path)
{
    if (!tab)
        return false;
    const QString target = path.isEmpty() ? tab->filePath() : QFileInfo(path).absoluteFilePath();
    if (target.isEmpty())
        return saveTabAs(tab);

    TextFileFormat format = tab->fileFormat();
    if (tab->filePath().isEmpty()) {
        format = {}; // 새 파일: UTF-8, BOM 없음, 줄 끝은 OS 방식
#ifdef Q_OS_WIN
        format.crlf = true;
#endif
    }
    QString error;
    bool unencodable = false;
    if (!TextFile::write(target, tab->editor()->toPlainText(), format, &error, &unencodable)) {
        if (!unencodable) {
            QMessageBox::warning(this, tr("Save"), tr("Could not save %1.\n%2").arg(QDir::toNativeSeparators(target), error));
            return false;
        }
        const auto answer = QMessageBox::question(
            this, tr("Save"),
            tr("Some characters in this tab cannot be saved in %1.\nSave the file as UTF-8 instead?")
                .arg(TextFile::encodingName(format)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return false;
        format.encoding = TextFileFormat::Encoding::Utf8;
        format.bom = false;
        if (!TextFile::write(target, tab->editor()->toPlainText(), format, &error)) {
            QMessageBox::warning(this, tr("Save"), tr("Could not save %1.\n%2").arg(QDir::toNativeSeparators(target), error));
            return false;
        }
    }

    tab->setFile(target, format);
    tab->setProperty(kTitleSource, kFixedTitle);
    tab->setProperty("dbaroDdlKey", QVariant()); // 파일이 된 DDL 탭은 F4가 내용을 바꾸지 않게 한다
    tab->editor()->document()->setModified(false);
    updateFileTabTitle(tab);
    statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(target)), 5000);
    scheduleWorkspaceSave();
    return true;
}

bool MainWindow::saveTabAs(QueryTab *tab)
{
    const int index = m_tabs->indexOf(tab);
    const QString suggested = !tab->filePath().isEmpty()
                                  ? tab->filePath()
                                  : QDir(lastFileDir()).filePath(fileNameFromTitle(m_tabs->tabText(index)) + QStringLiteral(".sql"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save SQL File"), suggested,
                                                      tr("SQL files (*.sql);;All files (*)"));
    if (path.isEmpty())
        return false;
    QSettings().setValue(kLastDirKey, QFileInfo(path).absolutePath());
    return saveTab(tab, path);
}

void MainWindow::updateFileTabTitle(QueryTab *tab)
{
    const int index = m_tabs->indexOf(tab);
    if (index < 0 || tab->filePath().isEmpty())
        return;
    const QString name = QFileInfo(tab->filePath()).fileName();
    m_tabs->setTabText(index, tab->hasUnsavedFile() ? name + QStringLiteral(" *") : name);
    m_tabs->setTabToolTip(index, QDir::toNativeSeparators(tab->filePath()));
    if (tab == currentQueryTab())
        m_encodingLabel->setText(TextFile::encodingName(tab->fileFormat()));
}

bool MainWindow::confirmCloseUnsaved(QueryTab *tab)
{
    if (!tab->hasUnsavedFile())
        return true;
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("Save the changes to %1?").arg(QFileInfo(tab->filePath()).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveTab(tab);
    return answer == QMessageBox::Discard;
}

QueryTab *MainWindow::currentQueryTab() const
{
    return qobject_cast<QueryTab *>(m_tabs->currentWidget());
}

QList<QueryTab *> MainWindow::queryTabs() const
{
    QList<QueryTab *> tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *tab = qobject_cast<QueryTab *>(m_tabs->widget(i)))
            tabs.append(tab);
    }
    return tabs;
}

ConsoleTab *MainWindow::openConsole()
{
    const ConnectionProfile *profile = this->profile();
    if (!profile)
        return nullptr;
    QString password;
    if (!m_app->passwordFor(*profile, &password, true, this))
        return nullptr;
    // 보고 있던 탭의 DB·스키마에서 이어서
    const ConnectionProfile target = profileOn(m_activeDatabase);
    ConnectSpec spec = m_app->connectSpec(target, password, m_autoCommitAction->isChecked());
    if (QueryTab *current = currentQueryTab())
        spec.schema = current->currentSchema();

    auto *console = new ConsoleTab(m_app->engine(), target, m_tabs);
    // 실행 기록은 F8 목록에 바로 보이게 앱의 기록에 넣고, 위험한 문장은 쿼리 탭처럼 묻는다
    console->cli()->setHistorySink([this](const HistoryEntry &entry) { m_app->history().add(entry); });
    QPointer<ConsoleTab> guard(console);
    console->cli()->setConfirm([this](const QString &question) {
        return QMessageBox::warning(this, tr("CLI"), question, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
               == QMessageBox::Yes;
    });
    connect(console, &ConsoleTab::closeRequested, this, [this, guard] {
        if (guard)
            closeQueryTab(m_tabs->indexOf(guard));
    });
    connect(console, &ConsoleTab::stateChanged, this, [this, guard] {
        if (guard && m_tabs->currentWidget() == guard)
            updateConnectionStatus();
    });

    int number = 1;
    for (ConsoleTab *other : findChildren<ConsoleTab *>())
        number += other != console ? 1 : 0;
    const QString title = number == 1 ? tr("CLI") : tr("CLI %1").arg(number);
    const int index = m_tabs->addTab(console, Icons::get(QStringLiteral("console")), title);
    m_tabs->setTabToolTip(index, tr("CLI on %1 (same commands as dbaro-cli, \\help)").arg(profile->name));
    m_tabs->setCurrentIndex(index);
    console->start(spec);
    console->focusInput();
    return console;
}

void MainWindow::updateBusyTab(QueryTab *tab)
{
    const bool busy = tab->session()->isBusy();
    if (busy && !m_busyTabs.contains(tab)) {
        QElapsedTimer clock;
        clock.start();
        m_busyTabs.insert(tab, clock);
        if (!m_busyTabTimer) {
            m_busyTabTimer = new QTimer(this);
            m_busyTabTimer->setInterval(80);
            connect(m_busyTabTimer, &QTimer::timeout, this, &MainWindow::animateBusyTabs);
        }
        m_busyTabTimer->start();
    } else if (!busy && m_busyTabs.remove(tab)) {
        const int index = m_tabs->indexOf(tab);
        if (index >= 0)
            m_tabs->setTabIcon(index, QIcon());
    }
}

void MainWindow::animateBusyTabs()
{
    if (m_busyTabs.isEmpty()) {
        m_busyTabTimer->stop();
        return;
    }
    m_busyTabAngle = (m_busyTabAngle + 30) % 360;
    const int size = m_tabs->tabBar()->iconSize().height() > 0 ? m_tabs->tabBar()->iconSize().height() : 16;
    const QIcon icon(BusySpinner::frame(size, m_busyTabAngle, palette().color(QPalette::Link), devicePixelRatioF()));
    for (auto it = m_busyTabs.cbegin(); it != m_busyTabs.cend(); ++it) {
        if (it.value().elapsed() < 400)
            continue; // 금방 끝나는 실행은 깜빡이지 않게
        const int index = m_tabs->indexOf(it.key());
        if (index >= 0)
            m_tabs->setTabIcon(index, icon);
    }
}

QueryTab *MainWindow::addQueryTab()
{
    return addQueryTabOn(m_activeDatabase);
}

QueryTab *MainWindow::addQueryTabOn(const QString &database)
{
    // 탭은 모두 이 창의 연결을 쓴다 (세션은 탭마다 따로 연다). PostgreSQL은 탭마다 DB를 고를 수 있다.
    auto *tab = new QueryTab(m_app->engine(), m_tabs);
    tab->setMetadataService(m_app->metadata());
    tab->setDatabase(database); // 탭을 고르면 바로 접속하므로 그 전에 정한다
    tab->session()->setAutoCommit(m_autoCommitAction->isChecked());
    connect(tab, &QueryTab::cursorPositionChanged, this, [this, tab](int line, int column) {
        if (tab == currentQueryTab())
            updateCursorPosition(line, column);
    });
    connect(tab->session(), &QuerySession::stateChanged, this, [this, tab] {
        updateBusyTab(tab);
        if (tab == currentQueryTab()) {
            updateConnectionStatus();
            updateActionStates();
        }
    });
    connect(tab, &QObject::destroyed, this, [this, tab] { m_busyTabs.remove(tab); });
    connect(tab->editor(), &QPlainTextEdit::textChanged, this, &MainWindow::scheduleWorkspaceSave);
    connect(tab->editor(), &SqlEditor::wordMarksChanged, this, [this](const QString &word, int count) {
        if (word.isEmpty())
            statusBar()->clearMessage();
        else
            statusBar()->showMessage(count == 1 ? tr("%1: 1 occurrence").arg(word)
                                                : tr("%1: %2 occurrences").arg(word).arg(count));
    });
    connect(tab, &QueryTab::schemaSwitchRequested, this, [this, tab](const QString &schema) { switchSchema(tab, schema); });
    connect(tab, &QueryTab::databaseSwitchRequested, this,
            [this, tab](const QString &database) { switchDatabase(tab, database); });
    connect(tab->resultPanel(), &ResultPanel::planObjectRequested, this,
            [this, tab](const QCborMap &object) { showPlanObject(tab, object); });
    connect(tab->resultPanel(), &ResultPanel::exportFinished, this,
            [this](const QString &message) { statusBar()->showMessage(message, 10000); });
    connect(tab->session(), &QuerySession::statementFinished, this,
            [this, tab](const SqlStatement &statement, bool ok, qint64 elapsedMs, qint64 rows, const QString &error) {
                HistoryEntry entry;
                entry.connectionId = tab->connectionId();
                if (const ConnectionProfile *p = profile())
                    entry.connectionName = p->name;
                entry.sql = statement.originalText();
                entry.ok = ok;
                entry.elapsedMs = elapsedMs;
                entry.rows = rows;
                entry.error = error;
                m_app->history().add(entry);
            });
    connect(tab->editor()->document(), &QTextDocument::modificationChanged, this, [this, tab] { updateFileTabTitle(tab); });

    const int index = m_tabs->addTab(tab, tr("Query %1").arg(++m_tabCounter));
    applyConnection(tab);
    m_tabs->setCurrentIndex(index);
    tab->editor()->setFocus();
    return tab;
}

void MainWindow::applyConnection(QueryTab *tab)
{
    const ConnectionProfile *p = profile();
    // DB를 바꿀 수 없게 됐거나(DB 종류를 바꿈) 고른 DB가 접속 설정의 DB가 되면 기본 DB로
    if (p && (!Db::canSwitchDatabase(p->type) || tab->database() == p->database))
        tab->setDatabase({});
    tab->setConnectionId(p ? m_connectionId : QString());
    tab->setConnectionProfile(p);
    tab->setDialect(p ? sqlDialectFor(p->type) : SqlDialect::Generic);
    updateFileTabTitle(tab);
    scheduleWorkspaceSave();
}

ConnectionProfile MainWindow::profileOn(const QString &database) const
{
    ConnectionProfile target;
    if (const ConnectionProfile *p = profile())
        target = *p;
    if (!database.isEmpty() && Db::canSwitchDatabase(target.type))
        target.database = database;
    return target;
}

QString MainWindow::activeMetadataId() const
{
    return Db::metadataId(m_connectionId, m_activeDatabase);
}

void MainWindow::syncExplorerDatabase(bool force)
{
    const ConnectionProfile *p = profile();
    if (!p)
        return;
    const QString id = activeMetadataId();
    if (id == m_explorerId && !force)
        return;
    // 다른 DB로 옮길 때 펼쳐 있었으면 그 DB도 펼친다 (같은 비밀번호로 접속한다)
    const bool reopen = !force && !m_explorerId.isEmpty() && m_explorer->isConnectionExpanded(m_explorerId);
    ConnectionProfile shown = profileOn(m_activeDatabase);
    shown.id = id;
    m_explorerId = id;
    m_explorer->setConnections({shown});
    if (reopen)
        m_explorer->expandConnection(id);
}

QueryTab *MainWindow::queryTabFor(const QString &metadataId)
{
    QueryTab *tab = currentQueryTab();
    if (tab && tab->metadataId() == metadataId)
        return tab;
    return addQueryTabOn(Db::databaseOf(metadataId));
}

void MainWindow::openObject(const QString &metadataId, const QString &schema, const QString &name)
{
    const ConnectionProfile *profile = this->profile();
    if (!profile || Db::baseProfileId(metadataId) != m_connectionId)
        return;
    // 테이블·뷰를 두 번 클릭하면 새 탭에서 바로 조회한다 (결과는 페이지 단위로 가져온다).
    QueryTab *tab = addQueryTabOn(Db::databaseOf(metadataId));
    tab->setProperty(kTitleSource, kFixedTitle); // 연결 이름 대신 객체 이름을 제목으로 둔다
    m_tabs->setTabText(m_tabs->indexOf(tab), name);
    tab->editor()->setPlainText(
        QStringLiteral("SELECT *\n  FROM %1;").arg(SqlIdentifiers::qualified(schema, name, sqlDialectFor(profile->type))));
    executeCurrent(false);
}

void MainWindow::selectAdjacentTab(int step)
{
    const int count = m_tabs->count();
    if (count < 2)
        return;
    m_tabs->setCurrentIndex((m_tabs->currentIndex() + step + count) % count);
    // 옮겨 간 탭에서 바로 입력할 수 있게
    QWidget *widget = m_tabs->currentWidget();
    if (auto *tab = qobject_cast<QueryTab *>(widget))
        tab->editor()->setFocus(Qt::TabFocusReason);
    else if (auto *console = qobject_cast<ConsoleTab *>(widget))
        console->focusInput();
    else if (widget)
        widget->setFocus(Qt::TabFocusReason);
}

void MainWindow::closeQueryTab(int index)
{
    QWidget *widget = m_tabs->widget(index);
    auto *tab = qobject_cast<QueryTab *>(widget);
    if (!tab) {
        if (auto *console = qobject_cast<ConsoleTab *>(widget); console && console->hasPendingTransaction()) {
            const auto answer = QMessageBox::warning(
                this, tr("Uncommitted Changes"),
                tr("This CLI tab has uncommitted changes. They will be rolled back if you close this tab."),
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Discard)
                return;
        }
        if (widget) { // 테이블 상세 탭, CLI 탭 같은 쿼리 탭이 아닌 탭
            m_tabs->removeTab(index);
            widget->deleteLater();
        }
        return;
    }
    if (!confirmCloseUnsaved(tab) || !confirmDiscardTransaction(tab, tr("close this tab")))
        return;
    if (tab->resultPanel()->hasPendingEdits()
        && QMessageBox::question(this, tr("Close Tab"), tr("The result has changes that are not applied. Discard them?"),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
               != QMessageBox::Discard)
        return;
    m_tabs->removeTab(index);
    tab->deleteLater(); // 세션은 탭과 함께 연결을 끊는다 (커밋 안 된 변경은 롤백)
    scheduleWorkspaceSave();
}

void MainWindow::executeCurrent(bool wholeScript)
{
    QueryTab *tab = currentQueryTab();
    if (!tab)
        return;
    QuerySession *session = tab->session();
    ResultPanel *results = tab->resultPanel();
    if (session->isBusy())
        return;

    QList<SqlStatement> statements = tab->editor()->statementsToExecute(wholeScript);
    if (statements.isEmpty()) {
        results->appendLog(tr("Nothing to execute."));
        results->showLog();
        return;
    }

    if (!requireConnection(tab) || !applyBindVariables(tab, &statements) || !confirmSafeToRun(tab, statements))
        return;
    // 결과를 고치던 중이면 새 결과가 덮기 전에 묻는다
    if (results->hasPendingEdits()) {
        const auto answer = QMessageBox::question(
            this, tr("Edit Result"), tr("The result has changes that are not applied. Discard them and run?"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard)
            return;
        results->discardEdits();
    }

    if (statements.size() > 1)
        results->appendLog(tr("Running %n statement(s) ...", nullptr, int(statements.size())));

    QPointer<QuerySession> guard(session);
    withConnectedSession(tab, [guard, statements] {
        if (guard)
            guard->run(statements);
    });
}

void MainWindow::explainCurrent(bool actual)
{
    QueryTab *tab = currentQueryTab();
    if (!tab || tab->session()->isBusy())
        return;
    ResultPanel *results = tab->resultPanel();

    // 커서가 있는 문장(선택 영역이 있으면 그 안의 첫 문장)의 계획.
    // 예상 계획은 문장을 실행하지 않고, 실제 계획(actual)은 실행한 뒤 바꾼 데이터를 되돌린다.
    const QList<SqlStatement> statements = tab->editor()->statementsToExecute(false);
    if (statements.isEmpty()) {
        results->appendLog(tr("Nothing to explain."));
        results->showLog();
        return;
    }
    if (!requireConnection(tab))
        return;
    if (statements.size() > 1)
        results->appendLog(tr("Explaining the first of %n selected statement(s).", nullptr, int(statements.size())));

    QList<SqlStatement> first{statements.first()};
    if (!applyBindVariables(tab, &first))
        return;
    const SqlStatement statement = first.first();
    QPointer<QuerySession> guard(tab->session());
    withConnectedSession(tab, [guard, statement, actual] {
        if (guard)
            guard->explain(statement, actual);
    });
}

bool MainWindow::applyBindVariables(QueryTab *tab, QList<SqlStatement> *statements)
{
    QList<BindVariable> variables;
    QList<QList<BindOccurrence>> occurrences;
    int positional = 0;
    for (const SqlStatement &statement : std::as_const(*statements))
        occurrences.append(SqlBinds::scan(statement.text, tab->dialect(), &variables, &positional));
    if (variables.isEmpty())
        return true;

    // &&로 이미 정한 치환 변수는 SQL*Plus처럼 다시 묻지 않는다.
    QList<SqlBinds::Value> values(variables.size());
    QList<int> ask;
    for (int i = 0; i < variables.size(); ++i) {
        const BindVariable &variable = variables[i];
        if (variable.kind == BindVariable::Kind::Substitution && tab->defines().contains(variable.key()))
            values[i] = {SqlBinds::Value::Type::Raw, tab->defines().value(variable.key())};
        else
            ask.append(i);
    }
    if (!ask.isEmpty()) {
        QList<BindVariable> asked;
        for (int i : std::as_const(ask))
            asked.append(variables[i]);
        BindDialog dialog(asked, m_app->bindValues(), this);
        if (dialog.exec() != QDialog::Accepted) {
            tab->resultPanel()->appendLog(tr("Cancelled: no values for the bind variables."));
            return false;
        }
        const QList<SqlBinds::Value> entered = dialog.values();
        for (int k = 0; k < ask.size(); ++k) {
            const BindVariable &variable = variables[ask[k]];
            values[ask[k]] = entered[k];
            m_app->bindValues().insert(variable.key(), entered[k]);
            if (variable.define)
                tab->setDefine(variable.key(), entered[k].text);
        }
    }

    QStringList replacements;
    for (int i = 0; i < variables.size(); ++i)
        replacements.append(SqlBinds::literal(variables[i], values[i], tab->dialect()));
    for (int k = 0; k < statements->size(); ++k) {
        if (occurrences[k].isEmpty())
            continue;
        SqlStatement &statement = (*statements)[k];
        statement.sourceText = statement.text;
        statement.text = SqlBinds::substitute(statement.sourceText, occurrences[k], replacements, &statement.edits);
    }
    return true;
}

void MainWindow::formatCurrent()
{
    QueryTab *tab = currentQueryTab();
    if (!tab)
        return;
    SqlEditor *editor = tab->editor();
    const QTextCursor cursor = editor->textCursor();
    int start = 0;
    int end = 0;
    if (cursor.hasSelection()) {
        start = cursor.selectionStart();
        end = cursor.selectionEnd();
    } else {
        const QList<SqlStatement> statements = editor->statementsToExecute(false);
        if (statements.isEmpty()) {
            statusBar()->showMessage(tr("Nothing to format."), 5000);
            return;
        }
        start = statements.first().start;
        end = statements.first().end;
    }
    const QString text = editor->document()->toPlainText().mid(start, end - start);
    QTextCursor startCursor(editor->document());
    startCursor.setPosition(start);
    const QString formatted = SqlFormatter::format(text, tab->dialect(), startCursor.positionInBlock());
    if (formatted == text) {
        statusBar()->showMessage(SqlFormatter::canFormat(text, tab->dialect())
                                     ? tr("Already formatted.")
                                     : tr("Only SELECT, INSERT, UPDATE, DELETE, MERGE and WITH statements are formatted."),
                                 5000);
        return;
    }
    QTextCursor edit(editor->document());
    edit.beginEditBlock(); // Ctrl+Z 한 번으로 되돌린다
    edit.setPosition(start);
    edit.setPosition(end, QTextCursor::KeepAnchor);
    edit.insertText(formatted);
    edit.endEditBlock();
    edit.setPosition(start);
    editor->setTextCursor(edit);
    editor->setFocus();
}

void MainWindow::showHistory()
{
    QueryTab *tab = currentQueryTab();
    HistoryDialog dialog(m_app->history().entries(), m_connectionId,
                         tab ? tab->dialect() : SqlDialect::Generic, this);
    const int result = dialog.exec();
    if (dialog.clearRequested()) {
        QString error;
        if (!m_app->history().clear(&error))
            QMessageBox::warning(this, tr("Execution History"), tr("Could not clear the history.\n%1").arg(error));
        else
            statusBar()->showMessage(tr("Execution history cleared."), 5000);
        return;
    }
    if (result == QDialog::Accepted && tab && !dialog.selectedSql().isEmpty())
        insertSql(tab, dialog.selectedSql());
}

void MainWindow::insertSql(QueryTab *tab, const QString &sql)
{
    // 커서 자리에 넣는다. 그 줄에 글이 있으면 다음 줄에 넣고, 다음 문장과 섞이지 않게 끝맺음(; 또는 PL/SQL의 /)을 붙인다.
    SqlEditor *editor = tab->editor();
    QString text = sql.trimmed();
    const QList<SqlStatement> parts = SqlSplitter::split(text + QStringLiteral(";\n"), tab->dialect());
    const bool semicolonEnds = parts.size() == 1 && parts.first().text == text;
    if (semicolonEnds)
        text += u';';
    else if (tab->dialect() == SqlDialect::Oracle || tab->dialect() == SqlDialect::Generic)
        text += QStringLiteral("\n/");

    QTextCursor cursor = editor->textCursor();
    if (!cursor.hasSelection() && !cursor.block().text().trimmed().isEmpty()) {
        cursor.movePosition(QTextCursor::EndOfBlock);
        text.prepend(QStringLiteral("\n"));
    }
    cursor.insertText(text);
    editor->setTextCursor(cursor);
    editor->setFocus();
}

void MainWindow::useSchema(const QString &metadataId, const QString &schema)
{
    // 지금 탭(없거나 다른 DB면 새 탭)의 스키마를 바꿔 스키마 이름 없이 조회하게 한다.
    if (Db::baseProfileId(metadataId) != m_connectionId)
        return;
    switchSchema(queryTabFor(metadataId), schema);
}

void MainWindow::switchDatabase(QueryTab *tab, const QString &chosen)
{
    const ConnectionProfile *p = profile();
    if (!tab || !p || chosen.isEmpty() || !Db::canSwitchDatabase(p->type))
        return;
    const QString database = chosen == p->database ? QString() : chosen; // 접속 설정의 DB면 기본으로
    if (database == tab->database())
        return;
    auto revert = [tab] { tab->connectionBar()->setDatabase(tab->database()); }; // 목록에서 고른 것을 되돌린다
    if (tab->session()->isBusy()) {
        tab->resultPanel()->appendLog(tr("The tab is busy. Change the database when the current statement finishes."), true);
        tab->resultPanel()->showLog();
        revert();
        return;
    }
    if (!confirmDiscardTransaction(tab, tr("switch to database %1").arg(chosen))) {
        revert();
        return;
    }

    // PostgreSQL은 세션 안에서 DB를 바꿀 수 없다: 끊고 그 DB로 다시 접속한다.
    // 스키마는 DB마다 다르므로 그 DB의 기본 스키마(search_path)에서 시작한다.
    tab->session()->disconnectFromServer();
    tab->setDatabase(database);
    tab->setPreferredSchema({});
    if (tab == currentQueryTab()) {
        m_activeDatabase = database;
        syncExplorerDatabase();
    }
    updateConnectionStatus();
    scheduleWorkspaceSave();
    QPointer<QueryTab> guard(tab);
    withConnectedSession(tab, [this, guard, chosen] {
        if (guard)
            statusBar()->showMessage(tr("Connected to database %1").arg(chosen), 5000);
    });
}

void MainWindow::switchSchema(QueryTab *tab, const QString &schema)
{
    if (!tab || schema.isEmpty())
        return;
    if (tab->session()->isBusy()) {
        tab->resultPanel()->appendLog(tr("The tab is busy. Change the schema when the current statement finishes."), true);
        tab->resultPanel()->showLog();
        tab->connectionBar()->setSchema(tab->currentSchema()); // 목록에서 고른 것을 되돌린다
        return;
    }
    QPointer<QueryTab> guard(tab);
    withConnectedSession(tab, [this, guard, schema] {
        if (!guard)
            return;
        guard->session()->setSchema(schema, [this, guard](bool ok) {
            if (!guard)
                return;
            if (!ok) {
                guard->connectionBar()->setSchema(guard->currentSchema());
                guard->resultPanel()->showLog();
                return;
            }
            guard->setPreferredSchema(guard->currentSchema()); // 다시 접속해도 이 스키마로
            scheduleWorkspaceSave();
            statusBar()->showMessage(tr("Current schema: %1").arg(guard->currentSchema()), 5000);
        });
    });
}

void MainWindow::showDdlAtCursor()
{
    // Database Explorer에서 객체를 고른 채 F4를 누르면 그 객체
    QString explorerConnection, explorerSchema, explorerName;
    if (m_explorer->tree()->hasFocus()
        && m_explorer->selectedObject(&explorerConnection, &explorerSchema, &explorerName)) {
        requestDdl(explorerConnection, explorerSchema, explorerName, true, true, currentQueryTab());
        return;
    }

    QueryTab *tab = currentQueryTab();
    if (!tab || !requireConnection(tab))
        return;
    const QString text = tab->editor()->objectNameAtCursor();
    const std::optional<QualifiedName> name = SqlIdentifiers::parseQualified(text);
    if (!name) {
        tab->resultPanel()->appendLog(tr("Select a table or object name in the editor, then press F4."), true);
        tab->resultPanel()->showLog();
        return;
    }
    // 스키마 없이 쓴 이름은 이 탭의 지금 스키마에서 찾는다 (requestDdl이 source 탭에서 가져간다)
    requestDdl(tab->metadataId(), name->schema, name->name, name->schemaQuoted, name->nameQuoted, tab);
}

void MainWindow::showTableDetailsAtCursor()
{
    QString explorerConnection, explorerSchema, explorerName;
    if (m_explorer->tree()->hasFocus() && m_explorer->selectedObject(&explorerConnection, &explorerSchema, &explorerName)) {
        openTableDetails(explorerConnection, explorerSchema, explorerName, true, true);
        return;
    }
    QueryTab *tab = currentQueryTab();
    if (!tab)
        return;
    const std::optional<QualifiedName> name = SqlIdentifiers::parseQualified(tab->editor()->objectNameAtCursor());
    if (!name) {
        tab->resultPanel()->appendLog(tr("Select a table name in the editor, then press Shift+F4."), true);
        tab->resultPanel()->showLog();
        return;
    }
    openTableDetails(tab->metadataId(), name->schema, name->name, name->schemaQuoted, name->nameQuoted, tab->currentSchema());
}

void MainWindow::openTableDetails(const QString &metadataId, const QString &schema, const QString &name, bool schemaQuoted,
                                  bool nameQuoted, const QString &currentSchema)
{
    if (!profile() || Db::baseProfileId(metadataId) != m_connectionId)
        return;
    const QString key = (schema.isEmpty() ? QString() : schema + u'.') + name;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *existing = qobject_cast<TableDetailTab *>(m_tabs->widget(i));
            existing && existing->property("dbaroDetailSource").toString() == metadataId
            && existing->property("dbaroDetailKey").toString().compare(key, Qt::CaseInsensitive) == 0) {
            m_tabs->setCurrentIndex(i);
            existing->refresh();
            return;
        }
    }
    auto *detail = new TableDetailTab(m_app->metadata(), metadataId, schema, name, schemaQuoted, nameQuoted,
                                      sqlDialectFor(profile()->type), m_tabs, currentSchema);
    detail->setProperty("dbaroDetailKey", key);
    detail->setProperty("dbaroDetailSource", metadataId); // 같은 이름이어도 DB가 다르면 다른 탭
    const int index = m_tabs->addTab(detail, Icons::get(QStringLiteral("table")), name);
    m_tabs->setTabToolTip(index, tr("Table details: %1").arg(key));
    m_tabs->setCurrentIndex(index);
    connect(detail, &TableDetailTab::loaded, this, [this, detail](const QString &title) {
        const int at = m_tabs->indexOf(detail);
        if (at >= 0) {
            m_tabs->setTabText(at, title);
            detail->setProperty("dbaroDetailKey", detail->schema() + u'.' + detail->name());
        }
    });
    connect(detail, &TableDetailTab::selectRowsRequested, this,
            [this, metadataId](const QString &s, const QString &n) { openObject(metadataId, s, n); });
    connect(detail, &TableDetailTab::importRequested, this,
            [this, metadataId](const QString &s, const QString &n) { importData(metadataId, s, n); });
}

void MainWindow::importData(const QString &metadataId, const QString &schema, const QString &table)
{
    const ConnectionProfile *p = profile();
    if (!p || Db::baseProfileId(metadataId) != m_connectionId)
        return;
    ImportDialog dialog(m_app->metadata(), metadataId, sqlDialectFor(p->type), schema, table, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QStringList statements = dialog.statements();
    if (statements.isEmpty())
        return;
    if (dialog.openAsSql()) {
        // 바로 실행하지 않고 새 탭에서 고쳐 실행할 수 있게
        QueryTab *sqlTab = addQueryTabOn(Db::databaseOf(metadataId));
        sqlTab->editor()->setPlainText(statements.join(QStringLiteral(";\n")) + u';');
        sqlTab->editor()->moveCursor(QTextCursor::Start);
        return;
    }
    QueryTab *tab = queryTabFor(metadataId); // 불러올 테이블이 있는 DB의 탭
    runImport(tab, statements, dialog.stopOnError(), dialog.targetDisplay());
}

void MainWindow::runImport(QueryTab *tab, const QStringList &statements, bool stopOnError, const QString &target)
{
    if (tab->session()->isBusy()) {
        QMessageBox::information(this, tr("Import Data"), tr("The tab is busy. Try again when the current statement finishes."));
        return;
    }
    QPointer<QueryTab> guard(tab);
    withConnectedSession(tab, [this, guard, statements, stopOnError, target] {
        if (!guard)
            return;
        // 500문장씩 엔진에 보낸다. 진행 상황을 보여 주고 취소할 수 있다.
        struct State
        {
            int next = 0;
            qint64 executed = 0;
            qint64 failed = 0;
            QStringList errors;
            bool cancelled = false;
        };
        auto state = std::make_shared<State>();
        auto *progress = new QProgressDialog(tr("Importing into %1 ...").arg(target), tr("Cancel"), 0, int(statements.size()), this);
        progress->setObjectName(QStringLiteral("importProgress"));
        progress->setWindowTitle(tr("Import Data"));
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(300);
        progress->setAutoClose(false);
        progress->setAutoReset(false);
        connect(progress, &QProgressDialog::canceled, this, [guard, state] {
            state->cancelled = true;
            if (guard)
                guard->session()->cancel(); // 엔진은 다음 문장 앞에서 멈춘다
        });
        guard->resultPanel()->appendLog(tr("Importing %n statement(s) into %1 ...", nullptr, int(statements.size())).arg(target));

        auto finish = [this, guard, state, progress, target, total = int(statements.size())] {
            progress->close();
            progress->deleteLater();
            if (!guard)
                return;
            m_app->metadata()->invalidate(guard->metadataId()); // 새 테이블·행이 자동완성·탐색기에 보이게
            QString message = tr("Import into %1: %2 of %3 statement(s) ran").arg(target).arg(state->executed).arg(total);
            if (state->failed > 0)
                message += tr(", %n failed", nullptr, int(state->failed));
            if (state->cancelled)
                message += tr(", cancelled");
            guard->resultPanel()->appendLog(message + u'.', state->failed > 0);
            for (const QString &error : std::as_const(state->errors))
                guard->resultPanel()->appendLog(error, true);
            guard->resultPanel()->showLog();
            statusBar()->showMessage(message, 10000);
            if (state->executed > 0 && guard->session()->hasPendingTransaction()) {
                const auto answer = QMessageBox::question(
                    this, tr("Import Data"),
                    tr("%1.\n\nCommit now? (Rollback undoes the rows inserted in this tab.)").arg(message),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (answer == QMessageBox::Yes)
                    guard->session()->commit();
            } else if (state->failed > 0) {
                QMessageBox::warning(this, tr("Import Data"),
                                     message + u'.' + u'\n' + state->errors.mid(0, 5).join(u'\n'));
            }
        };

        auto step = std::make_shared<std::function<void()>>();
        *step = [this, guard, state, statements, stopOnError, progress, finish, step] {
            if (!guard)
                return;
            constexpr int kChunk = 500;
            if (state->next >= statements.size() || state->cancelled || (stopOnError && state->failed > 0)) {
                finish();
                return;
            }
            const QStringList chunk = statements.mid(state->next, kChunk);
            const int offset = state->next;
            guard->session()->executeBatch(chunk, stopOnError, [guard, state, chunk, offset, progress, step, finish](
                                                                   const QCborMap &result, const EngineError &error) {
                if (!guard)
                    return;
                if (error.isError()) {
                    state->failed += chunk.size();
                    state->errors << error.message;
                    finish();
                    return;
                }
                state->executed += result.value(QStringLiteral("executed")).toInteger();
                state->failed += result.value(QStringLiteral("failed")).toInteger();
                if (result.value(QStringLiteral("cancelled")).toBool())
                    state->cancelled = true;
                for (const QCborValue &item : result.value(QStringLiteral("errors")).toArray()) {
                    const QCborMap e = item.toMap();
                    if (state->errors.size() < 20)
                        state->errors << QObject::tr("Row %1: %2")
                                             .arg(offset + e.value(QStringLiteral("index")).toInteger() + 1)
                                             .arg(e.value(QStringLiteral("message")).toString());
                }
                state->next = offset + int(chunk.size());
                progress->setValue(state->next);
                (*step)();
            });
        };
        (*step)();
    });
}

void MainWindow::requestDdl(const QString &connectionId, const QString &schema, const QString &name, bool schemaQuoted,
                            bool nameQuoted, QueryTab *source)
{
    // connectionId는 메타데이터 id다 (PostgreSQL에서 다른 DB를 고른 탭이면 그 DB)
    if (!m_app->profile(Db::baseProfileId(connectionId)))
        return;
    QCborMap params;
    if (!schema.isEmpty())
        params.insert(QStringLiteral("schema"), schema);
    params.insert(QStringLiteral("name"), name);
    params.insert(QStringLiteral("schemaQuoted"), schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), nameQuoted);
    // 메타데이터 세션은 탭이 바꾼 스키마를 모르므로, 스키마 없이 쓴 이름을 찾을 스키마를 함께 보낸다.
    if (schema.isEmpty() && source && !source->currentSchema().isEmpty())
        params.insert(QStringLiteral("currentSchema"), source->currentSchema());

    const QString display = schema.isEmpty() ? name : schema + u'.' + name;
    statusBar()->showMessage(tr("Generating DDL for %1 ...").arg(display));
    QPointer<QueryTab> origin(source);
    // 오래 걸리면 F4를 누른 탭의 에디터 오른쪽 위에 기다리는 중임을 보인다
    const int ticket = source ? source->beginActivity(tr("Generating DDL for %1...").arg(display)) : 0;
    // DDL은 메타데이터 세션에서 만든다 (탭에서 쿼리가 실행 중이어도 된다). 늘 새로 읽는다.
    m_app->metadata()->request(
        connectionId, QStringLiteral("ddl"), params,
        [this, origin, ticket, connectionId, display](const QCborMap &result, const QString &error) {
            if (origin && ticket)
                origin->endActivity(ticket);
            if (!error.isEmpty()) {
                statusBar()->showMessage(tr("Could not generate the DDL of %1.").arg(display), 8000);
                if (origin) {
                    origin->resultPanel()->appendLog(tr("DDL of %1 failed: %2").arg(display, error), true);
                    origin->resultPanel()->showLog();
                }
                return;
            }
            openDdlTab(connectionId, result);
        },
        true, true);
}

void MainWindow::showPlanObject(QueryTab *tab, const QCborMap &object)
{
    if (!profile())
        return;
    if (!m_planObjectWindow) {
        m_planObjectWindow = new PlanObjectWindow(this);
        connect(m_planObjectWindow, &PlanObjectWindow::tableDetailsRequested, this,
                [this](const QString &schema, const QString &name) {
                    openTableDetails(m_planObjectSource, schema, name, true, true);
                    activateWindow();
                });
    }
    QCborMap params;
    for (const QString &key : {QStringLiteral("schema"), QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("index")}) {
        const QString value = object.value(key).toString();
        if (!value.isEmpty())
            params.insert(key, value);
    }
    // 메타데이터 세션은 탭이 바꾼 스키마를 모르므로 함께 보낸다 (계획에 스키마가 없을 때: PostgreSQL, MySQL)
    if (tab && !tab->currentSchema().isEmpty())
        params.insert(QStringLiteral("currentSchema"), tab->currentSchema());

    const QString schema = object.value(QStringLiteral("schema")).toString();
    const QString name = object.value(QStringLiteral("name")).toString();
    const QString display = schema.isEmpty() ? name : schema + u'.' + name;
    PlanObjectWindow *window = m_planObjectWindow;
    const int ticket = window->beginLoading(display);
    if (!window->isVisible()) {
        // 처음에는 연결 창 오른쪽 위에 겹쳐 놓는다
        const QRect area = geometry();
        window->move(area.right() - window->width() - 40, area.top() + 80);
    }
    window->show();
    window->raise();

    QPointer<PlanObjectWindow> guard(window);
    m_planObjectSource = tab ? tab->metadataId() : activeMetadataId(); // 계획을 얻은 탭의 DB
    m_app->metadata()->request(
        m_planObjectSource, QStringLiteral("planObject"), params,
        [guard, ticket](const QCborMap &result, const QString &error) {
            if (!guard)
                return;
            if (error.isEmpty())
                guard->showResult(ticket, result);
            else
                guard->showError(ticket, error);
        },
        true, true);
}

void MainWindow::openDdlTab(const QString &connectionId, const QCborMap &result)
{
    const QString schema = result.value(QStringLiteral("schema")).toString();
    const QString name = result.value(QStringLiteral("name")).toString();
    const QString type = result.value(QStringLiteral("type")).toString();
    const QString key = connectionId + u'|' + schema + u'.' + name;

    // 같은 객체의 DDL 탭이 이미 있으면 내용만 새로 바꾼다.
    QueryTab *tab = nullptr;
    for (QueryTab *candidate : queryTabs()) {
        if (candidate->property("dbaroDdlKey").toString() == key) {
            tab = candidate;
            break;
        }
    }
    if (!tab) {
        tab = addQueryTabOn(Db::databaseOf(connectionId)); // 그 객체가 있는 DB로
        tab->setProperty("dbaroDdlKey", key);
        tab->setProperty(kTitleSource, kFixedTitle);
    }
    const int index = m_tabs->indexOf(tab);
    m_tabs->setTabText(index, tr("%1 DDL").arg(name));
    m_tabs->setTabToolTip(index, tr("DDL of %1.%2 (%3)").arg(schema, name, type));
    tab->editor()->setPlainText(result.value(QStringLiteral("script")).toString());
    tab->editor()->moveCursor(QTextCursor::Start);
    m_tabs->setCurrentWidget(tab);
    tab->editor()->setFocus();
    statusBar()->showMessage(tr("DDL of %1.%2 (%3)").arg(schema, name, type), 8000);
}

bool MainWindow::requireConnection(QueryTab *tab)
{
    if (profile())
        return true;
    tab->resultPanel()->appendLog(tr("This connection no longer exists."), true);
    tab->resultPanel()->showLog();
    return false;
}

void MainWindow::withConnectedSession(QueryTab *tab, std::function<void()> work)
{
    QuerySession *session = tab->session();
    if (session->isBusy())
        return;
    const ConnectionProfile *profile = this->profile();
    if (!profile)
        return;
    if (session->isConnecting()) {
        // 창을 열 때 시작한 접속이 아직 끝나지 않았다: 끝나면 이어서 한다.
        session->whenConnected([work](bool ok) {
            if (ok)
                work();
        });
        return;
    }
    if (session->isConnected() && session->profileId() == profile->id) {
        work();
        return;
    }
    m_autoConnectBlocked = false; // 사용자가 직접 실행했다: 이제 다른 탭도 다시 저절로 접속해 본다

    QString password;
    if (!m_app->passwordFor(*profile, &password, true, this))
        return;
    const QString profileId = profile->id;
    // 이 탭에서 고른 DB(PostgreSQL)와 스키마로 접속한다
    ConnectSpec spec = m_app->connectSpec(profileOn(tab->database()), password, m_autoCommitAction->isChecked());
    spec.schema = tab->preferredSchema();
    QPointer<QuerySession> guard(session);
    session->connectTo(spec, profileId, [this, guard, work, profileId](bool ok) {
        if (!guard)
            return;
        if (!ok) {
            m_app->forgetPassword(profileId); // 잘못 입력한 비밀번호는 다시 묻는다
            return;
        }
        work();
    });
}

void MainWindow::connectCurrentTab()
{
    QueryTab *tab = currentQueryTab();
    if (!tab || m_autoConnectBlocked || !m_app->autoConnect() || !isVisible())
        return;
    QuerySession *session = tab->session();
    if (session->isConnected() || session->isConnecting() || session->isBusy())
        return;
    const ConnectionProfile *profile = this->profile();
    if (!profile)
        return;

    QString password;
    if (!m_app->passwordFor(*profile, &password, true, this)) {
        m_autoConnectBlocked = true; // 묻는 창을 닫았다: 실행할 때 다시 묻는다
        return;
    }
    const QString profileId = profile->id;
    // 이 탭에서 고른 DB(PostgreSQL)와 스키마로 접속한다
    ConnectSpec spec = m_app->connectSpec(profileOn(tab->database()), password, m_autoCommitAction->isChecked());
    spec.schema = tab->preferredSchema();
    session->connectTo(spec, profileId, [this, profileId](bool ok) {
        if (ok)
            return;
        m_app->forgetPassword(profileId);
        m_autoConnectBlocked = true; // 서버가 꺼져 있으면 탭을 고를 때마다 실패하지 않게
    });
    m_explorer->expandConnection(m_explorerId); // 같은 비밀번호로 스키마 목록도 바로 가져온다 (탐색기는 이 탭의 DB)
}

bool MainWindow::confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements)
{
    const ConnectionProfile *profile = this->profile();
    const bool production = profile && profile->environment == Environment::Production;

    for (const SqlStatement &statement : statements) {
        if (profile && profile->readOnly && SqlSafety::mayModify(statement.text)) {
            tab->resultPanel()->appendLog(
                tr("Blocked on a read-only connection: %1").arg(preview(statement.text)), true);
            tab->resultPanel()->showLog();
            return false;
        }
        if (SqlSafety::isUnfilteredUpdateOrDelete(statement.text)) {
            const auto answer = QMessageBox::warning(
                this, tr("No WHERE Clause"),
                tr("This statement changes every row in the table:\n\n%1\n\nRun it anyway?").arg(preview(statement.text)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes)
                return false;
        }
        if (production && SqlSafety::isDestructiveDdl(statement.text)) {
            const auto answer = QMessageBox::warning(
                this, tr("Production Database"),
                tr("You are about to run this on the PRODUCTION connection \"%1\":\n\n%2\n\nRun it anyway?")
                    .arg(profile->name, preview(statement.text)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes)
                return false;
        }
    }
    return true;
}

bool MainWindow::confirmDiscardTransaction(QueryTab *tab, const QString &action)
{
    if (!tab->session()->hasPendingTransaction())
        return true;
    const auto answer = QMessageBox::warning(
        this, tr("Uncommitted Changes"),
        tr("This tab has uncommitted changes. They will be rolled back if you %1.").arg(action),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Discard;
}

void MainWindow::updateConnectionStatus()
{
    QueryTab *tab = currentQueryTab();
    const ConnectionProfile *profile = this->profile();
    if (!profile) {
        m_connectionLabel->setText(tr("No connection"));
        return;
    }

    const QuerySession *session = tab ? tab->session() : nullptr;
    QString state;
    if (auto *console = qobject_cast<ConsoleTab *>(m_tabs->currentWidget())) {
        state = console->isBusy() ? (console->isConnected() ? tr("CLI, running...") : tr("CLI, connecting..."))
                                  : (console->isConnected() ? tr("CLI, connected") : tr("CLI, not connected"));
        if (console->hasPendingTransaction())
            state += tr(" · uncommitted changes");
    } else if (!session)
        state = tr("no tab");
    else if (session->isConnecting())
        state = tr("connecting...");
    else if (session->isConnected())
        state = session->isBusy() ? tr("running...") : tr("connected");
    else
        state = tr("not connected");

    // PostgreSQL에서 다른 DB를 고른 탭이면 그 DB도
    const QString name = tab && !tab->database().isEmpty()
                             ? QStringLiteral("%1 · %2").arg(profile->name, tab->database())
                             : profile->name;
    QString text = tr("%1: %2 (%3)").arg(Db::typeName(profile->type), name, state);
    if (profile->environment == Environment::Production)
        text += tr(" · PRODUCTION");
    if (profile->readOnly)
        text += tr(" · read-only");
    if (session && session->hasPendingTransaction())
        text += tr(" · uncommitted changes");
    m_connectionLabel->setText(text);
}

void MainWindow::updateActionStates()
{
    const QueryTab *tab = currentQueryTab();
    const QuerySession *session = tab ? tab->session() : nullptr;
    const bool busy = session && session->isBusy(); // 접속 중에 실행하면 접속이 끝난 뒤 실행한다
    const bool connected = session && session->isConnected();
    const bool manual = !m_autoCommitAction->isChecked();

    m_executeAction->setEnabled(tab && !busy);
    m_executeScriptAction->setEnabled(tab && !busy);
    m_explainAction->setEnabled(tab && !busy);
    m_explainActualAction->setEnabled(tab && !busy);
    m_ddlAction->setEnabled(true); // 탭이 없어도 탐색기에서 쓸 수 있다
    m_cancelAction->setEnabled(session && session->isBusy());
    m_commitAction->setEnabled(connected && manual && !busy);
    m_rollbackAction->setEnabled(connected && manual && !busy);
}

void MainWindow::updateCursorPosition(int line, int column)
{
    m_cursorLabel->setText(tr("Ln %1, Col %2").arg(line).arg(column));
}

void MainWindow::updateTransactionLabel()
{
    const QString mode = m_autoCommitAction->isChecked() ? tr("Auto-commit") : tr("Manual commit");
    m_transactionLabel->setText(mode);
    m_transactionButton->setText(mode + QStringLiteral(" ▾"));
}
