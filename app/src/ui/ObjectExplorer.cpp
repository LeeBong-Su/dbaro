#include "ObjectExplorer.h"

#include "UiHelpers.h"

#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace {
constexpr int kConnectionIdRole = Qt::UserRole + 1;
}

ObjectExplorer::ObjectExplorer(QWidget *parent)
    : QWidget(parent)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter objects"));
    m_filter->setClearButtonEnabled(true);

    m_model = new QStandardItemModel(this);

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setRecursiveFilteringEnabled(true);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString id = index.data(kConnectionIdRole).toString();
        if (!id.isEmpty())
            emit connectionActivated(id);
    });

    setConnections({});
}

void ObjectExplorer::setConnections(const QList<ConnectionProfile> &profiles)
{
    m_model->clear();

    if (profiles.isEmpty()) {
        auto *empty = new QStandardItem(tr("No connections yet"));
        empty->setFlags(Qt::NoItemFlags);
        m_model->appendRow(empty);
        return;
    }

    for (const ConnectionProfile &profile : profiles) {
        auto *item = new QStandardItem(colorSwatchIcon(Env::color(profile.environment)), profile.name);
        item->setData(profile.id, kConnectionIdRole);
        item->setToolTip(QStringLiteral("%1 — %2").arg(Db::typeName(profile.type), Db::jdbcUrl(profile)));
        m_model->appendRow(item);
    }
}
