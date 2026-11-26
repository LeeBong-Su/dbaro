#include "MonitorTab.h"

#include "QuerySession.h"
#include "SqlHighlighter.h"
#include "Theme.h"
#include "core/SqlDialect.h"

#include <QApplication>
#include <QCborArray>
#include <QCheckBox>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kSortRole = Qt::UserRole + 1;     // 정렬 값 (숫자 컬럼)
constexpr int kIdRole = Qt::UserRole + 2;       // 세션 id
constexpr int kFractionRole = Qt::UserRole + 3; // 사용률 0..1 (막대)
constexpr int kBlockerRole = Qt::UserRole + 4;  // 다른 세션을 막고 있음
const auto kAutoRefreshKey = QStringLiteral("monitor/autoRefreshSeconds");

enum SessionColumn { SId, SUser, SStatus, STime, SWait, SBlockedBy, SDatabase, SClient, SProgram, SSql, SColumnCount };

// 숫자 컬럼은 정렬 값으로, 글자는 대소문자 무시·숫자 크기대로 정렬한다.
class SortItem : public QTableWidgetItem
{
public:
    explicit SortItem(const QString &text = {}, const QVariant &key = {})
        : QTableWidgetItem(text)
    {
        if (key.isValid())
            setData(kSortRole, key);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant a = data(kSortRole);
        const QVariant b = other.data(kSortRole);
        if (a.isValid() || b.isValid())
            return a.toDouble() < b.toDouble(); // 값을 모르면 0 (맨 앞)
        static const QCollator collator = [] {
            QCollator c;
            c.setCaseSensitivity(Qt::CaseInsensitive);
            c.setNumericMode(true);
            return c;
        }();
        return collator.compare(text(), other.text()) < 0;
    }
};

// 사용률 컬럼: 칸 안에 막대를 그리고 그 위에 글자를 쓴다 (80% 넘으면 주황, 90% 넘으면 빨강).
class UsageDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QVariant fraction = index.data(kFractionRole);
        if (fraction.isValid()) {
            const double f = qBound(0.0, fraction.toDouble(), 1.0);
            const QRect bar = opt.rect.adjusted(4, 5, -4, -5);
            QColor fill = f >= 0.9 ? QColor(214, 64, 64) : f >= 0.8 ? QColor(214, 140, 32) : QColor(54, 160, 84);
            QColor track = opt.palette.color(QPalette::Mid);
            track.setAlpha(70);
            fill.setAlpha(170);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(track);
            painter->drawRoundedRect(bar, 3, 3);
            QRect used = bar;
            used.setWidth(int(bar.width() * f));
            painter->setBrush(fill);
            painter->drawRoundedRect(used, 3, 3);
            painter->restore();
        }
        painter->setPen(opt.palette.color(opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(opt.rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
};

QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, int(headers.size()), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionsMovable(true);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 8);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    table->setSortingEnabled(true);
    return table;
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("monitorSectionTitle"));
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QString duration(qint64 seconds)
{
    if (seconds < 0)
        return {};
    if (seconds < 60)
        return MonitorTab::tr("%1 s").arg(seconds);
    if (seconds < 3600)
        return MonitorTab::tr("%1 m %2 s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (seconds < 86400)
        return MonitorTab::tr("%1 h %2 m").arg(seconds / 3600).arg((seconds % 3600) / 60, 2, 10, QLatin1Char('0'));
    return MonitorTab::tr("%1 d %2 h").arg(seconds / 86400).arg((seconds % 86400) / 3600, 2, 10, QLatin1Char('0'));
}

// 화면이 영어라 크기·개수도 영어 형식으로 (시스템 로캘이면 "0 바이트"처럼 섞인다)
const QLocale &numbers()
{
    static const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale;
}

QString bytesText(const QCborValue &value)
{
    if (!value.isInteger() && !value.isDouble())
        return {};
    return numbers().formattedDataSize(value.toInteger(), 1, QLocale::DataSizeTraditionalFormat);
}

SortItem *bytesItem(const QCborValue &value)
{
    auto *item = new SortItem(bytesText(value), value.isInteger() || value.isDouble() ? QVariant(double(value.toInteger())) : QVariant(-1.0));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (value.isInteger())
        item->setToolTip(numbers().toString(value.toInteger()) + MonitorTab::tr(" bytes"));
    return item;
}

SortItem *secondsItem(const QCborValue &value)
{
    const bool known = value.isInteger() || value.isDouble();
    auto *item = new SortItem(known ? duration(value.toInteger()) : QString(), known ? QVariant(double(value.toInteger())) : QVariant(-1.0));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

SortItem *countItem(const QCborValue &value)
{
    const bool known = value.isInteger() || value.isDouble();
    auto *item = new SortItem(known ? numbers().toString(value.toInteger()) : QString(),
                              known ? QVariant(double(value.toInteger())) : QVariant(-1.0));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QString oneLine(const QString &sql, int max = 300)
{
    QString text = sql.simplified();
    if (text.size() > max)
        text = text.left(max) + QStringLiteral("…");
    return text;
}

QStringList strings(const QCborValue &value)
{
    QStringList list;
    for (const QCborValue &item : value.toArray())
        list << item.toString();
    return list;
}

QColor blockedTint() { return QColor(230, 140, 20, 60); }
QColor blockerTint() { return QColor(220, 60, 60, 75); }

} // namespace

MonitorTab::MonitorTab(EngineClient *engine, const ConnectionProfile &profile, QWidget *parent)
    : QWidget(parent)
    , m_profile(profile)
{
    setObjectName(QStringLiteral("monitorTab"));
    m_session = new QuerySession(engine, this);
    connect(m_session, &QuerySession::message, this, [this](const QString &line, bool isError) {
        if (isError)
            setStatus(line, true);
    });

    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setToolTip(tr("Read the page again (F5)"));
    m_refresh->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_refresh, &QPushButton::clicked, this, &MonitorTab::refresh);

    m_autoRefresh = new QComboBox(this);
    m_autoRefresh->setToolTip(tr("Read sessions and locks again at this interval while the tab is visible"));
    for (int seconds : {0, 2, 5, 10, 30, 60})
        m_autoRefresh->addItem(seconds == 0 ? tr("Off") : tr("Every %1 s").arg(seconds), seconds);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("monitorStatus"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *header = new QHBoxLayout;
    header->addWidget(m_refresh);
    header->addSpacing(8);
    header->addWidget(new QLabel(tr("Auto refresh:"), this));
    header->addWidget(m_autoRefresh);
    header->addSpacing(12);
    header->addWidget(m_status, 1);

    m_pages = new QTabWidget(this);
    m_pages->setDocumentMode(true);
    m_pages->addTab(buildSessionsPage(), tr("Sessions"));
    m_pages->addTab(buildLocksPage(), tr("Locks"));
    m_pages->addTab(buildSpacePage(), tr("Space"));
    connect(m_pages, &QTabWidget::currentChanged, this, [this] {
        if (m_started && (page() != Page::Space || !m_spaceLoaded))
            refresh(); // 세션·락은 고를 때마다 새로, 용량은 처음 볼 때만
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 0);
    layout->addLayout(header);
    layout->addWidget(m_pages, 1);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && page() != Page::Space)
            refresh();
    });
    connect(m_autoRefresh, &QComboBox::activated, this, [this] {
        const int seconds = m_autoRefresh->currentData().toInt();
        QSettings().setValue(kAutoRefreshKey, seconds);
        setAutoRefresh(seconds);
    });
    const int saved = QSettings().value(kAutoRefreshKey, 5).toInt();
    m_autoRefresh->setCurrentIndex(qMax(0, m_autoRefresh->findData(saved)));
    setAutoRefresh(m_autoRefresh->currentData().toInt());
    updateButtons();
}

MonitorTab::~MonitorTab() = default; // 세션(QuerySession)이 자식이라 함께 연결을 끊는다

QWidget *MonitorTab::buildSessionsPage()
{
    auto *page = new QWidget(this);
    m_filter = new QLineEdit(page);
    m_filter->setPlaceholderText(tr("Filter: user, program, SQL, wait ..."));
    m_filter->setClearButtonEnabled(true);
    m_activeOnly = new QCheckBox(tr("Active only"), page);
    m_activeOnly->setToolTip(tr("Hide idle sessions (blocked and blocking sessions stay)"));
    m_showBackground = new QCheckBox(tr("Background"), page);
    m_showBackground->setToolTip(tr("Show the database's own background processes"));
    m_sessionCount = new QLabel(page);
    connect(m_filter, &QLineEdit::textChanged, this, &MonitorTab::applySessionFilter);
    connect(m_activeOnly, &QCheckBox::toggled, this, &MonitorTab::applySessionFilter);
    connect(m_showBackground, &QCheckBox::toggled, this, &MonitorTab::applySessionFilter);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 8, 0, 4);
    bar->addWidget(m_filter, 1);
    bar->addWidget(m_activeOnly);
    bar->addWidget(m_showBackground);
    bar->addSpacing(8);
    bar->addWidget(m_sessionCount);

    m_sessionNotes = new QLabel(page);
    m_sessionNotes->setWordWrap(true);
    m_sessionNotes->hide();

    const bool oracleLike = m_profile.type == DbType::Oracle || m_profile.type == DbType::Tibero;
    m_sessions = makeTable({tr("ID"), tr("User"), tr("Status"), tr("Time"), tr("Wait / State"), tr("Blocked by"),
                            oracleLike ? tr("Schema") : tr("Database"), tr("Client"), tr("Program"), tr("SQL")},
                           page);
    m_sessions->setObjectName(QStringLiteral("monitorSessions"));
    m_sessions->horizontalHeaderItem(STime)->setToolTip(tr("Time in the current state: running for active sessions, idle for the others"));
    m_sessions->horizontalHeaderItem(SBlockedBy)->setToolTip(tr("Sessions holding the lock this session waits for"));
    const int widths[] = {90, 90, 110, 80, 190, 90, 90, 130, 150};
    for (int column = 0; column < SSql; ++column)
        m_sessions->setColumnWidth(column, widths[column]);
    m_sessions->sortByColumn(STime, Qt::DescendingOrder);
    m_sessions->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessions, &QTableWidget::itemSelectionChanged, this, [this] {
        updateSessionDetails();
        updateButtons();
    });

    m_sessionSql = new QPlainTextEdit(page);
    m_sessionSql->setReadOnly(true);
    m_sessionSql->setFrameShape(QFrame::NoFrame);
    m_sessionSql->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_sessionSql->setPlaceholderText(tr("Select a session to see its SQL."));
    auto *highlighter = new SqlHighlighter(m_sessionSql->document());
    highlighter->setDialect(sqlDialectFor(m_profile.type));
    highlighter->setDarkTheme(Theme::current() == Theme::Kind::Dark);

    m_kill = new QPushButton(tr("Kill Session..."), page);
    m_cancelQuery = new QPushButton(tr("Cancel Statement..."), page);
    m_cancelQuery->setToolTip(tr("Stop only the running statement; the session stays connected"));
    m_openSql = new QPushButton(tr("Open SQL in Query Tab"), page);
    connect(m_kill, &QPushButton::clicked, this, [this] { kill(selectedSessionId(), false); });
    connect(m_cancelQuery, &QPushButton::clicked, this, [this] { kill(selectedSessionId(), true); });
    connect(m_openSql, &QPushButton::clicked, this, [this] {
        const QString sql = m_sessionData.value(selectedSessionId()).value(QStringLiteral("sql")).toString();
        if (!sql.isEmpty())
            emit openSqlRequested(sql);
    });
    connect(m_sessions, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_sessions->itemAt(pos))
            return;
        const QString id = selectedSessionId();
        const QString sql = m_sessionData.value(id).value(QStringLiteral("sql")).toString();
        QMenu menu(this);
        menu.addAction(m_kill->text(), this, [this, id] { kill(id, false); })->setEnabled(m_kill->isEnabled());
        menu.addAction(m_cancelQuery->text(), this, [this, id] { kill(id, true); })->setEnabled(m_cancelQuery->isEnabled());
        menu.addSeparator();
        menu.addAction(m_openSql->text(), this, [this, sql] { emit openSqlRequested(sql); })->setEnabled(!sql.isEmpty());
        menu.addAction(tr("Copy SQL"), this, [sql] { QApplication::clipboard()->setText(sql); })->setEnabled(!sql.isEmpty());
        menu.addAction(tr("Copy Session ID"), this, [id] { QApplication::clipboard()->setText(id); });
        menu.exec(m_sessions->viewport()->mapToGlobal(pos));
    });

    auto *detail = new QWidget(page);
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 4, 0, 0);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_kill);
    buttons->addWidget(m_cancelQuery);
    buttons->addStretch();
    buttons->addWidget(m_openSql);
    detailLayout->addLayout(buttons);
    detailLayout->addWidget(m_sessionSql, 1);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->addWidget(m_sessions);
    splitter->addWidget(detail);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(bar);
    layout->addWidget(m_sessionNotes);
    layout->addWidget(splitter, 1);
    return page;
}

QWidget *MonitorTab::buildLocksPage()
{
    auto *page = new QWidget(this);
    m_locksSummary = new QLabel(page);
    m_locksSummary->setWordWrap(true);
    m_locksError = new QLabel(page);
    m_locksError->setWordWrap(true);
    m_locksError->hide();
    m_killLocker = new QPushButton(tr("Kill Session..."), page);
    m_killLocker->setToolTip(tr("Kill the selected session (pick the blocking session at the top of a group)"));
    auto *showInSessions = new QPushButton(tr("Show in Sessions"), page);
    connect(m_killLocker, &QPushButton::clicked, this, [this] { kill(lockSelectionId(), false); });
    connect(showInSessions, &QPushButton::clicked, this, [this] {
        const QString id = lockSelectionId();
        if (id.isEmpty())
            return;
        showPage(Page::Sessions);
        selectSession(id);
    });

    m_waits = new QTreeWidget(page);
    m_waits->setObjectName(QStringLiteral("monitorWaits"));
    m_waits->setHeaderLabels({tr("Session"), tr("User"), tr("State"), tr("Time"), tr("Lock"), tr("Object"), tr("SQL")});
    m_waits->setAlternatingRowColors(true);
    m_waits->setUniformRowHeights(true);
    m_waits->setFrameShape(QFrame::NoFrame);
    m_waits->headerItem()->setToolTip(3, tr("Blocking session: time in its transaction (or state). Waiting session: time waiting."));
    const int waitWidths[] = {140, 90, 190, 80, 170, 150};
    for (int column = 0; column < 6; ++column)
        m_waits->setColumnWidth(column, waitWidths[column]);

    m_held = makeTable({tr("Session"), tr("User"), tr("Object"), tr("Mode"), tr("Status"), tr("Time"), tr("Detail")}, page);
    m_held->setObjectName(QStringLiteral("monitorHeld"));
    const int heldWidths[] = {100, 90, 200, 150, 140, 80};
    for (int column = 0; column < 6; ++column)
        m_held->setColumnWidth(column, heldWidths[column]);

    connect(m_waits, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!m_waits->selectedItems().isEmpty())
            m_held->clearSelection();
        updateButtons();
    });
    connect(m_held, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!m_held->selectedItems().isEmpty())
            m_waits->clearSelection();
        updateButtons();
    });

    auto *waitsBox = new QWidget(page);
    auto *waitsLayout = new QVBoxLayout(waitsBox);
    waitsLayout->setContentsMargins(0, 0, 0, 0);
    auto *waitsHeader = new QHBoxLayout;
    waitsHeader->addWidget(sectionTitle(tr("Lock waits (blocking session → waiting sessions)"), waitsBox), 1);
    waitsHeader->addWidget(showInSessions);
    waitsHeader->addWidget(m_killLocker);
    waitsLayout->addLayout(waitsHeader);
    waitsLayout->addWidget(m_locksSummary);
    waitsLayout->addWidget(m_waits, 1);

    auto *heldBox = new QWidget(page);
    auto *heldLayout = new QVBoxLayout(heldBox);
    heldLayout->setContentsMargins(0, 6, 0, 0);
    QString heldTitle = tr("Locks held");
    if (m_profile.type == DbType::PostgreSQL)
        heldTitle = tr("Table locks held (stronger than AccessShareLock, this database)");
    else if (m_profile.type == DbType::MariaDB)
        heldTitle = tr("Transactions holding locks");
    else if (m_profile.type == DbType::MySQL)
        heldTitle = tr("Table locks held (InnoDB)");
    else
        heldTitle = tr("Locked objects");
    heldLayout->addWidget(sectionTitle(heldTitle, heldBox));
    heldLayout->addWidget(m_held, 1);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->addWidget(waitsBox);
    splitter->addWidget(heldBox);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(m_locksError);
    layout->addWidget(splitter, 1);
    return page;
}

QWidget *MonitorTab::buildSpacePage()
{
    auto *page = new QWidget(this);
    m_spaceSummary = new QLabel(page);
    m_spaceSummary->setWordWrap(true);
    m_spaceSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_tablespacesTitle = sectionTitle(m_profile.type == DbType::MySQL ? tr("InnoDB tablespaces") : tr("Tablespaces"), page);
    m_tablespaces = makeTable({tr("Name"), tr("Type"), tr("Status"), tr("Used %"), tr("Used"), tr("Allocated"), tr("Max"),
                               tr("Free"), tr("Auto-extend"), tr("Files"), tr("Location")},
                              page);
    m_tablespaces->setObjectName(QStringLiteral("monitorTablespaces"));
    m_tablespaces->setItemDelegateForColumn(3, new UsageDelegate(m_tablespaces));
    m_tablespaces->horizontalHeaderItem(3)->setToolTip(
        tr("Used space of the allocated size. With auto-extend, the tooltip also shows the share of the maximum size."));
    const int tablespaceWidths[] = {150, 100, 80, 140, 90, 90, 90, 90, 90, 60};
    for (int column = 0; column < 10; ++column)
        m_tablespaces->setColumnWidth(column, tablespaceWidths[column]);
    m_tablespaces->sortByColumn(0, Qt::AscendingOrder);

    m_databasesTitle = sectionTitle(m_profile.type == DbType::PostgreSQL ? tr("Databases") : tr("Schemas (databases)"), page);
    m_databases = makeTable({tr("Name"), tr("Size"), tr("Data"), tr("Index"), tr("Free"), tr("Tables")}, page);
    m_databases->setObjectName(QStringLiteral("monitorDatabases"));
    for (int column = 0; column < 5; ++column)
        m_databases->setColumnWidth(column, column == 0 ? 200 : 100);
    m_databases->sortByColumn(1, Qt::DescendingOrder);

    const bool oracleLike = m_profile.type == DbType::Oracle || m_profile.type == DbType::Tibero;
    m_objectsTitle = sectionTitle(oracleLike ? tr("Largest segments") : tr("Largest tables"), page);
    m_objects = makeTable({tr("Schema"), tr("Name"), tr("Type"), tr("Size"), tr("Data"), tr("Index"), tr("Rows (est.)"),
                           tr("Tablespace"), tr("Partitions")},
                          page);
    m_objects->setObjectName(QStringLiteral("monitorObjects"));
    const int objectWidths[] = {120, 220, 140, 90, 90, 90, 100, 110};
    for (int column = 0; column < 8; ++column)
        m_objects->setColumnWidth(column, objectWidths[column]);
    m_objects->sortByColumn(3, Qt::DescendingOrder);

    auto section = [page](QLabel *title, QTableWidget *table) {
        auto *box = new QWidget(page);
        auto *layout = new QVBoxLayout(box);
        layout->setContentsMargins(0, 6, 0, 0);
        layout->addWidget(title);
        layout->addWidget(table, 1);
        return box;
    };
    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->addWidget(section(m_tablespacesTitle, m_tablespaces));
    splitter->addWidget(section(m_databasesTitle, m_databases));
    splitter->addWidget(section(m_objectsTitle, m_objects));

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(m_spaceSummary);
    layout->addWidget(splitter, 1);
    return page;
}

void MonitorTab::start(const ConnectSpec &spec)
{
    m_spec = spec;
    m_started = true;
    refresh();
}

void MonitorTab::showPage(Page page)
{
    m_pages->setCurrentIndex(int(page)); // 바뀌면 currentChanged가 읽는다
}

MonitorTab::Page MonitorTab::page() const
{
    return Page(qBound(0, m_pages->currentIndex(), 2));
}

void MonitorTab::refresh()
{
    if (m_started)
        load(page());
}

void MonitorTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 다른 탭을 보다 돌아오면 바로 새로 읽는다 (자동 새로 고침이 켜져 있을 때)
    if (m_started && m_timer->isActive() && page() != Page::Space)
        QTimer::singleShot(0, this, &MonitorTab::refresh);
}

void MonitorTab::setAutoRefresh(int seconds)
{
    if (seconds <= 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(seconds * 1000);
}

void MonitorTab::connectThen(std::function<void()> work, std::function<void()> failed)
{
    auto finish = [work, failed](bool ok) {
        if (ok)
            work();
        else if (failed)
            failed();
    };
    if (m_session->isConnected()) {
        work();
        return;
    }
    if (m_session->isConnecting()) {
        m_session->whenConnected(finish);
        return;
    }
    setStatus(tr("Connecting..."));
    QPointer<MonitorTab> self(this);
    m_session->connectTo(m_spec, m_profile.id, [self, finish](bool ok) {
        if (self)
            finish(ok);
    });
}

void MonitorTab::load(Page which)
{
    const int index = int(which);
    if (m_loading[index])
        return;
    m_loading[index] = true;
    QPointer<MonitorTab> self(this);
    // 접속하지 못하면 다음 Refresh 때 다시 해 볼 수 있게 읽는 중 표시를 푼다
    auto failed = [self, index] {
        if (self)
            self->m_loading[index] = false;
    };
    connectThen([self, which, index] {
        if (!self)
            return;
        static const char *methods[] = {"monitorSessions", "monitorLocks", "monitorSpace"};
        if (which == Page::Space)
            self->setStatus(tr("Reading space usage..."));
        self->m_session->call(QString::fromLatin1(methods[index]), {},
                              [self, which, index](const QCborMap &result, const EngineError &error) {
                                  if (!self)
                                      return;
                                  self->m_loading[index] = false;
                                  if (error.isError()) {
                                      self->showError(which, error);
                                      return;
                                  }
                                  switch (which) {
                                  case Page::Sessions: self->showSessions(result); break;
                                  case Page::Locks: self->showLocks(result); break;
                                  case Page::Space: self->showSpace(result); break;
                                  }
                              });
    }, failed);
}

void MonitorTab::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: %1;").arg(Theme::warningTextColor().name()) : QString());
    m_status->setToolTip(error ? text : QString());
}

QString MonitorTab::statusText() const
{
    return m_status->text();
}

void MonitorTab::showError(Page which, const EngineError &error)
{
    Q_UNUSED(which);
    setStatus(tr("Could not read: %1").arg(error.message.section(u'\n', 0, 0)), true);
    m_status->setToolTip(error.message);
}

// ---------------------------------------------------------------------------
// 세션

void MonitorTab::showSessions(const QCborMap &result)
{
    const QString keep = selectedSessionId();
    const QCborArray list = result.value(QStringLiteral("sessions")).toArray();

    // 다른 세션을 막고 있는 세션
    QSet<QString> blockers;
    for (const QCborValue &value : list) {
        for (const QString &id : strings(value.toMap().value(QStringLiteral("blockedBy"))))
            blockers.insert(id);
    }

    m_sessionData.clear();
    m_sessions->setSortingEnabled(false);
    m_sessions->setRowCount(0);
    m_sessions->setRowCount(int(list.size()));
    const QColor dim = palette().color(QPalette::PlaceholderText);
    int row = 0;
    for (const QCborValue &value : list) {
        const QCborMap s = value.toMap();
        const QString id = s.value(QStringLiteral("id")).toString();
        m_sessionData.insert(id, s);
        const QStringList blockedBy = strings(s.value(QStringLiteral("blockedBy")));
        const bool background = s.value(QStringLiteral("background")).toBool();
        const bool self = s.value(QStringLiteral("self")).toBool();
        const bool blocker = blockers.contains(id);

        // ID는 SID(Oracle) 또는 pid 숫자로 정렬한다
        auto *idItem = new SortItem(id, id.section(u',', 0, 0).toDouble());
        idItem->setData(kIdRole, id);
        idItem->setData(kBlockerRole, blocker);
        QString status = s.value(QStringLiteral("status")).toString();
        QString wait = s.value(QStringLiteral("wait")).toString();
        auto *sqlItem = new SortItem(oneLine(s.value(QStringLiteral("sql")).toString()));
        if (s.value(QStringLiteral("sqlIsPrevious")).toBool() && !sqlItem->text().isEmpty())
            sqlItem->setToolTip(tr("Last statement (the session is not running one now)"));

        QTableWidgetItem *items[SColumnCount] = {
            idItem,
            new SortItem(s.value(QStringLiteral("user")).toString()),
            new SortItem(status),
            secondsItem(s.value(QStringLiteral("seconds"))),
            new SortItem(wait),
            new SortItem(blockedBy.join(QStringLiteral(", "))),
            new SortItem(s.value(QStringLiteral("database")).toString()),
            new SortItem(s.value(QStringLiteral("client")).toString()),
            new SortItem(s.value(QStringLiteral("program")).toString()),
            sqlItem,
        };
        for (int column = 0; column < SColumnCount; ++column) {
            QTableWidgetItem *item = items[column];
            if (!blockedBy.isEmpty())
                item->setBackground(blockedTint());
            else if (blocker)
                item->setBackground(blockerTint());
            if (background || (s.value(QStringLiteral("idleWait")).toBool() && column == SWait))
                item->setForeground(dim);
            if (self) {
                QFont font = item->font();
                font.setItalic(true);
                item->setFont(font);
                item->setToolTip(tr("This monitor's own session"));
            }
            m_sessions->setItem(row, column, item);
        }
        if (blocker)
            idItem->setToolTip(tr("Blocking other sessions"));
        else if (!blockedBy.isEmpty())
            idItem->setToolTip(tr("Waiting for a lock held by %1").arg(blockedBy.join(QStringLiteral(", "))));
        ++row;
    }
    m_sessions->setSortingEnabled(true);
    applySessionFilter();
    if (!keep.isEmpty())
        selectSession(keep);
    updateSessionDetails();
    updateButtons();

    const QStringList notes = strings(result.value(QStringLiteral("notes")));
    m_sessionNotes->setText(notes.join(u'\n'));
    m_sessionNotes->setVisible(!notes.isEmpty());
    m_sessionNotes->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningTextColor().name()));

    const int waiting = int(std::count_if(m_sessionData.cbegin(), m_sessionData.cend(), [](const QCborMap &s) {
        return !s.value(QStringLiteral("blockedBy")).toArray().isEmpty();
    }));
    QString text = tr("Updated %1 · %n session(s)", nullptr, int(list.size()))
                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    if (waiting > 0)
        text += tr(" · %n waiting for a lock", nullptr, waiting);
    setStatus(text + tr(" · %1 ms").arg(result.value(QStringLiteral("elapsedMs")).toInteger()), waiting > 0);
}

void MonitorTab::applySessionFilter()
{
    const QStringList words = m_filter->text().split(u' ', Qt::SkipEmptyParts);
    const bool activeOnly = m_activeOnly->isChecked();
    const bool background = m_showBackground->isChecked();
    int shown = 0;
    for (int row = 0; row < m_sessions->rowCount(); ++row) {
        const QString id = m_sessions->item(row, SId)->data(kIdRole).toString();
        const QCborMap s = m_sessionData.value(id);
        bool visible = background || !s.value(QStringLiteral("background")).toBool();
        const bool involved = !s.value(QStringLiteral("blockedBy")).toArray().isEmpty()
                              || m_sessions->item(row, SId)->data(kBlockerRole).toBool();
        if (activeOnly)
            visible = visible && (s.value(QStringLiteral("active")).toBool() || involved);
        if (visible && !words.isEmpty()) {
            QString haystack = s.value(QStringLiteral("sql")).toString();
            for (int column = 0; column < SSql; ++column)
                haystack += u' ' + m_sessions->item(row, column)->text();
            for (const QString &word : words)
                visible = visible && haystack.contains(word, Qt::CaseInsensitive);
        }
        m_sessions->setRowHidden(row, !visible);
        shown += visible ? 1 : 0;
    }
    m_sessionCount->setText(tr("%1 of %2").arg(shown).arg(m_sessions->rowCount()));
}

QString MonitorTab::selectedSessionId() const
{
    const QList<QTableWidgetItem *> selected = m_sessions->selectedItems();
    if (selected.isEmpty())
        return {};
    const QTableWidgetItem *item = m_sessions->item(selected.first()->row(), SId);
    return item ? item->data(kIdRole).toString() : QString();
}

void MonitorTab::selectSession(const QString &id)
{
    for (int row = 0; row < m_sessions->rowCount(); ++row) {
        if (m_sessions->item(row, SId)->data(kIdRole).toString() == id) {
            if (m_sessions->isRowHidden(row)) {
                // 거른 것 때문에 숨어 있으면 거르기를 풀고 보인다
                m_filter->clear();
                m_activeOnly->setChecked(false);
                m_showBackground->setChecked(true);
            }
            m_sessions->selectRow(row);
            m_sessions->scrollToItem(m_sessions->item(row, SId));
            return;
        }
    }
}

void MonitorTab::updateSessionDetails()
{
    const QString id = selectedSessionId();
    if (id.isEmpty()) {
        m_sessionSql->clear();
        return;
    }
    const QCborMap s = m_sessionData.value(id);
    auto value = [&s](const char *key) { return s.value(QLatin1String(key)).toString(); };
    QStringList head;
    QStringList first = {tr("Session %1").arg(id)};
    for (const char *key : {"user", "status"}) {
        if (!value(key).isEmpty())
            first << value(key);
    }
    if (s.value(QStringLiteral("seconds")).isInteger())
        first << duration(s.value(QStringLiteral("seconds")).toInteger());
    head << first.join(QStringLiteral(" · "));
    QStringList wait;
    if (!value("wait").isEmpty())
        wait << tr("Wait: %1").arg(value("wait"));
    const QStringList blockedBy = strings(s.value(QStringLiteral("blockedBy")));
    if (!blockedBy.isEmpty())
        wait << tr("blocked by %1").arg(blockedBy.join(QStringLiteral(", ")));
    if (s.value(QStringLiteral("transactionSeconds")).isInteger())
        wait << tr("transaction open for %1").arg(duration(s.value(QStringLiteral("transactionSeconds")).toInteger()));
    if (!wait.isEmpty())
        head << wait.join(QStringLiteral(" · "));
    QStringList client;
    for (const char *key : {"client", "osUser", "program", "module", "database"}) {
        if (!value(key).isEmpty())
            client << value(key);
    }
    if (!client.isEmpty())
        head << client.join(QStringLiteral(" · "));
    QStringList more;
    if (!value("logonTime").isEmpty())
        more << tr("Logon %1").arg(value("logonTime"));
    if (!value("sqlId").isEmpty())
        more << QStringLiteral("SQL_ID %1").arg(value("sqlId"));
    if (s.value(QStringLiteral("sqlIsPrevious")).toBool() && !value("sql").isEmpty())
        more << tr("last statement");
    if (!more.isEmpty())
        head << more.join(QStringLiteral(" · "));

    QString text;
    for (const QString &line : std::as_const(head))
        text += QStringLiteral("-- ") + line + u'\n';
    text += value("sql");
    m_sessionSql->setPlainText(text);
}

void MonitorTab::updateButtons()
{
    const QString id = selectedSessionId();
    const QCborMap s = m_sessionData.value(id);
    const bool self = s.value(QStringLiteral("self")).toBool();
    const bool canKill = !m_profile.readOnly;
    const QString readOnlyTip = tr("Read-only connection: killing sessions is turned off.");
    m_kill->setEnabled(canKill && !id.isEmpty() && !self);
    m_cancelQuery->setEnabled(canKill && !id.isEmpty() && !self && m_profile.type != DbType::Tibero
                              && s.value(QStringLiteral("active")).toBool());
    m_openSql->setEnabled(!s.value(QStringLiteral("sql")).toString().isEmpty());
    m_kill->setToolTip(canKill ? tr("Disconnect the session; its open transaction is rolled back") : readOnlyTip);

    const QString lockId = lockSelectionId();
    m_killLocker->setEnabled(canKill && !lockId.isEmpty());
    if (!canKill)
        m_killLocker->setToolTip(readOnlyTip);
}

void MonitorTab::kill(const QString &id, bool cancelQuery)
{
    if (id.isEmpty() || m_profile.readOnly)
        return;
    const QCborMap s = m_sessionData.value(id);
    QStringList who;
    for (const char *key : {"user", "program", "client", "status"}) {
        const QString text = s.value(QLatin1String(key)).toString();
        if (!text.isEmpty())
            who << text;
    }
    QString info = who.join(QStringLiteral(" · "));
    const QString sql = s.value(QStringLiteral("sql")).toString();
    if (!sql.isEmpty())
        info += u'\n' + oneLine(sql, 200);
    info += QStringLiteral("\n\n");
    info += cancelQuery ? tr("Only the running statement stops; the session stays connected.")
                        : tr("The session is disconnected and its open transaction is rolled back.");

    QString text = cancelQuery ? tr("Cancel the running statement of session %1?").arg(id) : tr("Kill session %1?").arg(id);
    if (m_profile.environment == Environment::Production)
        text = tr("This is a PRODUCTION database (%1).").arg(m_profile.name) + QStringLiteral("\n\n") + text;
    QMessageBox box(QMessageBox::Warning, cancelQuery ? tr("Cancel Statement") : tr("Kill Session"), text,
                    QMessageBox::Cancel, this);
    QPushButton *yes = box.addButton(cancelQuery ? tr("Cancel Statement") : tr("Kill Session"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setInformativeText(info);
    box.exec();
    if (box.clickedButton() != yes)
        return;

    QCborMap params;
    params.insert(QStringLiteral("targetId"), id);
    params.insert(QStringLiteral("cancelQuery"), cancelQuery);
    QPointer<MonitorTab> self(this);
    connectThen([self, params, id, cancelQuery] {
        if (!self)
            return;
        self->m_session->call(QStringLiteral("killSession"), params,
                              [self, id, cancelQuery](const QCborMap &result, const EngineError &error) {
                                  if (!self)
                                      return;
                                  if (error.isError()) {
                                      self->setStatus(error.message.section(u'\n', 0, 0), true);
                                      self->m_status->setToolTip(error.message);
                                      QMessageBox::warning(self, tr("Kill Session"), error.message);
                                      return;
                                  }
                                  self->setStatus((cancelQuery ? tr("Cancelled the statement of %1: %2")
                                                               : tr("Killed session %1: %2"))
                                                      .arg(id, result.value(QStringLiteral("sql")).toString()));
                                  // 끊긴 세션이 목록에서 빠질 시간을 조금 두고 다시 읽는다
                                  QTimer::singleShot(600, self, [self] {
                                      if (self)
                                          self->refresh();
                                  });
                              });
    });
}

// ---------------------------------------------------------------------------
// 락

void MonitorTab::showLocks(const QCborMap &result)
{
    const QString keep = lockSelectionId();
    const QCborArray waits = result.value(QStringLiteral("waits")).toArray();
    const QCborArray held = result.value(QStringLiteral("held")).toArray();
    const QCborMap errors = result.value(QStringLiteral("errors")).toMap();

    // 막는 세션마다 묶는다 (나온 순서: 오래 기다린 것부터)
    m_waits->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    QFont bold = m_waits->font();
    bold.setBold(true);
    for (const QCborValue &value : waits) {
        const QCborMap w = value.toMap();
        auto text = [&w](const char *key) { return w.value(QLatin1String(key)).toString(); };
        const QString blockerId = text("blockerId");
        QTreeWidgetItem *group = groups.value(blockerId);
        if (!group) {
            group = new QTreeWidgetItem(m_waits);
            group->setText(0, blockerId);
            group->setData(0, kIdRole, blockerId);
            group->setText(1, text("blockerUser"));
            group->setText(2, text("blockerStatus"));
            if (w.value(QStringLiteral("blockerSeconds")).isInteger())
                group->setText(3, duration(w.value(QStringLiteral("blockerSeconds")).toInteger()));
            group->setText(6, oneLine(text("blockerSql")));
            group->setToolTip(6, text("blockerSql"));
            group->setToolTip(0, tr("Blocking session"));
            for (int column = 0; column < 7; ++column) {
                group->setBackground(column, blockerTint());
                group->setFont(column, bold);
            }
            groups.insert(blockerId, group);
        }
        auto *child = new QTreeWidgetItem(group);
        child->setText(0, text("waiterId"));
        child->setData(0, kIdRole, text("waiterId"));
        child->setText(1, text("waiterUser"));
        child->setText(2, text("waitEvent"));
        if (w.value(QStringLiteral("waiterSeconds")).isInteger())
            child->setText(3, duration(w.value(QStringLiteral("waiterSeconds")).toInteger()));
        child->setText(4, text("lock"));
        child->setText(5, text("object"));
        child->setText(6, oneLine(text("waiterSql")));
        child->setToolTip(6, text("waiterSql"));
        child->setToolTip(0, tr("Waiting session"));
        for (int column = 0; column < 7; ++column)
            child->setBackground(column, blockedTint());
    }
    m_waits->expandAll();

    m_held->setSortingEnabled(false);
    m_held->setRowCount(0);
    m_held->setRowCount(int(held.size()));
    int row = 0;
    for (const QCborValue &value : held) {
        const QCborMap h = value.toMap();
        auto text = [&h](const char *key) { return h.value(QLatin1String(key)).toString(); };
        auto *idItem = new SortItem(text("id"), text("id").section(u',', 0, 0).toDouble());
        idItem->setData(kIdRole, text("id"));
        m_held->setItem(row, 0, idItem);
        m_held->setItem(row, 1, new SortItem(text("user")));
        m_held->setItem(row, 2, new SortItem(text("object")));
        m_held->setItem(row, 3, new SortItem(text("mode")));
        m_held->setItem(row, 4, new SortItem(text("status")));
        m_held->setItem(row, 5, secondsItem(h.value(QStringLiteral("seconds"))));
        m_held->setItem(row, 6, new SortItem(text("detail")));
        ++row;
    }
    m_held->setSortingEnabled(true);

    // 고른 것을 다시 고른다
    if (!keep.isEmpty()) {
        for (QTreeWidgetItemIterator it(m_waits); *it; ++it) {
            if ((*it)->data(0, kIdRole).toString() == keep) {
                m_waits->setCurrentItem(*it);
                break;
            }
        }
    }

    const int waiting = int(waits.size());
    if (waiting == 0)
        m_locksSummary->setText(tr("No sessions are waiting for locks."));
    else
        m_locksSummary->setText(tr("%n session(s) waiting, blocked by %1 session(s). Killing a blocking session rolls back its transaction.",
                                   nullptr, waiting)
                                    .arg(groups.size()));
    QStringList problems;
    for (auto it = errors.constBegin(); it != errors.constEnd(); ++it)
        problems << it.value().toString();
    m_locksError->setText(problems.join(u'\n'));
    m_locksError->setVisible(!problems.isEmpty());
    m_locksError->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningTextColor().name()));
    updateButtons();

    QString status = tr("Updated %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    if (waiting > 0)
        status += tr(" · %n waiting for a lock", nullptr, waiting);
    setStatus(status + tr(" · %1 ms").arg(result.value(QStringLiteral("elapsedMs")).toInteger()), waiting > 0);
}

QString MonitorTab::lockSelectionId() const
{
    if (QTreeWidgetItem *item = m_waits->currentItem(); item && item->isSelected())
        return item->data(0, kIdRole).toString();
    const QList<QTableWidgetItem *> selected = m_held->selectedItems();
    if (!selected.isEmpty()) {
        if (const QTableWidgetItem *item = m_held->item(selected.first()->row(), 0))
            return item->data(kIdRole).toString();
    }
    return {};
}

// ---------------------------------------------------------------------------
// 용량

void MonitorTab::showSpace(const QCborMap &result)
{
    m_spaceLoaded = true;
    const QCborArray tablespaces = result.value(QStringLiteral("tablespaces")).toArray();
    const QCborArray databases = result.value(QStringLiteral("databases")).toArray();
    const QCborArray objects = result.value(QStringLiteral("objects")).toArray();
    const QCborMap errors = result.value(QStringLiteral("errors")).toMap();
    const bool oracleLike = m_profile.type == DbType::Oracle || m_profile.type == DbType::Tibero;
    const QString warning = Theme::warningTextColor().name();

    auto titleWithError = [&errors, &warning](QLabel *label, const QString &title, const char *section) {
        const QString error = errors.value(QLatin1String(section)).toString();
        if (error.isEmpty()) {
            label->setText(title);
            label->setToolTip({});
        } else {
            label->setText(QStringLiteral("%1 — <span style=\"color:%2; font-weight:normal;\">%3</span>")
                               .arg(title.toHtmlEscaped(), warning, tr("could not read: %1").arg(error.section(u'\n', 0, 0)).toHtmlEscaped()));
            label->setToolTip(error);
        }
    };

    // 테이블스페이스
    m_tablespaces->setSortingEnabled(false);
    m_tablespaces->setRowCount(0);
    m_tablespaces->setRowCount(int(tablespaces.size()));
    int row = 0;
    for (const QCborValue &value : tablespaces) {
        const QCborMap t = value.toMap();
        const QCborValue allocated = t.value(QStringLiteral("allocatedBytes"));
        const QCborValue used = t.value(QStringLiteral("usedBytes"));
        const QCborValue max = t.value(QStringLiteral("maxBytes"));
        const bool autoextend = t.value(QStringLiteral("autoextend")).toBool();
        auto *usage = new SortItem;
        if (used.isInteger() && allocated.isInteger() && allocated.toInteger() > 0
            && !(m_profile.type == DbType::PostgreSQL)) {
            const double fraction = double(used.toInteger()) / double(allocated.toInteger());
            usage->setText(QStringLiteral("%1%").arg(fraction * 100, 0, 'f', 1));
            usage->setData(kFractionRole, fraction);
            usage->setData(kSortRole, fraction);
            if (autoextend && max.isInteger() && max.toInteger() > allocated.toInteger())
                usage->setToolTip(tr("%1% of the maximum size (auto-extend up to %2)")
                                      .arg(double(used.toInteger()) / double(max.toInteger()) * 100, 0, 'f', 1)
                                      .arg(bytesText(max)));
        } else {
            usage->setData(kSortRole, -1.0);
        }
        m_tablespaces->setItem(row, 0, new SortItem(t.value(QStringLiteral("name")).toString()));
        m_tablespaces->setItem(row, 1, new SortItem(t.value(QStringLiteral("type")).toString()));
        m_tablespaces->setItem(row, 2, new SortItem(t.value(QStringLiteral("status")).toString()));
        m_tablespaces->setItem(row, 3, usage);
        m_tablespaces->setItem(row, 4, bytesItem(used));
        m_tablespaces->setItem(row, 5, bytesItem(allocated));
        m_tablespaces->setItem(row, 6, bytesItem(max));
        m_tablespaces->setItem(row, 7, bytesItem(t.value(QStringLiteral("freeBytes"))));
        m_tablespaces->setItem(row, 8, new SortItem(autoextend ? tr("Yes") : QString()));
        m_tablespaces->setItem(row, 9, countItem(t.value(QStringLiteral("files"))));
        m_tablespaces->setItem(row, 10, new SortItem(t.value(QStringLiteral("location")).toString()));
        ++row;
    }
    m_tablespaces->setSortingEnabled(true);
    // PostgreSQL 테이블스페이스는 디렉터리라 할당·최대·여유가 없다: 이름·크기·위치만
    const bool directories = m_profile.type == DbType::PostgreSQL;
    for (int column : {1, 2, 3, 5, 6, 7, 8, 9})
        m_tablespaces->setColumnHidden(column, directories);
    m_tablespaces->horizontalHeaderItem(4)->setText(directories ? tr("Size") : tr("Used"));
    m_tablespaces->setColumnHidden(10, m_profile.type == DbType::Oracle || m_profile.type == DbType::Tibero);
    const bool noTablespaces = tablespaces.isEmpty() && !errors.contains(QStringLiteral("tablespaces"));
    m_tablespaces->setVisible(!noTablespaces);
    m_tablespacesTitle->parentWidget()->setVisible(!noTablespaces); // MariaDB: 공용 테이블스페이스 정보가 없다
    titleWithError(m_tablespacesTitle,
                   m_profile.type == DbType::MySQL ? tr("InnoDB tablespaces") : tr("Tablespaces"), "tablespaces");

    // DB 크기
    qint64 total = 0;
    QString current;
    qint64 currentBytes = -1;
    m_databases->setSortingEnabled(false);
    m_databases->setRowCount(0);
    if (!oracleLike) {
        m_databases->setRowCount(int(databases.size()));
        row = 0;
        for (const QCborValue &value : databases) {
            const QCborMap d = value.toMap();
            const QString name = d.value(QStringLiteral("name")).toString();
            const QCborValue bytes = d.value(QStringLiteral("bytes"));
            total += bytes.toInteger();
            const bool isCurrent = d.value(QStringLiteral("current")).toBool() || name == m_profile.database;
            if (isCurrent) {
                current = name;
                currentBytes = bytes.toInteger();
            }
            auto *nameItem = new SortItem(name);
            if (isCurrent) {
                QFont font = nameItem->font();
                font.setBold(true);
                nameItem->setFont(font);
                nameItem->setToolTip(tr("The database this connection uses"));
            }
            m_databases->setItem(row, 0, nameItem);
            m_databases->setItem(row, 1, bytesItem(bytes));
            m_databases->setItem(row, 2, bytesItem(d.value(QStringLiteral("dataBytes"))));
            m_databases->setItem(row, 3, bytesItem(d.value(QStringLiteral("indexBytes"))));
            m_databases->setItem(row, 4, bytesItem(d.value(QStringLiteral("freeBytes"))));
            m_databases->setItem(row, 5, countItem(d.value(QStringLiteral("objects"))));
            ++row;
        }
    }
    m_databases->setSortingEnabled(true);
    m_databasesTitle->parentWidget()->setVisible(!oracleLike);
    for (int column : {2, 3, 4, 5})
        m_databases->setColumnHidden(column, m_profile.type == DbType::PostgreSQL);
    titleWithError(m_databasesTitle, m_profile.type == DbType::PostgreSQL ? tr("Databases") : tr("Schemas (databases)"), "databases");

    // 큰 객체
    m_objects->setSortingEnabled(false);
    m_objects->setRowCount(0);
    m_objects->setRowCount(int(objects.size()));
    row = 0;
    for (const QCborValue &value : objects) {
        const QCborMap o = value.toMap();
        m_objects->setItem(row, 0, new SortItem(o.value(QStringLiteral("schema")).toString()));
        m_objects->setItem(row, 1, new SortItem(o.value(QStringLiteral("name")).toString()));
        m_objects->setItem(row, 2, new SortItem(o.value(QStringLiteral("type")).toString()));
        m_objects->setItem(row, 3, bytesItem(o.value(QStringLiteral("bytes"))));
        m_objects->setItem(row, 4, bytesItem(o.value(QStringLiteral("dataBytes"))));
        m_objects->setItem(row, 5, bytesItem(o.value(QStringLiteral("indexBytes"))));
        m_objects->setItem(row, 6, countItem(o.value(QStringLiteral("rows"))));
        m_objects->setItem(row, 7, new SortItem(o.value(QStringLiteral("tablespace")).toString()));
        m_objects->setItem(row, 8, countItem(o.value(QStringLiteral("parts"))));
        ++row;
    }
    m_objects->setSortingEnabled(true);
    for (int column : {4, 5, 6})
        m_objects->setColumnHidden(column, oracleLike);
    m_objects->setColumnHidden(8, !oracleLike);
    m_objects->setColumnHidden(7, m_profile.type == DbType::MySQL || m_profile.type == DbType::MariaDB);
    titleWithError(m_objectsTitle, oracleLike ? tr("Largest segments") : tr("Largest tables"), "objects");

    // 요약
    QString summary;
    if (oracleLike && !databases.isEmpty()) {
        const QCborMap d = databases.first().toMap();
        summary = tr("Database %1: %2 on disk (data files %3, temp files %4, redo logs %5)")
                      .arg(d.value(QStringLiteral("name")).toString(), bytesText(d.value(QStringLiteral("bytes"))),
                           bytesText(d.value(QStringLiteral("dataBytes"))), bytesText(d.value(QStringLiteral("tempBytes"))),
                           bytesText(d.value(QStringLiteral("redoBytes"))));
        if (d.value(QStringLiteral("usedBytes")).isInteger())
            summary += tr(" · segments use %1").arg(bytesText(d.value(QStringLiteral("usedBytes"))));
    } else if (!databases.isEmpty()) {
        if (currentBytes >= 0)
            summary = tr("%1: %2").arg(current, bytesText(QCborValue(currentBytes))) + QStringLiteral(" · ");
        summary += (m_profile.type == DbType::PostgreSQL ? tr("all databases: %1") : tr("all schemas: %1"))
                       .arg(bytesText(QCborValue(total)));
    }
    if (m_profile.type == DbType::PostgreSQL)
        summary += tr(" · tables are those of this database");
    else if (m_profile.type == DbType::MySQL || m_profile.type == DbType::MariaDB)
        summary += tr(" · sizes come from the table statistics");
    m_spaceSummary->setText(summary);
    setStatus(tr("Space read at %1 · %2 ms")
                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")))
                  .arg(result.value(QStringLiteral("elapsedMs")).toInteger()),
              !errors.isEmpty());
}
