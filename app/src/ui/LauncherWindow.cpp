#include "LauncherWindow.h"

#include "AppController.h"
#include "Icons.h"
#include "MainWindow.h"
#include "UiHelpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCollator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kSortRole = Qt::UserRole + 2; // 글자 대신 이 값으로 정렬하는 컬럼 (환경: 개발 → 운영)
const auto kGeometryKey = QStringLiteral("launcher/geometry");
const auto kColumnsKey = QStringLiteral("launcher/columns"); // 머리글 상태: 순서·너비·정렬

enum Column { NameColumn, TypeColumn, AddressColumn, UserColumn, EnvironmentColumn, StatusColumn, ColumnCount };

// 정렬: 대소문자 무시, 숫자는 크기대로 (dev2 < dev10, 10.0.0.2 < 10.0.0.10).
// 같은 값끼리는 최근에 연 순서를 그대로 둔다 (Qt가 안정 정렬을 쓴다).
class ConnectionItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : NameColumn;
        const QVariant key = data(column, kSortRole);
        if (key.isValid())
            return key.toInt() < other.data(column, kSortRole).toInt();
        return collator().compare(text(column), other.text(column)) < 0;
    }

private:
    static const QCollator &collator()
    {
        static const QCollator instance = [] {
            QCollator c;
            c.setCaseSensitivity(Qt::CaseInsensitive);
            c.setNumericMode(true);
            return c;
        }();
        return instance;
    }
};
} // namespace

LauncherWindow::LauncherWindow(AppController *app, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_app(app)
{
    setObjectName(QStringLiteral("launcherWindow"));
    setAttribute(Qt::WA_StyledBackground);
    setWindowTitle(QStringLiteral("DBaro"));
    resize(820, 520);

    auto *logo = new QLabel(this);
    logo->setPixmap(Icons::get(QStringLiteral("dbaro")).pixmap(48, 48));
    auto *title = new QLabel(QStringLiteral("DBaro"), this);
    title->setObjectName(QStringLiteral("launcherTitle"));
    auto *subtitle = new QLabel(tr("Choose a connection. Each connection opens in its own window."), this);
    subtitle->setObjectName(QStringLiteral("launcherSubtitle"));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(title);
    titles->addWidget(subtitle);
    auto *header = new QHBoxLayout;
    header->setSpacing(14);
    header->addWidget(logo);
    header->addLayout(titles, 1);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("launcherFilter"));
    m_filter->setPlaceholderText(tr("Search connections"));
    m_filter->setClearButtonEnabled(true);
    m_filter->installEventFilter(this);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("launcherList"));
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setHeaderLabels({tr("Name"), tr("Database"), tr("Address"), tr("User"), tr("Environment"), tr("Window")});
    m_list->installEventFilter(this);
    setUpColumns();

    m_empty = new QLabel(tr("No connections yet. Click New Connection to add one."), this);
    m_empty->setObjectName(QStringLiteral("launcherEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);

    auto *newButton = new QPushButton(Icons::get(QStringLiteral("connection-add")), tr("New Connection..."), this);
    m_edit = new QPushButton(tr("Edit..."), this);
    m_open = new QPushButton(tr("Open"), this);
    m_open->setDefault(true);
    m_reopen = new QCheckBox(tr("Reopen the connection windows that were open when DBaro was last closed"), this);
    m_reopen->setChecked(app->reopenAtStartup());

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(newButton);
    buttons->addWidget(m_edit);
    buttons->addStretch();
    buttons->addWidget(m_open);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(10);
    layout->addLayout(header);
    layout->addSpacing(6);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_empty, 1);
    layout->addLayout(buttons);
    layout->addWidget(m_reopen);

    connect(m_filter, &QLineEdit::textChanged, this, &LauncherWindow::applyFilter);
    connect(m_list, &QTreeWidget::itemActivated, this, &LauncherWindow::openSelected);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &LauncherWindow::updateButtons);
    connect(m_open, &QPushButton::clicked, this, &LauncherWindow::openSelected);
    connect(newButton, &QPushButton::clicked, this, [this] { m_app->newConnection(this); });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        QString selected;
        if (m_app->editConnections(this, false, selectedId(), &selected)) {
            refresh();
            for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
                if (m_list->topLevelItem(i)->data(0, kIdRole).toString() == selected)
                    m_list->setCurrentItem(m_list->topLevelItem(i));
            }
        }
    });
    connect(m_reopen, &QCheckBox::toggled, m_app, &AppController::setReopenAtStartup);
    connect(m_app, &AppController::connectionsChanged, this, &LauncherWindow::refresh);
    connect(m_app, &AppController::windowsChanged, this, &LauncherWindow::refresh);

    restoreGeometry(QSettings().value(kGeometryKey).toByteArray());
    refresh();
}

void LauncherWindow::setUpColumns()
{
    QHeaderView *header = m_list->header();
    header->setSectionResizeMode(QHeaderView::Interactive); // 모든 컬럼 너비를 끌어서 바꾼다
    header->setStretchLastSection(true);                     // 맨 오른쪽 컬럼이 남는 폭을 채운다
    header->setMinimumSectionSize(40);

    const QByteArray saved = QSettings().value(kColumnsKey).toByteArray();
    if (saved.isEmpty() || !header->restoreState(saved) || header->count() != ColumnCount)
        resetColumns();

    // 저장한 상태와 상관없이 늘 켜 둔다
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(true); // 이름 컬럼도 옮길 수 있게
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicatorClearable(true); // 세 번째로 누르면 정렬을 풀고 최근 순으로
    const QString tip = tr("Click to sort, click again to reverse, a third time to go back to the recently used order.\n"
                           "Drag a column to move it, drag its edge to resize it. Right-click to reset the columns.");
    for (int column = 0; column < ColumnCount; ++column)
        m_list->headerItem()->setToolTip(column, tip);

    m_saveColumnsTimer = new QTimer(this);
    m_saveColumnsTimer->setSingleShot(true);
    m_saveColumnsTimer->setInterval(400); // 끄는 동안 계속 쓰지 않게
    connect(m_saveColumnsTimer, &QTimer::timeout, this, &LauncherWindow::saveColumns);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_saveColumnsTimer->isActive())
            saveColumns();
    });
    connect(header, &QHeaderView::sectionResized, m_saveColumnsTimer, qOverload<>(&QTimer::start));
    connect(header, &QHeaderView::sectionMoved, m_saveColumnsTimer, qOverload<>(&QTimer::start));
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this](int column) {
        onSortChanged(column);
        m_saveColumnsTimer->start();
    });

    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QHeaderView::customContextMenuRequested, this, [this, header](const QPoint &pos) {
        QMenu menu(this);
        menu.addAction(tr("Reset Columns"), this, [this] {
            resetColumns();
            saveColumns();
        });
        menu.exec(header->viewport()->mapToGlobal(pos));
    });
}

void LauncherWindow::resetColumns()
{
    QHeaderView *header = m_list->header();
    for (int column = 0; column < header->count(); ++column)
        header->moveSection(header->visualIndex(column), column);
    static constexpr int kWidths[ColumnCount] = {200, 95, 210, 100, 115, 60};
    for (int column = 0; column < ColumnCount; ++column)
        header->resizeSection(column, kWidths[column]);
    header->setSortIndicator(-1, Qt::AscendingOrder); // 최근 순 (refresh가 이 순서로 만든다)
}

void LauncherWindow::onSortChanged(int column)
{
    if (column < 0)
        refresh(); // 정렬을 풀었다: 최근에 연 순서로 다시 만든다
    else
        m_list->sortItems(column, m_list->header()->sortIndicatorOrder());
    if (QTreeWidgetItem *current = m_list->currentItem())
        m_list->scrollToItem(current);
}

void LauncherWindow::saveColumns()
{
    m_saveColumnsTimer->stop();
    QSettings().setValue(kColumnsKey, m_list->header()->saveState());
}

void LauncherWindow::refresh()
{
    const QString keep = selectedId();
    m_list->clear();

    // 최근에 연 연결이 위, 나머지는 이름 순
    QList<ConnectionProfile> profiles = m_app->store().profiles();
    const QStringList recent = m_app->recentConnections();
    std::stable_sort(profiles.begin(), profiles.end(), [&recent](const ConnectionProfile &a, const ConnectionProfile &b) {
        const qsizetype ra = recent.indexOf(a.id);
        const qsizetype rb = recent.indexOf(b.id);
        if (ra != rb)
            return ra >= 0 && (rb < 0 || ra < rb);
        return a.name.localeAwareCompare(b.name) < 0;
    });

    QTreeWidgetItem *mostRecent = nullptr;
    for (const ConnectionProfile &profile : std::as_const(profiles)) {
        auto *item = new ConnectionItem(m_list);
        if (!mostRecent)
            mostRecent = item;
        item->setText(NameColumn, profile.name);
        const QColor color = Env::color(profile.environment);
        item->setIcon(NameColumn, color.isValid() ? colorSwatchIcon(color, 10) : Icons::get(QStringLiteral("database")));
        item->setText(TypeColumn, Db::typeName(profile.type));
        item->setText(AddressColumn, QStringLiteral("%1:%2%3").arg(profile.host).arg(profile.port)
                                         .arg(profile.database.isEmpty() ? QString() : u'/' + profile.database));
        item->setText(UserColumn, profile.user);
        item->setText(EnvironmentColumn,
                      profile.environment == Environment::None ? QString() : Env::name(profile.environment));
        item->setData(EnvironmentColumn, kSortRole, int(Env::all().indexOf(profile.environment))); // 없음 → 개발 → ... → 운영
        if (color.isValid())
            item->setIcon(EnvironmentColumn, colorSwatchIcon(color, 8));
        if (m_app->windowFor(profile.id))
            item->setText(StatusColumn, tr("Open"));
        item->setData(NameColumn, kIdRole, profile.id);
        item->setToolTip(NameColumn, Db::jdbcUrl(profile));
        if (profile.id == keep)
            m_list->setCurrentItem(item);
    }
    // 머리글에서 고른 정렬이 있으면 그 순서로 (같은 값끼리는 위의 최근 순서를 지킨다)
    if (const int column = m_list->header()->sortIndicatorSection(); column >= 0 && column < ColumnCount)
        m_list->sortItems(column, m_list->header()->sortIndicatorOrder());
    // 처음에는 마지막에 연 연결을 고른다 (정렬해도 Enter로 바로 다시 열게)
    if (!m_list->currentItem() && mostRecent)
        m_list->setCurrentItem(mostRecent);
    if (QTreeWidgetItem *current = m_list->currentItem(); current && m_list->isVisible())
        m_list->scrollToItem(current); // 보이기 전에는 크기를 몰라 맨 위로 밀어 버린다: showEvent에서

    const bool empty = profiles.isEmpty();
    m_list->setVisible(!empty);
    m_filter->setVisible(!empty);
    m_empty->setVisible(empty);
    applyFilter();
}

void LauncherWindow::setFilterText(const QString &text)
{
    m_filter->setText(text);
}

void LauncherWindow::applyFilter()
{
    const QStringList words = m_filter->text().split(u' ', Qt::SkipEmptyParts);
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        const QString haystack = QStringLiteral("%1 %2 %3 %4 %5")
                                     .arg(item->text(NameColumn), item->text(TypeColumn), item->text(AddressColumn),
                                          item->text(UserColumn), item->text(EnvironmentColumn));
        bool visible = true;
        for (const QString &word : words)
            visible = visible && haystack.contains(word, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (visible && !firstVisible)
            firstVisible = item;
    }
    if (!m_list->currentItem() || m_list->currentItem()->isHidden())
        m_list->setCurrentItem(firstVisible);
    updateButtons();
}

void LauncherWindow::updateButtons()
{
    const bool has = !selectedId().isEmpty();
    m_open->setEnabled(has);
    m_edit->setEnabled(!m_app->store().profiles().isEmpty());
}

QString LauncherWindow::selectedId() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return item && !item->isHidden() ? item->data(0, kIdRole).toString() : QString();
}

void LauncherWindow::openSelected()
{
    const QString id = selectedId();
    if (!id.isEmpty())
        m_app->openConnection(id);
}

bool LauncherWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (watched == m_filter && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            QApplication::sendEvent(m_list, event); // 검색란에서 위·아래로 목록을 고른다
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            openSelected();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LauncherWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 정렬한 목록에서 고른 연결(마지막에 연 연결)이 화면 밖이면 보이게 한다. 배치가 끝난 뒤에.
    QTimer::singleShot(0, this, [this] {
        if (QTreeWidgetItem *current = m_list->currentItem())
            m_list->scrollToItem(current);
    });
}

void LauncherWindow::closeEvent(QCloseEvent *event)
{
    QSettings().setValue(kGeometryKey, saveGeometry());
    if (m_saveColumnsTimer->isActive())
        saveColumns();
    QWidget::closeEvent(event);
    if (event->isAccepted() && event->spontaneous())
        emit closedByUser();
}
