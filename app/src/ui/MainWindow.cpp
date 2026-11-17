#include "MainWindow.h"

#include "ObjectExplorer.h"
#include "QueryTab.h"
#include "ResultPanel.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolBar>

namespace {
const auto kGeometryKey = QStringLiteral("mainWindow/geometry");
const auto kStateKey = QStringLiteral("mainWindow/state");
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("DBaro"));
    resize(1280, 800);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    setCentralWidget(m_tabs);

    createActions();
    createDocks();
    createMenus();
    createToolBar();
    createStatusBar();

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeQueryTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (QueryTab *tab = currentQueryTab()) {
            const QTextCursor cursor = tab->editor()->textCursor();
            updateCursorPosition(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
        }
    });

    addQueryTab();
    restoreLayout();
}

void MainWindow::createActions()
{
    m_newTabAction = new QAction(tr("New Query Tab"), this);
    m_newTabAction->setShortcut(QKeySequence::AddTab);
    connect(m_newTabAction, &QAction::triggered, this, [this] { addQueryTab(); });

    m_closeTabAction = new QAction(tr("Close Tab"), this);
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this, [this] { closeQueryTab(m_tabs->currentIndex()); });

    m_quitAction = new QAction(tr("Exit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    // 연결 관리 화면은 다음 단계에서 구현한다.
    m_newConnectionAction = new QAction(tr("New Connection..."), this);
    m_newConnectionAction->setEnabled(false);

    m_executeAction = new QAction(tr("Execute"), this);
    m_executeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    m_executeAction->setToolTip(tr("Execute the statement at the cursor (Ctrl+Enter)"));
    connect(m_executeAction, &QAction::triggered, this, [this] { executeCurrent(false); });

    m_executeScriptAction = new QAction(tr("Execute Script"), this);
    m_executeScriptAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_executeScriptAction->setToolTip(tr("Execute the whole script (F5)"));
    connect(m_executeScriptAction, &QAction::triggered, this, [this] { executeCurrent(true); });

    // 연결과 실행이 생기기 전까지는 비활성.
    m_cancelAction = new QAction(tr("Cancel"), this);
    m_cancelAction->setEnabled(false);
    m_commitAction = new QAction(tr("Commit"), this);
    m_commitAction->setEnabled(false);
    m_rollbackAction = new QAction(tr("Rollback"), this);
    m_rollbackAction->setEnabled(false);

    m_autoCommitAction = new QAction(tr("Auto-commit"), this);
    m_autoCommitAction->setCheckable(true);
    m_autoCommitAction->setChecked(false);
    connect(m_autoCommitAction, &QAction::toggled, this, [this] { updateTransactionLabel(); });

    m_aboutAction = new QAction(tr("About DBaro"), this);
    connect(m_aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About DBaro"),
                           tr("DBaro %1\nCross-platform database tool.").arg(QApplication::applicationVersion()));
    });
}

void MainWindow::createDocks()
{
    m_explorer = new ObjectExplorer(this);

    m_explorerDock = new QDockWidget(tr("Database Explorer"), this);
    m_explorerDock->setObjectName(QStringLiteral("explorerDock"));
    m_explorerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_explorerDock->setWidget(m_explorer);
    addDockWidget(Qt::LeftDockWidgetArea, m_explorerDock);
    resizeDocks({m_explorerDock}, {280}, Qt::Horizontal);

    QAction *toggle = m_explorerDock->toggleViewAction();
    toggle->setShortcut(QKeySequence(Qt::ALT | Qt::Key_1));
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

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createToolBar()
{
    QToolBar *toolBar = addToolBar(tr("Main"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setMovable(false);

    m_connectionCombo = new QComboBox(toolBar);
    m_connectionCombo->setMinimumContentsLength(24);
    m_connectionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_connectionCombo->addItem(tr("No connection"));
    m_connectionCombo->setToolTip(tr("Connection for the current tab"));
    m_connectionCombo->setEnabled(false);
    toolBar->addWidget(m_connectionCombo);

    toolBar->addSeparator();
    toolBar->addAction(m_executeAction);
    toolBar->addAction(m_executeScriptAction);
    toolBar->addAction(m_cancelAction);
    toolBar->addSeparator();
    toolBar->addAction(m_commitAction);
    toolBar->addAction(m_rollbackAction);
    toolBar->addAction(m_autoCommitAction);
}

void MainWindow::createStatusBar()
{
    m_connectionLabel = new QLabel(tr("Not connected"), this);
    m_transactionLabel = new QLabel(this);
    m_cursorLabel = new QLabel(this);
    m_encodingLabel = new QLabel(QStringLiteral("UTF-8"), this);

    for (QLabel *label : {m_connectionLabel, m_transactionLabel, m_cursorLabel, m_encodingLabel}) {
        label->setContentsMargins(8, 0, 8, 0);
        statusBar()->addPermanentWidget(label);
    }

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
    auto *tab = new QueryTab(m_tabs);
    const int index = m_tabs->addTab(tab, tr("Query %1").arg(++m_tabCounter));
    m_tabs->setCurrentIndex(index);

    connect(tab, &QueryTab::cursorPositionChanged, this, [this, tab](int line, int column) {
        if (tab == currentQueryTab())
            updateCursorPosition(line, column);
    });

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

    // 엔진 연결이 생기기 전까지는 로그에 안내만 남긴다.
    const QString what = wholeScript ? tr("Execute script") : tr("Execute statement");
    tab->resultPanel()->appendLog(tr("%1: not connected (execution is not implemented yet)").arg(what));
    tab->resultPanel()->showLog();
}

void MainWindow::updateCursorPosition(int line, int column)
{
    m_cursorLabel->setText(tr("Ln %1, Col %2").arg(line).arg(column));
}

void MainWindow::updateTransactionLabel()
{
    m_transactionLabel->setText(m_autoCommitAction->isChecked() ? tr("Auto-commit") : tr("Manual commit"));
}
