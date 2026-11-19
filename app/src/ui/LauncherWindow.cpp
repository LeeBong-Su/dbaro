#include "LauncherWindow.h"

#include "AppController.h"
#include "Icons.h"
#include "MainWindow.h"
#include "UiHelpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
const auto kGeometryKey = QStringLiteral("launcher/geometry");
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
    m_list->setHeaderLabels({tr("Name"), tr("Database"), tr("Address"), tr("User"), tr("Environment"), QString()});
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->setColumnWidth(1, 100);
    m_list->setColumnWidth(2, 220);
    m_list->setColumnWidth(3, 110);
    m_list->setColumnWidth(4, 130);
    m_list->setColumnWidth(5, 60);
    m_list->installEventFilter(this);

    m_empty = new QLabel(tr("No connections yet. Click New Connection to add one."), this);
    m_empty->setObjectName(QStringLiteral("launcherEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);

    auto *newButton = new QPushButton(Icons::get(QStringLiteral("database-add")), tr("New Connection..."), this);
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

    for (const ConnectionProfile &profile : std::as_const(profiles)) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, profile.name);
        const QColor color = Env::color(profile.environment);
        item->setIcon(0, color.isValid() ? colorSwatchIcon(color, 10) : Icons::get(QStringLiteral("database")));
        item->setText(1, Db::typeName(profile.type));
        item->setText(2, QStringLiteral("%1:%2%3").arg(profile.host).arg(profile.port)
                             .arg(profile.database.isEmpty() ? QString() : u'/' + profile.database));
        item->setText(3, profile.user);
        item->setText(4, profile.environment == Environment::None ? QString() : Env::name(profile.environment));
        if (color.isValid())
            item->setIcon(4, colorSwatchIcon(color, 8));
        if (m_app->windowFor(profile.id))
            item->setText(5, tr("Open"));
        item->setData(0, kIdRole, profile.id);
        item->setToolTip(0, Db::jdbcUrl(profile));
        if (profile.id == keep)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));

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
                                     .arg(item->text(0), item->text(1), item->text(2), item->text(3), item->text(4));
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

void LauncherWindow::closeEvent(QCloseEvent *event)
{
    QSettings().setValue(kGeometryKey, saveGeometry());
    QWidget::closeEvent(event);
    if (event->isAccepted() && event->spontaneous())
        emit closedByUser();
}
