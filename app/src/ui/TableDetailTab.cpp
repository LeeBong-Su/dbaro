#include "TableDetailTab.h"

#include "BusyIndicator.h"
#include "MetadataService.h"
#include "SqlHighlighter.h"
#include "Theme.h"

#include <QCborArray>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, int(headers.size()), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    return table;
}

QString join(const QCborValue &list)
{
    QStringList items;
    for (const QCborValue &value : list.toArray())
        items << value.toString();
    return items.join(QStringLiteral(", "));
}
} // namespace

TableDetailTab::TableDetailTab(MetadataService *metadata, const QString &connectionId, const QString &schema,
                               const QString &name, bool schemaQuoted, bool nameQuoted, SqlDialect dialect, QWidget *parent,
                               const QString &currentSchema)
    : QWidget(parent)
    , m_metadata(metadata)
    , m_connectionId(connectionId)
    , m_schema(schema)
    , m_name(name)
    , m_currentSchema(currentSchema)
    , m_schemaQuoted(schemaQuoted)
    , m_nameQuoted(nameQuoted)
    , m_dialect(dialect)
{
    setObjectName(QStringLiteral("tableDetailTab"));

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("tableDetailTitle"));
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary = new QLabel(tr("Loading..."), this);
    m_summary->setObjectName(QStringLiteral("tableDetailSummary"));
    m_summary->setWordWrap(true);
    auto *select = new QPushButton(tr("Select Rows"), this);
    auto *import = new QPushButton(tr("Import Data..."), this);
    auto *reload = new QPushButton(tr("Refresh"), this);
    connect(select, &QPushButton::clicked, this, [this] { emit selectRowsRequested(m_schema, m_name); });
    connect(import, &QPushButton::clicked, this, [this] { emit importRequested(m_schema, m_name); });
    connect(reload, &QPushButton::clicked, this, &TableDetailTab::refresh);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_summary);
    header->addLayout(titles, 1);
    header->addWidget(select);
    header->addWidget(import);
    header->addWidget(reload);

    m_columns = makeTable({tr("#"), tr("Name"), tr("Type"), tr("Null?"), tr("Default"), tr("Key"), tr("Comment")}, this);
    m_indexes = makeTable({tr("Name"), tr("Unique"), tr("Columns")}, this);
    m_foreignKeys = makeTable({tr("Name"), tr("Columns"), tr("References")}, this);
    m_referencedBy = makeTable({tr("Table"), tr("Columns"), tr("Refers to"), tr("Constraint")}, this);
    m_ddl = new QPlainTextEdit(this);
    m_ddl->setReadOnly(true);
    m_ddl->setFrameShape(QFrame::NoFrame);
    m_ddl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *highlighter = new SqlHighlighter(m_ddl->document());
    highlighter->setDialect(dialect);
    highlighter->setDarkTheme(Theme::current() == Theme::Kind::Dark);

    m_pages = new QTabWidget(this);
    m_pages->setDocumentMode(true);
    m_pages->addTab(m_columns, tr("Columns"));
    m_pages->addTab(m_indexes, tr("Indexes"));
    m_pages->addTab(m_foreignKeys, tr("Foreign Keys"));
    m_pages->addTab(m_referencedBy, tr("Referenced By"));
    m_pages->addTab(m_ddl, tr("DDL"));
    connect(m_pages, &QTabWidget::currentChanged, this, [this](int index) {
        if (m_pages->widget(index) == m_ddl && !m_ddlLoaded)
            loadDdl(); // DDL은 처음 볼 때 만든다
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 0);
    layout->addLayout(header);
    layout->addWidget(m_pages, 1);
    m_busy = new BusyOverlay(m_pages, this);

    m_title->setText(m_schema.isEmpty() ? m_name : m_schema + u'.' + m_name);
    refresh();
}

void TableDetailTab::refresh()
{
    if (!m_metadata)
        return;
    QCborMap params;
    if (!m_schema.isEmpty())
        params.insert(QStringLiteral("schema"), m_schema);
    params.insert(QStringLiteral("name"), m_name);
    params.insert(QStringLiteral("schemaQuoted"), m_schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), m_nameQuoted);
    if (m_schema.isEmpty() && !m_currentSchema.isEmpty())
        params.insert(QStringLiteral("currentSchema"), m_currentSchema);
    m_summary->setText(tr("Loading..."));
    m_busy->start(tr("Reading the table"), m_schema.isEmpty() ? m_name : m_schema + u'.' + m_name);
    QPointer<TableDetailTab> self(this);
    m_metadata->request(
        m_connectionId, QStringLiteral("tableInfo"), params,
        [self](const QCborMap &table, const QString &error) {
            if (!self)
                return;
            self->m_busy->stop();
            if (!error.isEmpty()) {
                self->m_error = error;
                self->m_summary->setText(tr("Could not read the table: %1").arg(error));
                return;
            }
            self->showTable(table);
        },
        true, true);
    m_ddlLoaded = false;
    if (m_pages->currentWidget() == m_ddl)
        loadDdl();
}

void TableDetailTab::showTable(const QCborMap &table)
{
    // 이제 DB에 저장된 실제 이름을 안다 (이후 요청은 그대로 쓴다)
    m_schema = table.value(QStringLiteral("schema")).toString();
    m_name = table.value(QStringLiteral("name")).toString();
    m_schemaQuoted = m_nameQuoted = true;
    m_loaded = true;
    m_error.clear();
    m_title->setText(QStringLiteral("<b>%1</b>.%2  <span style='color:gray'>%3</span>")
                         .arg(m_schema.toHtmlEscaped(), m_name.toHtmlEscaped(),
                              table.value(QStringLiteral("type")).toString().toHtmlEscaped()));

    const QCborArray columns = table.value(QStringLiteral("columns")).toArray();
    const QCborArray indexes = table.value(QStringLiteral("indexes")).toArray();
    const QCborArray foreignKeys = table.value(QStringLiteral("foreignKeys")).toArray();
    const QCborArray referencedBy = table.value(QStringLiteral("referencedBy")).toArray();
    QString summary = tr("%n column(s)", nullptr, int(columns.size()));
    if (!table.value(QStringLiteral("primaryKey")).toArray().isEmpty())
        summary += tr(" · primary key (%1)").arg(join(table.value(QStringLiteral("primaryKey"))));
    summary += tr(" · %n index(es)", nullptr, int(indexes.size()));
    summary += tr(" · %n foreign key(s)", nullptr, int(foreignKeys.size()));
    const QString remarks = table.value(QStringLiteral("remarks")).toString();
    if (!remarks.isEmpty())
        summary += QStringLiteral(" · ") + remarks;
    m_summary->setText(summary);

    m_columns->setRowCount(int(columns.size()));
    for (int r = 0; r < columns.size(); ++r) {
        const QCborMap column = columns[r].toMap();
        const QStringList cells{QString::number(r + 1),
                                column.value(QStringLiteral("name")).toString(),
                                column.value(QStringLiteral("type")).toString(),
                                column.value(QStringLiteral("nullable")).toBool() ? QString() : QStringLiteral("NOT NULL"),
                                column.value(QStringLiteral("default")).toString(),
                                column.value(QStringLiteral("primaryKey")).toBool() ? QStringLiteral("PK") : QString(),
                                column.value(QStringLiteral("remarks")).toString()};
        for (int c = 0; c < cells.size(); ++c)
            m_columns->setItem(r, c, new QTableWidgetItem(cells[c]));
    }
    m_indexes->setRowCount(int(indexes.size()));
    for (int r = 0; r < indexes.size(); ++r) {
        const QCborMap index = indexes[r].toMap();
        m_indexes->setItem(r, 0, new QTableWidgetItem(index.value(QStringLiteral("name")).toString()));
        m_indexes->setItem(r, 1, new QTableWidgetItem(index.value(QStringLiteral("unique")).toBool() ? tr("Unique") : QString()));
        m_indexes->setItem(r, 2, new QTableWidgetItem(join(index.value(QStringLiteral("columns")))));
    }
    m_foreignKeys->setRowCount(int(foreignKeys.size()));
    for (int r = 0; r < foreignKeys.size(); ++r) {
        const QCborMap key = foreignKeys[r].toMap();
        const QString target = QStringLiteral("%1.%2 (%3)").arg(key.value(QStringLiteral("refSchema")).toString(),
                                                                key.value(QStringLiteral("refTable")).toString(),
                                                                join(key.value(QStringLiteral("refColumns"))));
        m_foreignKeys->setItem(r, 0, new QTableWidgetItem(key.value(QStringLiteral("name")).toString()));
        m_foreignKeys->setItem(r, 1, new QTableWidgetItem(join(key.value(QStringLiteral("columns")))));
        m_foreignKeys->setItem(r, 2, new QTableWidgetItem(target));
    }
    m_referencedBy->setRowCount(int(referencedBy.size()));
    for (int r = 0; r < referencedBy.size(); ++r) {
        const QCborMap key = referencedBy[r].toMap();
        m_referencedBy->setItem(r, 0, new QTableWidgetItem(QStringLiteral("%1.%2").arg(key.value(QStringLiteral("schema")).toString(),
                                                                                       key.value(QStringLiteral("table")).toString())));
        m_referencedBy->setItem(r, 1, new QTableWidgetItem(join(key.value(QStringLiteral("columns")))));
        m_referencedBy->setItem(r, 2, new QTableWidgetItem(join(key.value(QStringLiteral("refColumns")))));
        m_referencedBy->setItem(r, 3, new QTableWidgetItem(key.value(QStringLiteral("name")).toString()));
    }
    for (QTableWidget *table : {m_columns, m_indexes, m_foreignKeys, m_referencedBy}) {
        table->resizeColumnsToContents();
        for (int c = 0; c + 1 < table->columnCount(); ++c)
            table->setColumnWidth(c, qMin(table->columnWidth(c) + 16, 360));
    }
    m_pages->setTabText(1, tr("Indexes (%1)").arg(indexes.size()));
    m_pages->setTabText(2, tr("Foreign Keys (%1)").arg(foreignKeys.size()));
    m_pages->setTabText(3, tr("Referenced By (%1)").arg(referencedBy.size()));
    emit loaded(m_name);
}

void TableDetailTab::loadDdl()
{
    if (!m_metadata)
        return;
    m_ddlLoaded = true;
    m_ddl->setPlainText(tr("-- Generating DDL ..."));
    QCborMap params;
    if (!m_schema.isEmpty())
        params.insert(QStringLiteral("schema"), m_schema);
    params.insert(QStringLiteral("name"), m_name);
    params.insert(QStringLiteral("schemaQuoted"), m_schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), m_nameQuoted);
    if (m_schema.isEmpty() && !m_currentSchema.isEmpty())
        params.insert(QStringLiteral("currentSchema"), m_currentSchema);
    QPointer<TableDetailTab> self(this);
    m_metadata->request(
        m_connectionId, QStringLiteral("ddl"), params,
        [self](const QCborMap &result, const QString &error) {
            if (!self)
                return;
            self->m_ddl->setPlainText(error.isEmpty() ? result.value(QStringLiteral("script")).toString()
                                                      : tr("-- Could not generate the DDL: %1").arg(error));
        },
        true, true);
}
