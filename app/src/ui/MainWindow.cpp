#include "MainWindow.h"
#include "MetadataService.h"

#include "ConnectionDialog.h"
#include "BindDialog.h"
#include "EditorTabs.h"
#include "FindBar.h"
#include "HistoryDialog.h"
#include "Icons.h"
#include "ObjectExplorer.h"
#include "QuerySession.h"
#include "QueryTab.h"
#include "ResultPanel.h"
#include "SqlEditor.h"
#include "Theme.h"
#include "UiHelpers.h"
#include "core/ConnectSpec.h"
#include "core/SecretStore.h"
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
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("DBaro"));
    resize(1280, 800);

    m_engine = new EngineClient(this);
    connect(m_engine, &EngineClient::engineLog, this, [this](const QString &line) {
        // 사용자에게 의미 있는 줄(드라이버 다운로드, 오류)만 상태 표시줄에 보여 준다.
        QString text = line;
        text.remove(QStringLiteral("[engine] "));
        if (text.startsWith(QStringLiteral("Downloading")) || text.startsWith(QStringLiteral("Saved"))
            || text.startsWith(QStringLiteral("ERROR")))
            statusBar()->showMessage(text, 8000);
    });

    m_metadata = new MetadataService(m_engine, this);
    m_metadata->setSpecProvider([this](const ConnectionProfile &profile, ConnectSpec *spec, bool interactive) {
        QString password;
        if (!passwordFor(profile, &password, interactive))
            return false;
        *spec = ConnectSpec::fromProfile(profile, password, true);
        return true;
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
            // 연결을 다시 고를 때 돌아갈 탭을 알 수 있게 최근에 쓴 순서를 기억한다.
            m_tabHistory.removeAll(nullptr);
            m_tabHistory.removeAll(tab);
            m_tabHistory.prepend(tab);
            m_encodingLabel->setText(tab->filePath().isEmpty() ? QStringLiteral("UTF-8")
                                                               : TextFile::encodingName(tab->fileFormat()));
        }
        syncConnectionComboToCurrentTab();
        updateActionStates();
        scheduleWorkspaceSave();
    });

    loadConnections();
    if (!restoreWorkspace())
        addQueryTab();
    restoreLayout();
    updateActionStates();
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
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    m_newConnectionAction = new QAction(Icons::get(QStringLiteral("database-add")), tr("New Connection..."), this);
    connect(m_newConnectionAction, &QAction::triggered, this, [this] { openConnectionDialog(true); });

    m_manageConnectionsAction = new QAction(Icons::get(QStringLiteral("database")), tr("Connections..."), this);
    m_manageConnectionsAction->setToolTip(tr("Add, edit and delete connections"));
    connect(m_manageConnectionsAction, &QAction::triggered, this, [this] { openConnectionDialog(false); });

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
    connect(m_explainAction, &QAction::triggered, this, &MainWindow::explainCurrent);

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
    m_explorer = new ObjectExplorer(m_metadata, this);
    connect(m_explorer, &ObjectExplorer::connectionActivated, this, &MainWindow::activateConnection);
    connect(m_explorer, &ObjectExplorer::newQueryRequested, this, &MainWindow::addQueryTabFor);
    connect(m_explorer, &ObjectExplorer::openObjectRequested, this, &MainWindow::openObject);
    connect(m_explorer, &ObjectExplorer::useSchemaRequested, this, &MainWindow::useSchema);
    connect(m_explorer, &ObjectExplorer::ddlRequested, this,
            [this](const QString &connectionId, const QString &schema, const QString &name) {
                // 탐색기의 이름은 DB에 저장된 그대로이므로 따옴표로 감싼 것처럼 대소문자를 그대로 쓴다.
                requestDdl(connectionId, schema, name, true, true, currentQueryTab());
            });
    connect(m_explorer, &ObjectExplorer::insertTextRequested, this, [this](const QString &text) {
        if (QueryTab *tab = currentQueryTab()) {
            tab->editor()->insertPlainText(text);
            tab->editor()->setFocus();
        }
    });
    connect(m_explorer, &ObjectExplorer::editConnectionRequested, this,
            [this](const QString &id) { openConnectionDialog(false, id); });

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
    fileMenu->addAction(m_openFileAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_closeTabAction);
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
    databaseMenu->addAction(m_newConnectionAction);
    databaseMenu->addAction(m_manageConnectionsAction);

    QMenu *queryMenu = menuBar()->addMenu(tr("&Query"));
    queryMenu->addAction(m_executeAction);
    queryMenu->addAction(m_executeScriptAction);
    queryMenu->addAction(m_explainAction);
    queryMenu->addAction(m_ddlAction);
    queryMenu->addAction(m_historyAction);
    queryMenu->addAction(m_exportAction);
    queryMenu->addAction(m_cancelAction);
    queryMenu->addSeparator();
    queryMenu->addAction(m_commitAction);
    queryMenu->addAction(m_rollbackAction);
    queryMenu->addAction(m_autoCommitAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_explorerDock->toggleViewAction());
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

    // 왼쪽: 연결
    m_connectionCombo = new QComboBox(toolBar);
    m_connectionCombo->setObjectName(QStringLiteral("connectionCombo"));
    m_connectionCombo->setMinimumContentsLength(22);
    m_connectionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_connectionCombo->setToolTip(tr("Connection. Switches to the tab you last used with it"));
    m_connectionCombo->setPlaceholderText(tr("No connection"));
    connect(m_connectionCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_syncingCombo && index >= 0)
            activateConnection(m_connectionCombo->itemData(index).toString());
    });
    toolBar->addWidget(m_connectionCombo);
    toolBar->addAction(m_manageConnectionsAction);
    toolBar->addAction(m_newTabAction);

    // 가운데 여백: 실행 관련 버튼을 오른쪽 끝으로 민다
    auto *spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(spacer);

    // 오른쪽: 실행과 트랜잭션
    toolBar->addAction(m_executeAction);
    toolBar->addAction(m_executeScriptAction);
    toolBar->addAction(m_explainAction);
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
    QSettings settings;
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
    restoreState(settings.value(kStateKey).toByteArray());
}

void MainWindow::loadConnections()
{
    QString error;
    if (!m_store.load(&error))
        statusBar()->showMessage(tr("Could not load connections: %1").arg(error));
    refreshConnectionViews();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    int pending = 0;
    for (QueryTab *tab : queryTabs())
        pending += tab->session()->hasPendingTransaction() ? 1 : 0;
    if (pending > 0) {
        const auto answer = QMessageBox::warning(
            this, tr("Uncommitted Changes"),
            tr("%n tab(s) have uncommitted changes. They will be rolled back if you exit.", nullptr, pending),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            event->ignore();
            return;
        }
    }

    // 저장하지 않은 파일도 묻지 않고 작업 공간에 남긴다 (다음에 열면 고친 내용 그대로 돌아온다).
    saveWorkspace();
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kStateKey, saveState());
    QMainWindow::closeEvent(event);
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
        saved.connectionId = tab->connectionId();
        saved.filePath = tab->filePath();
        saved.format = tab->fileFormat();
        saved.modified = tab->hasUnsavedFile();
        // 저장한 그대로인 파일은 다음에 파일에서 다시 읽는다.
        if (saved.filePath.isEmpty() || saved.modified)
            saved.sql = tab->editor()->toPlainText();
        saved.cursor = tab->editor()->textCursor().position();
        saved.ddlKey = tab->property("dbaroDdlKey").toString();
        workspace.tabs.append(saved);
    }
    workspace.current = m_tabs->currentIndex();
    QString error;
    if (!m_workspaceStore.save(workspace, &error))
        statusBar()->showMessage(tr("Could not save the open tabs: %1").arg(error), 8000);
}

bool MainWindow::restoreWorkspace()
{
    Workspace workspace;
    QString error;
    if (!m_workspaceStore.load(&workspace, &error)) {
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
        QueryTab *tab = addQueryTabFor(saved.connectionId);
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
    if (QueryTab *tab = currentQueryTab())
        tab->editor()->setFocus();
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
    QString toolTip = QDir::toNativeSeparators(tab->filePath());
    if (const ConnectionProfile *profile = m_store.find(tab->connectionId()))
        toolTip += QStringLiteral("\n%1 — %2").arg(profile->name, Db::typeName(profile->type));
    m_tabs->setTabToolTip(index, toolTip);
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

QueryTab *MainWindow::addQueryTab()
{
    // 새 탭은 지금 탭의 연결을 그대로 쓴다 (세션은 따로 연다).
    return addQueryTabFor(currentQueryTab() ? currentQueryTab()->connectionId() : QString());
}

QueryTab *MainWindow::addQueryTabFor(const QString &connectionId)
{
    auto *tab = new QueryTab(m_engine, m_tabs);
    tab->setMetadataService(m_metadata);
    tab->session()->setAutoCommit(m_autoCommitAction->isChecked());
    connect(tab, &QueryTab::cursorPositionChanged, this, [this, tab](int line, int column) {
        if (tab == currentQueryTab())
            updateCursorPosition(line, column);
    });
    connect(tab->session(), &QuerySession::stateChanged, this, [this, tab] {
        if (tab == currentQueryTab()) {
            updateConnectionStatus();
            updateActionStates();
        }
    });
    connect(tab->editor(), &QPlainTextEdit::textChanged, this, &MainWindow::scheduleWorkspaceSave);
    connect(tab->resultPanel(), &ResultPanel::exportFinished, this,
            [this](const QString &message) { statusBar()->showMessage(message, 10000); });
    connect(tab->session(), &QuerySession::statementFinished, this,
            [this, tab](const SqlStatement &statement, bool ok, qint64 elapsedMs, qint64 rows, const QString &error) {
                HistoryEntry entry;
                entry.connectionId = tab->connectionId();
                if (const ConnectionProfile *profile = m_store.find(entry.connectionId))
                    entry.connectionName = profile->name;
                entry.sql = statement.originalText();
                entry.ok = ok;
                entry.elapsedMs = elapsedMs;
                entry.rows = rows;
                entry.error = error;
                m_history.add(entry);
            });
    connect(tab->editor()->document(), &QTextDocument::modificationChanged, this, [this, tab] { updateFileTabTitle(tab); });

    const int index = m_tabs->addTab(tab, tr("Query %1").arg(++m_tabCounter));
    setTabConnection(tab, connectionId);
    m_tabs->setCurrentIndex(index);
    syncConnectionComboToCurrentTab();
    tab->editor()->setFocus();
    return tab;
}

void MainWindow::openObject(const QString &connectionId, const QString &schema, const QString &name)
{
    const ConnectionProfile *profile = m_store.find(connectionId);
    if (!profile)
        return;
    // 테이블·뷰를 두 번 클릭하면 새 탭에서 바로 조회한다 (결과는 페이지 단위로 가져온다).
    QueryTab *tab = addQueryTabFor(connectionId);
    tab->setProperty(kTitleSource, kFixedTitle); // 연결 이름 대신 객체 이름을 제목으로 둔다
    m_tabs->setTabText(m_tabs->indexOf(tab), name);
    tab->editor()->setPlainText(
        QStringLiteral("SELECT *\n  FROM %1;").arg(SqlIdentifiers::qualified(schema, name, sqlDialectFor(profile->type))));
    executeCurrent(false);
}

void MainWindow::closeQueryTab(int index)
{
    auto *tab = qobject_cast<QueryTab *>(m_tabs->widget(index));
    if (!tab)
        return;
    if (!confirmCloseUnsaved(tab) || !confirmDiscardTransaction(tab, tr("close this tab")))
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
    if (session->isBusy() || session->isConnecting())
        return;

    QList<SqlStatement> statements = tab->editor()->statementsToExecute(wholeScript);
    if (statements.isEmpty()) {
        results->appendLog(tr("Nothing to execute."));
        results->showLog();
        return;
    }

    if (!requireConnection(tab) || !applyBindVariables(tab, &statements) || !confirmSafeToRun(tab, statements))
        return;

    if (statements.size() > 1)
        results->appendLog(tr("Running %n statement(s) ...", nullptr, int(statements.size())));

    QPointer<QuerySession> guard(session);
    withConnectedSession(tab, [guard, statements] {
        if (guard)
            guard->run(statements);
    });
}

void MainWindow::explainCurrent()
{
    QueryTab *tab = currentQueryTab();
    if (!tab || tab->session()->isBusy() || tab->session()->isConnecting())
        return;
    ResultPanel *results = tab->resultPanel();

    // 커서가 있는 문장(선택 영역이 있으면 그 안의 첫 문장)의 예상 계획. 문장은 실행하지 않는다.
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
    withConnectedSession(tab, [guard, statement] {
        if (guard)
            guard->explain(statement);
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
        BindDialog dialog(asked, m_bindValues, this);
        if (dialog.exec() != QDialog::Accepted) {
            tab->resultPanel()->appendLog(tr("Cancelled: no values for the bind variables."));
            return false;
        }
        const QList<SqlBinds::Value> entered = dialog.values();
        for (int k = 0; k < ask.size(); ++k) {
            const BindVariable &variable = variables[ask[k]];
            values[ask[k]] = entered[k];
            m_bindValues.insert(variable.key(), entered[k]);
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
    HistoryDialog dialog(m_history.entries(), tab ? tab->connectionId() : QString(),
                         tab ? tab->dialect() : SqlDialect::Generic, this);
    const int result = dialog.exec();
    if (dialog.clearRequested()) {
        QString error;
        if (!m_history.clear(&error))
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

void MainWindow::useSchema(const QString &connectionId, const QString &schema)
{
    // MySQL·MariaDB: 그 연결의 탭(최근에 쓴 탭, 없으면 새 탭)에서 USE를 실행해 스키마 이름 없이 조회하게 한다.
    activateConnection(connectionId);
    QueryTab *tab = currentQueryTab();
    if (!tab || tab->connectionId() != connectionId)
        return;
    if (tab->session()->isBusy() || tab->session()->isConnecting()) {
        tab->resultPanel()->appendLog(tr("The tab is busy. Try USE again when the current statement finishes."), true);
        tab->resultPanel()->showLog();
        return;
    }

    SqlStatement statement;
    statement.start = -1; // 에디터에 없는 문장
    statement.text = QStringLiteral("USE %1").arg(SqlIdentifiers::quote(schema, SqlDialect::MySql));
    QPointer<QuerySession> guard(tab->session());
    withConnectedSession(tab, [guard, statement] {
        if (guard)
            guard->run({statement});
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
    QString schema = name->schema;
    bool schemaQuoted = name->schemaQuoted;
    if (schema.isEmpty() && tab->dialect() == SqlDialect::MySql && !tab->currentSchema().isEmpty()) {
        schema = tab->currentSchema(); // 이 탭에서 USE한 데이터베이스
        schemaQuoted = true;
    }
    requestDdl(tab->connectionId(), schema, name->name, schemaQuoted, name->nameQuoted, tab);
}

void MainWindow::requestDdl(const QString &connectionId, const QString &schema, const QString &name, bool schemaQuoted,
                            bool nameQuoted, QueryTab *source)
{
    if (!m_store.find(connectionId))
        return;
    QCborMap params;
    if (!schema.isEmpty())
        params.insert(QStringLiteral("schema"), schema);
    params.insert(QStringLiteral("name"), name);
    params.insert(QStringLiteral("schemaQuoted"), schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), nameQuoted);

    const QString display = schema.isEmpty() ? name : schema + u'.' + name;
    statusBar()->showMessage(tr("Generating DDL for %1 ...").arg(display));
    QPointer<QueryTab> origin(source);
    // DDL은 메타데이터 세션에서 만든다 (탭에서 쿼리가 실행 중이어도 된다). 늘 새로 읽는다.
    m_metadata->request(
        connectionId, QStringLiteral("ddl"), params,
        [this, origin, connectionId, display](const QCborMap &result, const QString &error) {
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
        tab = addQueryTabFor(connectionId);
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
    if (m_store.find(tab->connectionId()))
        return true;
    tab->resultPanel()->appendLog(tr("Select a connection for this tab first."), true);
    tab->resultPanel()->showLog();
    m_connectionCombo->setFocus();
    return false;
}

void MainWindow::withConnectedSession(QueryTab *tab, std::function<void()> work)
{
    QuerySession *session = tab->session();
    if (session->isBusy() || session->isConnecting())
        return;
    const ConnectionProfile *profile = m_store.find(tab->connectionId());
    if (!profile)
        return;
    if (session->isConnected() && session->profileId() == profile->id) {
        work();
        return;
    }

    QString password;
    if (!passwordFor(*profile, &password))
        return;
    const QString profileId = profile->id;
    const ConnectSpec spec = ConnectSpec::fromProfile(*profile, password, m_autoCommitAction->isChecked());
    QPointer<QuerySession> guard(session);
    session->connectTo(spec, profileId, [this, guard, work, profileId](bool ok) {
        if (!guard)
            return;
        if (!ok) {
            m_sessionPasswords.remove(profileId); // 잘못 입력한 비밀번호는 다시 묻는다
            return;
        }
        work();
    });
}

bool MainWindow::confirmSafeToRun(QueryTab *tab, const QList<SqlStatement> &statements)
{
    const ConnectionProfile *profile = m_store.find(tab->connectionId());
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

bool MainWindow::passwordFor(const ConnectionProfile &profile, QString *password, bool interactive)
{
    if (profile.savePassword) {
        if (const std::optional<QString> saved = SecretStore::loadPassword(profile.id)) {
            *password = *saved;
            return true;
        }
    }
    if (m_sessionPasswords.contains(profile.id)) {
        *password = m_sessionPasswords.value(profile.id);
        return true;
    }

    if (!interactive)
        return false; // 자동완성처럼 사용자가 요청하지 않은 조회는 비밀번호를 묻지 않는다

    bool ok = false;
    const QString entered = QInputDialog::getText(
        this, tr("Password"), tr("Password for %1 on \"%2\":").arg(profile.user, profile.name), QLineEdit::Password,
        QString(), &ok);
    if (!ok)
        return false;
    m_sessionPasswords.insert(profile.id, entered);
    *password = entered;
    return true;
}

void MainWindow::openConnectionDialog(bool startNew, const QString &selectId)
{
    ConnectionDialog dialog(m_store.profiles(), this);
    dialog.setEngine(m_engine);
    if (startNew) {
        dialog.startNewConnection();
    } else if (!selectId.isEmpty()) {
        dialog.selectConnection(selectId);
    } else if (QueryTab *tab = currentQueryTab(); tab && !tab->connectionId().isEmpty()) {
        dialog.selectConnection(tab->connectionId());
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_store.setProfiles(dialog.profiles());
    QString error;
    if (!m_store.save(&error)) {
        QMessageBox::warning(this, tr("Connections"), tr("Could not save the connections.\n%1").arg(error));
    }

    // 비밀번호는 연결 파일이 아니라 OS 보안 저장소에 반영한다.
    for (const QString &id : dialog.removedIds())
        SecretStore::removePassword(id);

    const QHash<QString, QString> pending = dialog.pendingPasswords();
    QStringList failed;
    for (const ConnectionProfile &profile : m_store.profiles()) {
        m_sessionPasswords.remove(profile.id);
        if (!profile.savePassword) {
            SecretStore::removePassword(profile.id);
            continue;
        }
        const auto it = pending.constFind(profile.id);
        if (it != pending.constEnd() && !SecretStore::savePassword(profile.id, profile.user, it.value(), &error))
            failed << profile.name;
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, tr("Connections"),
                             tr("Could not save the password for: %1\n%2").arg(failed.join(QStringLiteral(", ")), error));
    }

    refreshConnectionViews();

    // 연결을 고르지 않은 탭이면 대화상자에서 선택한 연결을 바로 쓴다.
    QueryTab *tab = currentQueryTab();
    if (tab && tab->connectionId().isEmpty() && !dialog.selectedId().isEmpty()) {
        setTabConnection(tab, dialog.selectedId());
        syncConnectionComboToCurrentTab();
    }
}

void MainWindow::refreshConnectionViews()
{
    m_syncingCombo = true;
    m_connectionCombo->clear(); // 연결이 없는 탭이면 아무것도 고르지 않은 상태("No connection")로 보인다
    for (const ConnectionProfile &profile : m_store.profiles())
        m_connectionCombo->addItem(colorSwatchIcon(Env::color(profile.environment)), profile.name, profile.id);
    m_syncingCombo = false;

    m_metadata->setProfiles(m_store.profiles());
    m_explorer->setConnections(m_store.profiles());

    // 지워진 연결을 쓰던 탭은 연결을 해제하고, 나머지는 색상 등 바뀐 설정을 다시 반영한다.
    for (QueryTab *tab : queryTabs())
        setTabConnection(tab, m_store.find(tab->connectionId()) ? tab->connectionId() : QString());
    syncConnectionComboToCurrentTab();
}

void MainWindow::activateConnection(const QString &connectionId)
{
    // 탭은 연결마다 따로 둔다. 다른 연결을 고르면 지금 탭의 연결을 바꾸지 않고,
    // 그 연결로 마지막에 쓰던 탭으로 옮겨 간다 (없으면 새 탭). 작성 중인 SQL과 세션은 탭마다 그대로 남는다.
    if (!m_store.find(connectionId)) {
        syncConnectionComboToCurrentTab();
        return;
    }
    QueryTab *current = currentQueryTab();
    if (current && current->connectionId() == connectionId) {
        syncConnectionComboToCurrentTab();
        return;
    }
    if (QueryTab *tab = recentTabFor(connectionId)) {
        m_tabs->setCurrentWidget(tab);
        tab->editor()->setFocus();
        return;
    }
    if (current && current->connectionId().isEmpty()) {
        // 연결 없이 쓰던 탭은 고른 연결을 그대로 받는다.
        setTabConnection(current, connectionId);
        syncConnectionComboToCurrentTab();
        current->editor()->setFocus();
        return;
    }
    addQueryTabFor(connectionId);
}

QueryTab *MainWindow::recentTabFor(const QString &connectionId) const
{
    for (const QPointer<QueryTab> &tab : m_tabHistory) {
        if (tab && m_tabs->indexOf(tab) >= 0 && tab->connectionId() == connectionId)
            return tab;
    }
    for (QueryTab *tab : queryTabs()) {
        if (tab->connectionId() == connectionId)
            return tab;
    }
    return nullptr;
}

void MainWindow::updateTabTitle(QueryTab *tab)
{
    // 제목은 연결 이름을 따른다 (같은 연결의 두 번째 탭부터 "이름 (2)").
    // 객체 조회 탭처럼 따로 정한 제목은 그대로 둔다.
    const int index = m_tabs->indexOf(tab);
    const QString source = tab->property(kTitleSource).toString();
    if (index < 0 || source == QLatin1String(kFixedTitle))
        return;

    const ConnectionProfile *profile = m_store.find(tab->connectionId());
    const QString title = m_tabs->tabText(index);
    if (profile) {
        static const QRegularExpression numbered(QStringLiteral("^ \\(\\d+\\)$"));
        const bool matchesName = title == profile->name
                                 || (title.startsWith(profile->name)
                                     && numbered.match(title.mid(profile->name.size())).hasMatch());
        if (source == profile->id && matchesName)
            return;
        m_tabs->setTabText(index, uniqueTabTitle(profile->name, tab));
        tab->setProperty(kTitleSource, profile->id);
    } else if (!source.isEmpty()) {
        m_tabs->setTabText(index, tr("Query %1").arg(++m_tabCounter));
        tab->setProperty(kTitleSource, QString());
    }
}

QString MainWindow::uniqueTabTitle(const QString &base, const QueryTab *except) const
{
    QStringList used;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->widget(i) != except)
            used << m_tabs->tabText(i);
    }
    QString title = base;
    for (int n = 2; used.contains(title); ++n)
        title = QStringLiteral("%1 (%2)").arg(base).arg(n);
    return title;
}

void MainWindow::setTabConnection(QueryTab *tab, const QString &connectionId)
{
    const ConnectionProfile *profile = m_store.find(connectionId);
    const QString id = profile ? connectionId : QString();

    // 다른 연결로 바뀌면 이전 세션은 끊는다 (다음 실행 때 새로 연결).
    if (tab->session()->isConnected() && tab->session()->profileId() != id)
        tab->session()->disconnectFromServer();

    tab->setConnectionId(id);
    tab->setConnectionProfile(profile);
    tab->setDialect(profile ? sqlDialectFor(profile->type) : SqlDialect::Generic);

    const int index = m_tabs->indexOf(tab);
    if (index >= 0) {
        m_tabs->setTabToolTip(index, profile ? QStringLiteral("%1 — %2").arg(profile->name, Db::typeName(profile->type))
                                             : tr("No connection"));
        const QColor color = profile ? Env::color(profile->environment) : QColor();
        m_tabs->setTabIcon(index, color.isValid() ? colorSwatchIcon(color, 10) : QIcon());
    }
    updateTabTitle(tab);
    updateFileTabTitle(tab); // 파일 탭은 도구 설명에 경로를 함께 둔다
    scheduleWorkspaceSave();

    if (tab == currentQueryTab()) {
        updateConnectionStatus();
        updateActionStates();
    }
}

void MainWindow::syncConnectionComboToCurrentTab()
{
    const QueryTab *tab = currentQueryTab();
    const int index = tab && !tab->connectionId().isEmpty() ? m_connectionCombo->findData(tab->connectionId()) : -1;

    m_syncingCombo = true;
    m_connectionCombo->setCurrentIndex(index);
    m_syncingCombo = false;
    updateConnectionStatus();
}

void MainWindow::updateConnectionStatus()
{
    QueryTab *tab = currentQueryTab();
    const ConnectionProfile *profile = tab ? m_store.find(tab->connectionId()) : nullptr;
    if (!profile) {
        m_connectionLabel->setText(tr("No connection"));
        return;
    }

    const QuerySession *session = tab->session();
    QString state;
    if (session->isConnecting())
        state = tr("connecting...");
    else if (session->isConnected())
        state = session->isBusy() ? tr("running...") : tr("connected");
    else
        state = tr("not connected");

    QString text = tr("%1: %2 (%3)").arg(Db::typeName(profile->type), profile->name, state);
    if (profile->environment == Environment::Production)
        text += tr(" · PRODUCTION");
    if (profile->readOnly)
        text += tr(" · read-only");
    if (session->hasPendingTransaction())
        text += tr(" · uncommitted changes");
    m_connectionLabel->setText(text);
}

void MainWindow::updateActionStates()
{
    const QueryTab *tab = currentQueryTab();
    const QuerySession *session = tab ? tab->session() : nullptr;
    const bool busy = session && (session->isBusy() || session->isConnecting());
    const bool connected = session && session->isConnected();
    const bool manual = !m_autoCommitAction->isChecked();

    m_executeAction->setEnabled(tab && !busy);
    m_executeScriptAction->setEnabled(tab && !busy);
    m_explainAction->setEnabled(tab && !busy);
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
