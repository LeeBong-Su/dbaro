#include "PlanView.h"

#include "ScrollHelpers.h"

#include <QButtonGroup>
#include <QCborArray>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

// 숫자 컬럼은 오른쪽 정렬
bool isNumericColumn(const QString &name)
{
    static const QStringList numeric{QStringLiteral("Rows"), QStringLiteral("Bytes"), QStringLiteral("Cost"),
                                     QStringLiteral("Width"), QStringLiteral("rows"), QStringLiteral("filtered")};
    return numeric.contains(name);
}

} // namespace

PlanView::PlanView(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("planTitle"));

    m_treeButton = new QToolButton(this);
    m_treeButton->setText(tr("Tree"));
    m_textButton = new QToolButton(this);
    m_textButton->setText(tr("Text"));
    auto *group = new QButtonGroup(this);
    for (QToolButton *button : {m_treeButton, m_textButton}) {
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button);
    }
    m_treeButton->setChecked(true);
    connect(m_treeButton, &QToolButton::clicked, this, [this] { showTree(true); });
    connect(m_textButton, &QToolButton::clicked, this, [this] { showTree(false); });

    m_model = new QStandardItemModel(this);
    m_tree = new QTreeView(this);
    m_tree->setObjectName(QStringLiteral("planTree"));
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setModel(m_model);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("planText"));
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    enableShiftWheelHorizontalScroll(m_tree);
    enableShiftWheelHorizontalScroll(m_text);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_text);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(8, 4, 8, 4);
    header->setSpacing(4);
    header->addWidget(m_title, 1);
    header->addWidget(m_treeButton);
    header->addWidget(m_textButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack, 1);

    clear();
}

void PlanView::clear()
{
    m_model->clear();
    m_text->clear();
    m_title->setText(tr("Press Ctrl+E to see the estimated plan of the statement at the cursor."));
    m_treeButton->setVisible(false);
    m_textButton->setVisible(false);
    showTree(true);
}

void PlanView::setPlan(const QCborMap &plan)
{
    m_model->clear();
    const QCborArray columns = plan.value(QStringLiteral("columns")).toArray();
    QStringList headers;
    for (const QCborValue &column : columns)
        headers << column.toString();
    m_model->setHorizontalHeaderLabels(headers);

    // 노드는 부모가 먼저 오지 않을 수도 있어서 id로 찾아 붙인다.
    const QCborArray nodes = plan.value(QStringLiteral("nodes")).toArray();
    QHash<qint64, QStandardItem *> byId;
    QList<QPair<qint64, QList<QStandardItem *>>> rows;
    for (const QCborValue &value : nodes) {
        const QCborMap node = value.toMap();
        const QCborArray cells = node.value(QStringLiteral("cells")).toArray();
        QList<QStandardItem *> row;
        for (int column = 0; column < headers.size(); ++column) {
            auto *item = new QStandardItem(column < cells.size() ? cells.at(column).toString() : QString());
            item->setToolTip(item->text());
            if (isNumericColumn(headers.at(column)))
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row << item;
        }
        if (row.isEmpty())
            continue;
        byId.insert(node.value(QStringLiteral("id")).toInteger(), row.first());
        rows.append({node.value(QStringLiteral("parent")).toInteger(-1), row});
    }
    for (const auto &[parentId, row] : std::as_const(rows)) {
        QStandardItem *parent = byId.value(parentId);
        if (parent && parent != row.first())
            parent->appendRow(row);
        else
            m_model->appendRow(row);
    }

    const QString text = plan.value(QStringLiteral("text")).toString();
    m_text->setPlainText(text);

    const bool hasTree = !nodes.isEmpty();
    const bool hasText = !text.isEmpty();
    m_treeButton->setVisible(hasTree && hasText);
    m_textButton->setVisible(hasTree && hasText);
    m_title->setText(hasTree ? tr("Estimated plan: %n step(s)", nullptr, int(nodes.size())) : tr("Estimated plan"));

    m_tree->expandAll();
    for (int column = 0; column < headers.size(); ++column) {
        m_tree->resizeColumnToContents(column);
        m_tree->setColumnWidth(column, qMin(m_tree->columnWidth(column) + 12, 520));
    }
    showTree(hasTree);
}

QString PlanView::planText() const
{
    return m_text->toPlainText();
}

bool PlanView::showsText() const
{
    return m_stack->currentWidget() == m_text;
}

void PlanView::showTree(bool tree)
{
    m_stack->setCurrentWidget(tree ? static_cast<QWidget *>(m_tree) : m_text);
    m_treeButton->setChecked(tree);
    m_textButton->setChecked(!tree);
}
