#include "ObjectExplorer.h"

#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

ObjectExplorer::ObjectExplorer(QWidget *parent)
    : QWidget(parent)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter objects"));
    m_filter->setClearButtonEnabled(true);

    // 임시 모델. 엔진의 메타데이터 조회가 생기면 필요할 때 불러오는 모델로 교체한다.
    m_model = new QStandardItemModel(this);
    auto *empty = new QStandardItem(tr("No connections yet"));
    empty->setFlags(Qt::NoItemFlags);
    m_model->appendRow(empty);

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setRecursiveFilteringEnabled(true);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
}
