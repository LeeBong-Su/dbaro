#include "MainWindow.h"

#include "ConnectionDialog.h"
#include "EditorTabs.h"
#include "Icons.h"
#include "ObjectExplorer.h"
#include "QueryTab.h"
#include "ResultPanel.h"
#include "SqlEditor.h"
#include "Theme.h"
#include "UiHelpers.h"
#include "core/SecretStore.h"
#include "core/SqlDialect.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolBar>
#include <QToolButton>

namespace {
const auto kGeometryKey = QStringLiteral("mainWindow/geometry");
const auto kStateKey = QStringLiteral("mainWindow/state");

QString withShortcut(const QString &text, const QKeySequence &shortcut)
{
    return QStringLiteral("%1 (%2)").arg(text, shortcut.toString(QKeySequence::NativeText));
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

    m_tabs = new EditorTabs(this);
    setCentralWidget(m_tabs);

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
        }
        syncConnectionComboToCurrentTab();
    });

    addQueryTab();
    loadConnections();
    restoreLayout();
}

void MainWindow::createActions()
{
    m_newTabAction = new QAction(Icons::get(QStringLiteral("add")), tr("New Query Tab"), this);
    m_newTabAction->setShortcut(QKeySequence::AddTab);
    m_newTabAction->setToolTip(withShortcut(tr("New query tab"), m_newTabAction->shortcut()));
    connect(m_newTabAction, &QAction::triggered, this, [this] { addQueryTab(); });

    m_closeTabAction = new QAction(tr("Close Tab"), this);
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this, [this] { closeQueryTab(m_tabs->currentIndex()); });

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

    // 연결과 실행이 생기기 전까지는 비활성.
    m_cancelAction = new QAction(Icons::get(QStringLiteral("stop")), tr("Cancel"), this);
    m_cancelAction->setToolTip(tr("Cancel the running statement"));
    m_cancelAction->setEnabled(false);
    m_commitAction = new QAction(Icons::get(QStringLiteral("commit")), tr("Commit"), this);
    m_commitAction->setEnabled(false);
    m_rollbackAction = new QAction(Icons::get(QStringLiteral("rollback")), tr("Rollback"), this);
    m_rollbackAction->setEnabled(false);

    m_autoCommitAction = new QAction(tr("Auto-commit"), this);
    m_autoCommitAction->setCheckable(true);
    m_autoCommitAction->setChecked(false);
    connect(m_autoCommitAction, &QAction::toggled, this, [this] { updateTransactionLabel(); });

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
    m_explorer = new ObjectExplorer(this);
    connect(m_explorer, &ObjectExplorer::connectionActivated, this, [this](const QString &id) {
        if (QueryTab *tab = currentQueryTab()) {
            setTabConnection(tab, id);
            syncConnectionComboToCurrentTab();
        }
    });

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
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);

    QMenu *databaseMenu = menuBar()->addMenu(tr("&Database"));
    databaseMenu->addAction(m_newConnectionAction);
    databaseMenu->addAction(m_manageConnectionsAction);

    QMenu *queryMenu = menuBar()->addMenu(tr("&Query"));
    queryMenu->addAction(m_executeAction);
    queryMenu->addAction(m_executeScriptAction);
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
    m_connectionCombo->setToolTip(tr("Connection for the current tab"));
    connect(m_connectionCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_syncingCombo)
            return;
        if (QueryTab *tab = currentQueryTab())
            setTabConnection(tab, m_connectionCombo->currentData().toString());
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
    // TODO: 저장 안 된 SQL이나 커밋 안 된 트랜잭션이 있으면 닫기 전에 확인한다.
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kStateKey, saveState());
    QMainWindow::closeEvent(event);
}

QueryTab *MainWindow::currentQueryTab() const
{
    return qobject_cast<QueryTab *>(m_tabs->currentWidget());
}

QueryTab *MainWindow::addQueryTab()
{
    // 새 탭은 지금 탭의 연결을 그대로 쓴다.
    const QString inheritedConnection = currentQueryTab() ? currentQueryTab()->connectionId() : QString();

    auto *tab = new QueryTab(m_tabs);
    connect(tab, &QueryTab::cursorPositionChanged, this, [this, tab](int line, int column) {
        if (tab == currentQueryTab())
            updateCursorPosition(line, column);
    });

    const int index = m_tabs->addTab(tab, tr("Query %1").arg(++m_tabCounter));
    setTabConnection(tab, inheritedConnection);
    m_tabs->setCurrentIndex(index);
    tab->editor()->setFocus();
    return tab;
}

void MainWindow::closeQueryTab(int index)
{
    QWidget *widget = m_tabs->widget(index);
    if (!widget)
        return;
    // TODO: 저장 안 된 SQL이나 커밋 안 된 트랜잭션이 있으면 닫기 전에 확인한다.
    m_tabs->removeTab(index);
    widget->deleteLater();
}

void MainWindow::executeCurrent(bool wholeScript)
{
    QueryTab *tab = currentQueryTab();
    if (!tab)
        return;

    ResultPanel *results = tab->resultPanel();
    const QList<SqlStatement> statements = tab->editor()->statementsToExecute(wholeScript);
    if (statements.isEmpty()) {
        results->appendLog(tr("Nothing to execute."));
        results->showLog();
        return;
    }

    // 엔진 연결이 생기기 전까지는 실행할 문장만 로그에 보여 준다.
    const ConnectionProfile *profile = m_store.find(tab->connectionId());
    const QString target = profile ? profile->name : tr("no connection");
    results->appendLog(tr("%n statement(s) ready for %1 (execution is not implemented yet)", nullptr,
                          int(statements.size()))
                           .arg(target));
    for (int k = 0; k < statements.size(); ++k) {
        QString preview = statements[k].text.simplified();
        if (preview.size() > 160)
            preview = preview.left(157) + QStringLiteral("...");
        results->appendLog(QStringLiteral("  [%1] %2").arg(k + 1).arg(preview));
    }
    results->showLog();
}

void MainWindow::openConnectionDialog(bool startNew)
{
    ConnectionDialog dialog(m_store.profiles(), this);
    if (startNew) {
        dialog.startNewConnection();
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
    m_connectionCombo->clear();
    m_connectionCombo->addItem(tr("No connection"), QString());
    for (const ConnectionProfile &profile : m_store.profiles())
        m_connectionCombo->addItem(colorSwatchIcon(Env::color(profile.environment)), profile.name, profile.id);
    m_syncingCombo = false;

    m_explorer->setConnections(m_store.profiles());

    // 지워진 연결을 쓰던 탭은 연결을 해제하고, 나머지는 색상 등 바뀐 설정을 다시 반영한다.
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *tab = qobject_cast<QueryTab *>(m_tabs->widget(i)))
            setTabConnection(tab, m_store.find(tab->connectionId()) ? tab->connectionId() : QString());
    }
    syncConnectionComboToCurrentTab();
}

void MainWindow::setTabConnection(QueryTab *tab, const QString &connectionId)
{
    const ConnectionProfile *profile = m_store.find(connectionId);
    tab->setConnectionId(profile ? connectionId : QString());
    tab->setConnectionColor(profile ? Env::color(profile->environment) : QColor());
    tab->setDialect(profile ? sqlDialectFor(profile->type) : SqlDialect::Generic);

    const int index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabToolTip(index, profile ? profile->name : QString());

    if (tab == currentQueryTab())
        updateConnectionStatus();
}

void MainWindow::syncConnectionComboToCurrentTab()
{
    const QueryTab *tab = currentQueryTab();
    const int index = tab ? m_connectionCombo->findData(tab->connectionId()) : 0;

    m_syncingCombo = true;
    m_connectionCombo->setCurrentIndex(index < 0 ? 0 : index);
    m_syncingCombo = false;
    updateConnectionStatus();
}

void MainWindow::updateConnectionStatus()
{
    const QueryTab *tab = currentQueryTab();
    const ConnectionProfile *profile = tab ? m_store.find(tab->connectionId()) : nullptr;
    if (!profile) {
        m_connectionLabel->setText(tr("No connection"));
        return;
    }

    QString text = tr("%1: %2 (not connected)").arg(Db::typeName(profile->type), profile->name);
    if (profile->environment == Environment::Production)
        text += tr(" · PRODUCTION");
    if (profile->readOnly)
        text += tr(" · read-only");
    m_connectionLabel->setText(text);
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
