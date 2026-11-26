#include "PlanView.h"

#include "BlockSelectTextView.h"
#include "ScrollHelpers.h"
#include "Theme.h"

#include <QButtonGroup>
#include <QCborArray>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

constexpr int kObjectRole = Qt::UserRole + 1; // 줄 첫 칸: 단계가 읽는 객체 (QCborMap)

// 숫자 컬럼은 오른쪽 정렬
bool isNumericColumn(const QString &name)
{
    static const QStringList numeric{
        QStringLiteral("Rows"),     QStringLiteral("Bytes"),       QStringLiteral("Cost"),        QStringLiteral("Width"),
        QStringLiteral("rows"),     QStringLiteral("filtered"),    QStringLiteral("r_rows"),      QStringLiteral("r_filtered"),
        QStringLiteral("Starts"),   QStringLiteral("E-Rows"),      QStringLiteral("A-Rows"),      QStringLiteral("A-Time (ms)"),
        QStringLiteral("Buffers"),  QStringLiteral("Reads"),       QStringLiteral("Actual Rows"), QStringLiteral("Loops"),
        QStringLiteral("Time (ms)"), QStringLiteral("Writes"),   QStringLiteral("OMem"),        QStringLiteral("1Mem"),
        QStringLiteral("Used-Mem"),  QStringLiteral("Used-Tmp"),
    };
    return numeric.contains(name);
}

// 1.50 ms, 125.3 ms, 2.41 s
QString formatMilliseconds(double ms)
{
    const QLocale locale;
    if (ms < 100)
        return QCoreApplication::translate("PlanView", "%1 ms").arg(locale.toString(ms, 'f', 2));
    if (ms < 1000)
        return QCoreApplication::translate("PlanView", "%1 ms").arg(locale.toString(ms, 'f', 1));
    return QCoreApplication::translate("PlanView", "%1 s").arg(locale.toString(ms / 1000, 'f', 2));
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
    // 객체 이름(링크)을 두 번 클릭해야 그 단계가 읽는 테이블·인덱스 정보를 별도 창으로 띄운다.
    // 줄을 한 번 누르거나 다른 칸을 두 번 눌러서는 열리지 않는다 (계획을 훑어보다 창이 튀어나오지 않게).
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.column() == m_linkColumn)
            requestObject(index);
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_tree->indexAt(pos);
        if (objectAt(index).isEmpty())
            return;
        QMenu menu(this);
        menu.addAction(tr("Show Object Info"), this, [this, index] { requestObject(index); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });

    m_text = new BlockSelectTextView(this); // Alt+드래그로 컬럼(세로) 범위를 골라 복사
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
    m_title->setText(tr("Press Ctrl+E for the estimated plan of the statement at the cursor, or Alt+X to run it and see the actual plan."));
    m_actual = false;
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
    const int objectColumn = int(headers.indexOf(QStringLiteral("Object")));
    m_linkColumn = qMax(objectColumn, 0);
    // 링크가 첫 칸(MySQL처럼 Object 컬럼이 없음)이면 두 번 클릭이 펼치기·접기와 겹치지 않게 한다
    m_tree->setExpandsOnDoubleClick(m_linkColumn != 0);
    m_actual = plan.value(QStringLiteral("actual")).toBool();
    const int actualRowsColumn = int(headers.indexOf(plan.value(QStringLiteral("actualRowsColumn")).toString()));
    const QLocale locale;
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
        const QCborMap object = node.value(QStringLiteral("object")).toMap();
        if (!object.value(QStringLiteral("name")).toString().isEmpty()) {
            row.first()->setData(QVariant::fromValue(object), kObjectRole);
            // 누를 수 있는 객체 이름은 링크처럼 (MySQL처럼 Object 컬럼이 없으면 첫 칸)
            QStandardItem *link = objectColumn >= 0 && objectColumn < row.size() ? row[objectColumn] : row.first();
            QFont font = link->font();
            font.setUnderline(true);
            link->setFont(font);
            link->setForeground(m_tree->palette().color(QPalette::Link));
            link->setToolTip(tr("%1\nDouble-click to see the table and index information").arg(link->text()));
        }
        if (m_actual) {
            // 실행되지 않은 단계는 흐리게
            if (std::any_of(row.cbegin(), row.cend(), [](const QStandardItem *item) { return item->text() == u"never executed"; })) {
                for (QStandardItem *item : std::as_const(row))
                    item->setForeground(m_tree->palette().color(QPalette::Disabled, QPalette::Text));
            }
            // 예상과 실제 행 수가 크게 다른 단계: 옵티마이저가 잘못 짐작한 곳이라 계획이 나빠지기 쉽다
            const QCborMap rows = node.value(QStringLiteral("rows")).toMap();
            if (!rows.isEmpty() && actualRowsColumn >= 0 && actualRowsColumn < row.size()) {
                const qint64 estimated = rows.value(QStringLiteral("estimated")).toInteger();
                const qint64 actualRows = rows.value(QStringLiteral("actual")).toInteger();
                if (isMisestimate(estimated, actualRows)) {
                    QStandardItem *cell = row[actualRowsColumn];
                    QFont font = cell->font();
                    font.setBold(true);
                    cell->setFont(font);
                    cell->setForeground(Theme::warningTextColor());
                    cell->setData(true, Qt::UserRole + 2);
                    cell->setToolTip(tr("Estimated %1 rows, actual %2.\nThe optimizer misjudged this step, so the plan may not suit the data. "
                                        "Check the statistics of the tables and indexes it reads.")
                                         .arg(locale.toString(estimated), locale.toString(actualRows)));
                }
            }
        }
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
    if (!m_actual) {
        m_title->setText(hasTree ? tr("Estimated plan: %n step(s)", nullptr, int(nodes.size())) : tr("Estimated plan"));
    } else {
        // 실제 계획: 단계 수 · 돌려준 행 수 · 실행 시간 (계획 시간) · 되돌림
        QStringList parts{hasTree ? tr("Actual plan: %n step(s)", nullptr, int(nodes.size())) : tr("Actual plan")};
        if (plan.contains(QStringLiteral("resultRows")) && !plan.value(QStringLiteral("resultRows")).isNull()) {
            const qint64 resultRows = plan.value(QStringLiteral("resultRows")).toInteger();
            parts << tr("%1 row(s)", nullptr, int(qMin<qint64>(resultRows, INT_MAX))).arg(locale.toString(resultRows));
        }
        if (plan.contains(QStringLiteral("executionMs"))) {
            QString executed = tr("executed in %1").arg(formatMilliseconds(plan.value(QStringLiteral("executionMs")).toDouble()));
            if (plan.contains(QStringLiteral("planningMs")) && !plan.value(QStringLiteral("planningMs")).isNull())
                executed += u' ' + tr("(planning %1)").arg(formatMilliseconds(plan.value(QStringLiteral("planningMs")).toDouble()));
            parts << executed;
        }
        if (plan.value(QStringLiteral("rolledBack")).toBool())
            parts << tr("changes rolled back");
        m_title->setText(parts.join(QStringLiteral(" · ")));
    }

    // 마지막 컬럼이 숫자(MySQL Cost, Buffers 등)면 늘리지 않는다: 오른쪽 정렬이라 숫자가 멀리 떨어진다
    m_tree->header()->setStretchLastSection(headers.isEmpty() || !isNumericColumn(headers.last()));
    m_tree->expandAll();
    for (int column = 0; column < headers.size(); ++column) {
        m_tree->resizeColumnToContents(column);
        m_tree->setColumnWidth(column, qMin(m_tree->columnWidth(column) + 12, 520));
    }
    // 보통은 트리를 먼저, 엔진이 글을 먼저 보이라고 하면(Oracle 실제 계획의 ADVANCED ALLSTATS LAST) 글을 먼저
    showTree(hasTree && !(hasText && plan.value(QStringLiteral("textFirst")).toBool()));
}

QCborMap PlanView::objectAt(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};
    return index.siblingAtColumn(0).data(kObjectRole).value<QCborMap>();
}

void PlanView::requestObject(const QModelIndex &index)
{
    const QCborMap object = objectAt(index);
    if (!object.isEmpty())
        emit objectRequested(object);
}

QString PlanView::planText() const
{
    return m_text->toPlainText();
}

bool PlanView::showsText() const
{
    return m_stack->currentWidget() == m_text;
}

QString PlanView::title() const
{
    return m_title->text();
}

bool PlanView::isMisestimate(qint64 estimated, qint64 actual)
{
    const qint64 low = qMax<qint64>(1, qMin(estimated, actual));
    const qint64 high = qMax(estimated, actual);
    return high - qMin(estimated, actual) >= 100 && high >= 10 * low;
}

void PlanView::showTree(bool tree)
{
    m_stack->setCurrentWidget(tree ? static_cast<QWidget *>(m_tree) : m_text);
    m_treeButton->setChecked(tree);
    m_textButton->setChecked(!tree);
}
