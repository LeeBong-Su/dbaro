#include "PlanObjectWindow.h"

#include "Theme.h"

#include <QCborArray>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QTableWidget *makeTable(QWidget *parent)
{
    auto *table = new QTableWidget(0, 0, parent);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    return table;
}

QStringList strings(const QCborValue &list)
{
    QStringList items;
    for (const QCborValue &value : list.toArray())
        items << value.toString();
    return items;
}

// [[이름, 값]...] → 순서를 지킨 목록
QList<QPair<QString, QString>> pairs(const QCborValue &list)
{
    QList<QPair<QString, QString>> items;
    for (const QCborValue &value : list.toArray()) {
        const QCborArray pair = value.toArray();
        if (pair.size() == 2)
            items.append({pair.at(0).toString(), pair.at(1).toString()});
    }
    return items;
}

// 인덱스 컬럼 "NAME DESC" → "NAME"
QString columnOf(const QString &indexColumn)
{
    return indexColumn.section(u' ', 0, 0);
}

void fit(QTableWidget *table)
{
    table->resizeColumnsToContents();
    for (int c = 0; c < table->columnCount() - 1; ++c)
        table->setColumnWidth(c, qMin(table->columnWidth(c) + 12, 360));
}

} // namespace

PlanObjectWindow::PlanObjectWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("planObjectWindow"));
    setWindowTitle(tr("Plan Object"));
    setAttribute(Qt::WA_ShowWithoutActivating); // 계획 트리에서 방향키로 계속 옮겨 다닐 수 있게
    resize(820, 560);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("planObjectTitle"));
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("planObjectSummary"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_indexes = makeTable(this);
    m_indexes->setObjectName(QStringLiteral("planObjectIndexes"));
    m_columns = makeTable(this);
    m_columns->setObjectName(QStringLiteral("planObjectColumns"));
    m_stats = makeTable(this);
    m_stats->setObjectName(QStringLiteral("planObjectStats"));

    // 위: 인덱스 (계획이 쓴 인덱스를 칠한다), 아래: 컬럼과 테이블 통계
    auto *indexPane = new QWidget(this);
    auto *indexLabel = new QLabel(tr("Indexes"), indexPane);
    indexLabel->setObjectName(QStringLiteral("planObjectSection"));
    auto *indexLayout = new QVBoxLayout(indexPane);
    indexLayout->setContentsMargins(0, 0, 0, 0);
    indexLayout->setSpacing(4);
    indexLayout->addWidget(indexLabel);
    indexLayout->addWidget(m_indexes);

    m_pages = new QTabWidget(this);
    m_pages->addTab(m_columns, tr("Columns"));
    m_pages->addTab(m_stats, tr("Table Statistics"));

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(indexPane);
    splitter->addWidget(m_pages);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setChildrenCollapsible(false);

    m_detailsButton = new QPushButton(tr("Table Details"), this);
    m_detailsButton->setToolTip(tr("Open the table in a details tab (columns, indexes, foreign keys, DDL)"));
    connect(m_detailsButton, &QPushButton::clicked, this, [this] {
        if (!m_table.isEmpty())
            emit tableDetailsRequested(m_schema, m_table);
    });
    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_detailsButton);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    layout->addWidget(m_title);
    layout->addWidget(m_summary);
    layout->addWidget(splitter, 1);
    layout->addLayout(buttons);
}

int PlanObjectWindow::beginLoading(const QString &display)
{
    m_loading = true;
    m_schema.clear();
    m_table.clear();
    m_index.clear();
    setWindowTitle(tr("Plan Object — %1").arg(display));
    m_title->setText(QStringLiteral("<b>%1</b>").arg(display.toHtmlEscaped()));
    m_summary->setText(tr("Loading..."));
    m_summary->setProperty("error", false);
    for (QTableWidget *table : {m_indexes, m_columns, m_stats}) {
        table->clear();
        table->setRowCount(0);
        table->setColumnCount(0);
    }
    m_detailsButton->setEnabled(false);
    return ++m_ticket;
}

void PlanObjectWindow::showError(int ticket, const QString &message)
{
    if (ticket != m_ticket)
        return;
    m_loading = false;
    m_summary->setText(tr("Could not read the object: %1").arg(message));
    m_summary->setProperty("error", true);
    m_summary->style()->unpolish(m_summary);
    m_summary->style()->polish(m_summary);
}

void PlanObjectWindow::showResult(int ticket, const QCborMap &result)
{
    if (ticket != m_ticket)
        return;
    m_loading = false;
    const QCborMap table = result.value(QStringLiteral("table")).toMap();
    m_schema = table.value(QStringLiteral("schema")).toString();
    m_table = table.value(QStringLiteral("name")).toString();
    m_index = result.value(QStringLiteral("index")).toString();
    const QString qualified = m_schema.isEmpty() ? m_table : m_schema + u'.' + m_table;
    setWindowTitle(tr("Plan Object — %1").arg(qualified));

    // 제목: 테이블 이름과 종류, 쓴 인덱스
    QString title = QStringLiteral("<b>%1</b>  <span style='color:gray'>%2</span>")
                        .arg(qualified.toHtmlEscaped(), table.value(QStringLiteral("type")).toString().toHtmlEscaped());
    if (!m_index.isEmpty())
        title += tr("  ·  index <b>%1</b>").arg(m_index.toHtmlEscaped());
    m_title->setText(title);

    const QList<QPair<QString, QString>> tableStats = pairs(result.value(QStringLiteral("tableStats")));
    const QCborMap indexStats = result.value(QStringLiteral("indexStats")).toMap();
    const QCborArray indexes = table.value(QStringLiteral("indexes")).toArray();
    const QCborArray columns = table.value(QStringLiteral("columns")).toArray();

    // 요약: 행 수(통계), 컬럼·인덱스 수, 코멘트
    QStringList summary;
    for (const auto &[label, value] : tableStats) {
        if (label == QLatin1String("Rows (statistics)"))
            summary << tr("%1 rows (statistics)").arg(value);
        else if (label == QLatin1String("Last analyzed"))
            summary << tr("analyzed %1").arg(value);
    }
    summary << tr("%n column(s)", nullptr, int(columns.size())) << tr("%n index(es)", nullptr, int(indexes.size()));
    const QString remarks = table.value(QStringLiteral("remarks")).toString();
    if (!remarks.isEmpty())
        summary << remarks;
    m_summary->setText(summary.join(QStringLiteral("  ·  ")));
    m_summary->setProperty("error", false);
    m_summary->style()->unpolish(m_summary);
    m_summary->style()->polish(m_summary);

    // 인덱스: 이름, 컬럼, 고유 + 통계 (DB마다 있는 것만, 처음 나온 순서대로)
    QStringList statLabels;
    QHash<QString, QHash<QString, QString>> statsByIndex;
    for (auto it = indexStats.constBegin(); it != indexStats.constEnd(); ++it) {
        for (const auto &[label, value] : pairs(it.value())) {
            if (label == QLatin1String("Uniqueness") || label == QLatin1String("Unique") || label == QLatin1String("Non-unique"))
                continue; // 고유 컬럼과 같은 내용
            if (!statLabels.contains(label))
                statLabels << label;
            statsByIndex[it.key().toString()].insert(label, value);
        }
    }
    // 정의(PostgreSQL)는 길어서 맨 끝에
    if (statLabels.removeAll(QStringLiteral("Definition")) > 0)
        statLabels << QStringLiteral("Definition");

    QStringList indexNames;
    QHash<QString, QPair<QStringList, bool>> indexInfo; // 이름 → (컬럼, 고유)
    for (const QCborValue &value : indexes) {
        const QCborMap index = value.toMap();
        const QString name = index.value(QStringLiteral("name")).toString();
        indexNames << name;
        indexInfo.insert(name, {strings(index.value(QStringLiteral("columns"))), index.value(QStringLiteral("unique")).toBool()});
    }
    for (const QString &name : statsByIndex.keys()) {
        if (!indexNames.contains(name))
            indexNames << name; // JDBC 목록에 없는 인덱스 (LOB 인덱스 등)
    }

    QStringList headers{tr("Index"), tr("Columns"), tr("Unique")};
    headers += statLabels;
    m_indexes->setColumnCount(int(headers.size()));
    m_indexes->setHorizontalHeaderLabels(headers);
    m_indexes->setRowCount(int(indexNames.size()));
    QStringList usedColumns;
    int usedRow = -1;
    for (int r = 0; r < indexNames.size(); ++r) {
        const QString &name = indexNames[r];
        const auto info = indexInfo.value(name);
        const bool used = !m_index.isEmpty() && name.compare(m_index, Qt::CaseInsensitive) == 0;
        if (used) {
            usedRow = r;
            for (const QString &column : info.first)
                usedColumns << columnOf(column);
        }
        QStringList cells{name, info.first.join(QStringLiteral(", ")), info.second ? tr("Unique") : QString()};
        for (const QString &label : std::as_const(statLabels))
            cells << statsByIndex.value(name).value(label);
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            item->setToolTip(cells[c]);
            if (used) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
                item->setBackground(Theme::currentMatchColor());
                if (c == 0)
                    item->setToolTip(tr("The plan uses this index"));
            }
            m_indexes->setItem(r, c, item);
        }
    }
    fit(m_indexes);
    if (usedRow >= 0)
        m_indexes->scrollToItem(m_indexes->item(usedRow, 0));

    // 컬럼: 쓴 인덱스의 컬럼은 굵게
    const QStringList columnHeaders{tr("#"), tr("Column"), tr("Type"), tr("Null"), tr("Default"), tr("Key"), tr("Comment")};
    m_columns->setColumnCount(int(columnHeaders.size()));
    m_columns->setHorizontalHeaderLabels(columnHeaders);
    m_columns->setRowCount(int(columns.size()));
    for (int r = 0; r < columns.size(); ++r) {
        const QCborMap column = columns[r].toMap();
        const QString name = column.value(QStringLiteral("name")).toString();
        const bool inIndex = usedColumns.contains(name, Qt::CaseInsensitive);
        QString key = column.value(QStringLiteral("primaryKey")).toBool() ? QStringLiteral("PK") : QString();
        if (inIndex)
            key = key.isEmpty() ? tr("index") : key + tr(", index");
        const QStringList cells{QString::number(r + 1), name, column.value(QStringLiteral("type")).toString(),
                                column.value(QStringLiteral("nullable")).toBool() ? QString() : QStringLiteral("NOT NULL"),
                                column.value(QStringLiteral("default")).toString(), key,
                                column.value(QStringLiteral("remarks")).toString()};
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            if (inIndex) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
            m_columns->setItem(r, c, item);
        }
    }
    fit(m_columns);

    // 테이블 통계
    m_stats->setColumnCount(2);
    m_stats->setHorizontalHeaderLabels({tr("Statistic"), tr("Value")});
    m_stats->setRowCount(int(tableStats.size()));
    for (int r = 0; r < tableStats.size(); ++r) {
        m_stats->setItem(r, 0, new QTableWidgetItem(tableStats[r].first));
        m_stats->setItem(r, 1, new QTableWidgetItem(tableStats[r].second));
    }
    fit(m_stats);
    m_pages->setTabText(m_pages->indexOf(m_stats),
                        tableStats.isEmpty() ? tr("Table Statistics (none)") : tr("Table Statistics"));
    m_detailsButton->setEnabled(!m_table.isEmpty());
}
