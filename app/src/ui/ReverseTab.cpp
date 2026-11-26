#include "ReverseTab.h"

#include "BusyIndicator.h"
#include "QuerySession.h"
#include "SqlHighlighter.h"
#include "Theme.h"
#include "core/SqlDialect.h"

#include <QApplication>
#include <QCborArray>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const auto kLastDirKey = QStringLiteral("reverse/lastDirectory");

QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, int(headers.size()), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 8);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    return table;
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QStringList strings(const QCborValue &value)
{
    QStringList list;
    for (const QCborValue &item : value.toArray())
        list << item.toString();
    return list;
}

QString lastDirectory()
{
    const QString saved = QSettings().value(kLastDirKey).toString();
    return saved.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : saved;
}

void rememberDirectory(const QString &file)
{
    QSettings().setValue(kLastDirKey, QFileInfo(file).absolutePath());
}

QTableWidgetItem *numberItem(qint64 value)
{
    auto *item = new QTableWidgetItem;
    if (value >= 0)
        item->setData(Qt::DisplayRole, value); // 숫자로 정렬
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

} // namespace

ReverseTab::ReverseTab(EngineClient *engine, const ConnectionProfile &profile, const QString &schema, const QStringList &tables,
                       bool includeViews, QWidget *parent)
    : QWidget(parent)
    , m_profile(profile)
    , m_schema(schema)
    , m_only(tables)
    , m_includeViews(includeViews)
{
    setObjectName(QStringLiteral("reverseTab"));
    m_session = new QuerySession(engine, this);
    connect(m_session, &QuerySession::message, this, [this](const QString &line, bool isError) {
        if (isError)
            setStatus(line, true);
    });

    m_title = new QLabel(tr("Schema %1").arg(schema), this);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary = new QLabel(tr("Reading..."), this);
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *refresh = new QPushButton(tr("Refresh"), this);
    refresh->setToolTip(tr("Read the schema again"));
    connect(refresh, &QPushButton::clicked, this, &ReverseTab::reload);

    auto *header = new QHBoxLayout;
    header->addWidget(m_title);
    header->addSpacing(12);
    header->addWidget(m_summary);
    header->addStretch();
    header->addWidget(m_status);
    header->addSpacing(8);
    header->addWidget(refresh);

    m_pages = new QTabWidget(this);
    m_pages->setDocumentMode(true);
    m_pages->addTab(buildDiagramPage(), tr("ERD"));
    m_pages->addTab(buildTablesPage(), tr("Tables"));
    m_pages->addTab(buildDdlPage(), tr("DDL"));
    connect(m_pages, &QTabWidget::currentChanged, this, [this] {
        if (m_pages->currentIndex() == int(Page::Ddl) && m_loaded && !m_ddlLoaded)
            generateDdl(); // DDL은 처음 볼 때 만든다
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 0);
    layout->addLayout(header);
    layout->addWidget(m_pages, 1);
}

ReverseTab::~ReverseTab() = default;

QWidget *ReverseTab::buildDiagramPage()
{
    auto *page = new QWidget(this);
    m_find = new QLineEdit(page);
    m_find->setPlaceholderText(tr("Find a table (Enter)"));
    m_find->setClearButtonEnabled(true);
    m_find->setMaximumWidth(260);
    connect(m_find, &QLineEdit::returnPressed, this, [this] {
        const QString name = m_find->text().trimmed();
        if (!name.isEmpty() && !m_erd->focusTable(name))
            setStatus(tr("No table named %1 in the diagram.").arg(name), true);
    });

    m_detail = new QComboBox(page);
    m_detail->addItem(tr("All columns"), int(ErdView::Detail::AllColumns));
    m_detail->addItem(tr("Keys only"), int(ErdView::Detail::KeysOnly));
    m_detail->addItem(tr("Table names only"), int(ErdView::Detail::NamesOnly));
    m_detail->setToolTip(tr("What each table box shows. Changing it arranges the diagram again."));

    auto *arrange = new QPushButton(tr("Arrange"), page);
    arrange->setToolTip(tr("Place the tables again: referenced tables on the left, unrelated tables below"));
    auto *fit = new QPushButton(tr("Fit"), page);
    fit->setToolTip(tr("Show the whole diagram"));
    auto *zoomOut = new QToolButton(page);
    zoomOut->setText(QStringLiteral("−"));
    zoomOut->setToolTip(tr("Zoom out (Ctrl+wheel)"));
    auto *zoomIn = new QToolButton(page);
    zoomIn->setText(QStringLiteral("+"));
    zoomIn->setToolTip(tr("Zoom in (Ctrl+wheel)"));
    m_zoom = new QLabel(QStringLiteral("100%"), page);
    m_zoom->setMinimumWidth(44);
    m_zoom->setAlignment(Qt::AlignCenter);
    auto *exportImage = new QPushButton(tr("Export Image..."), page);
    exportImage->setToolTip(tr("Save the diagram as PNG or SVG (white background)"));

    m_erd = new ErdView(page);
    connect(m_detail, &QComboBox::activated, this, [this] {
        m_erd->setDetail(ErdView::Detail(m_detail->currentData().toInt()));
        m_erd->fitAll();
    });
    connect(arrange, &QPushButton::clicked, this, [this] {
        m_erd->arrange();
        m_erd->fitAll();
    });
    connect(fit, &QPushButton::clicked, m_erd, &ErdView::fitAll);
    connect(zoomOut, &QToolButton::clicked, this, [this] { m_erd->zoomBy(1 / 1.25); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { m_erd->zoomBy(1.25); });
    connect(m_erd, &ErdView::zoomChanged, this, [this](qreal zoom) { m_zoom->setText(QStringLiteral("%1%").arg(qRound(zoom * 100))); });
    connect(m_erd, &ErdView::tableActivated, this, [this](const QString &name) {
        showPage(Page::Tables);
        selectTable(name);
    });
    connect(exportImage, &QPushButton::clicked, this, &ReverseTab::exportImage);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 8, 0, 6);
    bar->addWidget(m_find);
    bar->addWidget(m_detail);
    bar->addWidget(arrange);
    bar->addWidget(fit);
    bar->addSpacing(8);
    bar->addWidget(zoomOut);
    bar->addWidget(m_zoom);
    bar->addWidget(zoomIn);
    bar->addStretch();
    auto *hint = new QLabel(tr("Drag tables to move them · drag the background to pan · double-click a table for its definition"), page);
    hint->setForegroundRole(QPalette::PlaceholderText);
    bar->addWidget(hint);
    bar->addSpacing(8);
    bar->addWidget(exportImage);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(bar);
    layout->addWidget(m_erd, 1);
    m_erdBusy = new BusyOverlay(m_erd, this);
    return page;
}

QWidget *ReverseTab::buildTablesPage()
{
    auto *page = new QWidget(this);
    m_tableFilter = new QLineEdit(page);
    m_tableFilter->setPlaceholderText(tr("Filter tables"));
    m_tableFilter->setClearButtonEnabled(true);
    connect(m_tableFilter, &QLineEdit::textChanged, this, &ReverseTab::applyTableFilter);
    m_tableList = makeTable({tr("Name"), tr("Type"), tr("Rows"), tr("Comment")}, page);
    m_tableList->setObjectName(QStringLiteral("reverseTableList"));
    m_tableList->setColumnWidth(0, 170);
    m_tableList->setColumnWidth(1, 70);
    m_tableList->setColumnWidth(2, 80);
    m_tableList->setSortingEnabled(true);
    connect(m_tableList, &QTableWidget::itemSelectionChanged, this, [this] {
        const QList<QTableWidgetItem *> selected = m_tableList->selectedItems();
        if (!selected.isEmpty())
            showTableDetails(m_tableList->item(selected.first()->row(), 0)->text());
    });

    auto *left = new QWidget(page);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 8, 6, 0);
    leftLayout->addWidget(m_tableFilter);
    leftLayout->addWidget(m_tableList, 1);

    m_tableTitle = new QLabel(page);
    m_tableTitle->setWordWrap(true);
    m_tableTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *showInErd = new QPushButton(tr("Show in ERD"), page);
    connect(showInErd, &QPushButton::clicked, this, [this] {
        const QList<QTableWidgetItem *> selected = m_tableList->selectedItems();
        if (selected.isEmpty())
            return;
        showPage(Page::Diagram);
        m_erd->focusTable(m_tableList->item(selected.first()->row(), 0)->text());
    });
    auto *exportDefinition = new QPushButton(tr("Export Table Definition..."), page);
    exportDefinition->setToolTip(tr("All tables' columns in one sheet: Excel, CSV or Markdown"));
    connect(exportDefinition, &QPushButton::clicked, this, &ReverseTab::exportDefinition);

    m_columns = makeTable({tr("No"), tr("Column"), tr("Type"), tr("Null"), tr("Default"), tr("Key"), tr("References"), tr("Comment")}, page);
    m_columns->setObjectName(QStringLiteral("reverseColumns"));
    const int widths[] = {40, 170, 140, 50, 120, 60, 190};
    for (int column = 0; column < 7; ++column)
        m_columns->setColumnWidth(column, widths[column]);
    m_indexes = makeTable({tr("Index"), tr("Unique"), tr("Columns")}, page);
    m_indexes->setColumnWidth(0, 220);
    m_indexes->setColumnWidth(1, 60);
    m_foreignKeys = makeTable({tr("Foreign key"), tr("Columns"), tr("References")}, page);
    m_foreignKeys->setColumnWidth(0, 220);
    m_foreignKeys->setColumnWidth(1, 200);

    auto *right = new QWidget(page);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(6, 8, 0, 0);
    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(m_tableTitle, 1);
    titleRow->addWidget(showInErd);
    titleRow->addWidget(exportDefinition);
    rightLayout->addLayout(titleRow);
    auto *details = new QSplitter(Qt::Vertical, right);
    details->addWidget(m_columns);
    auto *keys = new QWidget(details);
    auto *keysLayout = new QHBoxLayout(keys);
    keysLayout->setContentsMargins(0, 4, 0, 0);
    auto *indexBox = new QVBoxLayout;
    indexBox->addWidget(sectionTitle(tr("Indexes"), keys));
    indexBox->addWidget(m_indexes, 1);
    auto *fkBox = new QVBoxLayout;
    fkBox->addWidget(sectionTitle(tr("Foreign keys"), keys));
    fkBox->addWidget(m_foreignKeys, 1);
    keysLayout->addLayout(indexBox, 1);
    keysLayout->addLayout(fkBox, 1);
    details->addWidget(keys);
    details->setStretchFactor(0, 3);
    details->setStretchFactor(1, 1);
    rightLayout->addWidget(details, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({380, 1100});
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);
    return page;
}

QWidget *ReverseTab::buildDdlPage()
{
    auto *page = new QWidget(this);
    m_ddlInfo = new QLabel(tr("The DDL script is made when you open this page."), page);
    m_ddlInfo->setWordWrap(true);
    auto *regenerate = new QPushButton(tr("Generate Again"), page);
    connect(regenerate, &QPushButton::clicked, this, &ReverseTab::generateDdl);
    m_saveDdl = new QPushButton(tr("Save As..."), page);
    connect(m_saveDdl, &QPushButton::clicked, this, &ReverseTab::saveDdl);
    m_openDdl = new QPushButton(tr("Open in Query Tab"), page);
    connect(m_openDdl, &QPushButton::clicked, this, [this] {
        emit openSqlRequested(m_ddl->toPlainText(), tr("%1 DDL").arg(m_schema));
    });
    auto *copy = new QPushButton(tr("Copy"), page);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_ddl->toPlainText()); });
    m_saveDdl->setEnabled(false);
    m_openDdl->setEnabled(false);

    m_ddl = new QPlainTextEdit(page);
    m_ddl->setObjectName(QStringLiteral("reverseDdl"));
    m_ddl->setReadOnly(true);
    m_ddl->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_ddl->setFrameShape(QFrame::NoFrame);
    m_ddl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *highlighter = new SqlHighlighter(m_ddl->document());
    highlighter->setDialect(sqlDialectFor(m_profile.type));
    highlighter->setDarkTheme(Theme::current() == Theme::Kind::Dark);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 8, 0, 6);
    bar->addWidget(m_ddlInfo, 1);
    bar->addWidget(regenerate);
    bar->addWidget(copy);
    bar->addWidget(m_openDdl);
    bar->addWidget(m_saveDdl);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(bar);
    layout->addWidget(m_ddl, 1);
    m_ddlBusy = new BusyOverlay(m_ddl, this);
    connect(m_ddlBusy, &BusyOverlay::cancelRequested, this, [this] { m_session->interrupt(); });
    return page;
}

void ReverseTab::start(const ConnectSpec &spec)
{
    m_spec = spec;
    reload();
}

void ReverseTab::showPage(Page page)
{
    m_pages->setCurrentIndex(int(page));
}

QString ReverseTab::statusText() const
{
    return m_status->text();
}

void ReverseTab::setStatus(const QString &text, bool error)
{
    m_status->setText(text.section(u'\n', 0, 0));
    m_status->setToolTip(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: %1;").arg(Theme::warningTextColor().name()) : QString());
}

void ReverseTab::connectThen(std::function<void()> work, std::function<void()> failed)
{
    if (m_session->isConnected()) {
        work();
        return;
    }
    auto finish = [work, failed](bool ok) {
        if (ok)
            work();
        else
            failed();
    };
    if (m_session->isConnecting()) {
        m_session->whenConnected(finish);
        return;
    }
    QPointer<ReverseTab> self(this);
    m_session->connectTo(m_spec, m_profile.id, [self, finish](bool ok) {
        if (self)
            finish(ok);
    });
}

void ReverseTab::reload()
{
    if (m_loading)
        return;
    m_loading = true;
    m_erdBusy->start(tr("Reading the schema"), m_only.isEmpty() ? m_schema : tr("%1 (%n table(s))", nullptr, int(m_only.size())).arg(m_schema));
    QPointer<ReverseTab> self(this);
    auto failed = [self] {
        if (!self)
            return;
        self->m_loading = false;
        self->m_erdBusy->stop();
        self->m_summary->setText(tr("Could not connect."));
    };
    connectThen([self, failed] {
        if (!self)
            return;
        QCborMap params;
        params.insert(QStringLiteral("schema"), self->m_schema);
        params.insert(QStringLiteral("tables"), QCborArray::fromStringList(self->m_only));
        params.insert(QStringLiteral("includeViews"), self->m_includeViews);
        self->m_session->call(QStringLiteral("reverseModel"), params, [self](const QCborMap &result, const EngineError &error) {
            if (!self)
                return;
            self->m_loading = false;
            self->m_erdBusy->stop();
            if (error.isError()) {
                self->m_summary->setText(tr("Could not read the schema."));
                self->setStatus(error.message, true);
                return;
            }
            self->showModel(result);
        });
    }, failed);
}

void ReverseTab::showModel(const QCborMap &result)
{
    m_model = result;
    m_loaded = true;
    m_ddlLoaded = false;
    const QCborArray tables = result.value(QStringLiteral("tables")).toArray();
    m_tableIndex.clear();
    for (int i = 0; i < tables.size(); ++i)
        m_tableIndex.insert(tables.at(i).toMap().value(QStringLiteral("name")).toString(), i);

    const QList<ErdTable> erdTables = ErdTable::fromModel(tables);
    m_erd->setModel(m_schema, erdTables);
    QTimer::singleShot(0, m_erd, &ErdView::fitAll);
    auto *completer = new QCompleter(m_erd->tableNames(), m_find);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    m_find->setCompleter(completer);

    // 테이블 목록
    m_tableList->setSortingEnabled(false);
    m_tableList->setRowCount(0);
    m_tableList->setRowCount(int(tables.size()));
    int views = 0;
    for (int row = 0; row < tables.size(); ++row) {
        const QCborMap t = tables.at(row).toMap();
        const QString type = t.value(QStringLiteral("type")).toString();
        views += type.endsWith(QLatin1String("VIEW")) ? 1 : 0;
        m_tableList->setItem(row, 0, new QTableWidgetItem(t.value(QStringLiteral("name")).toString()));
        m_tableList->setItem(row, 1, new QTableWidgetItem(type));
        m_tableList->setItem(row, 2, numberItem(t.value(QStringLiteral("rows")).isInteger() ? t.value(QStringLiteral("rows")).toInteger() : -1));
        m_tableList->setItem(row, 3, new QTableWidgetItem(t.value(QStringLiteral("remarks")).toString().simplified()));
    }
    m_tableList->setSortingEnabled(true);
    m_tableList->sortByColumn(0, Qt::AscendingOrder);
    applyTableFilter();
    if (m_tableList->rowCount() > 0)
        m_tableList->selectRow(0);

    const int tableCount = int(tables.size()) - views;
    QString summary = tr("%n table(s)", nullptr, tableCount);
    if (views > 0)
        summary += tr(" · %n view(s)", nullptr, views);
    summary += tr(" · %n relationship(s)", nullptr, m_erd->relationCount());
    if (result.value(QStringLiteral("truncated")).toBool())
        summary += tr(" · only the first %1 are shown").arg(tables.size());
    m_summary->setText(summary);
    setStatus(tr("Read at %1 · %2 ms")
                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")))
                  .arg(result.value(QStringLiteral("elapsedMs")).toInteger()));

    m_ddl->clear();
    m_ddlInfo->setText(tr("The DDL script is made when you open this page."));
    if (m_pages->currentIndex() == int(Page::Ddl))
        generateDdl();
}

QCborMap ReverseTab::tableModel(const QString &name) const
{
    const int index = m_tableIndex.value(name, -1);
    return index < 0 ? QCborMap() : m_model.value(QStringLiteral("tables")).toArray().at(index).toMap();
}

void ReverseTab::selectTable(const QString &name)
{
    for (int row = 0; row < m_tableList->rowCount(); ++row) {
        if (m_tableList->item(row, 0)->text() == name) {
            if (m_tableList->isRowHidden(row))
                m_tableFilter->clear();
            m_tableList->selectRow(row);
            m_tableList->scrollToItem(m_tableList->item(row, 0));
            return;
        }
    }
}

void ReverseTab::applyTableFilter()
{
    const QString filter = m_tableFilter->text().trimmed();
    for (int row = 0; row < m_tableList->rowCount(); ++row) {
        const bool visible = filter.isEmpty() || m_tableList->item(row, 0)->text().contains(filter, Qt::CaseInsensitive)
                             || m_tableList->item(row, 3)->text().contains(filter, Qt::CaseInsensitive);
        m_tableList->setRowHidden(row, !visible);
    }
}

void ReverseTab::showTableDetails(const QString &name)
{
    const QCborMap t = tableModel(name);
    QString title = QStringLiteral("<b>%1.%2</b> &nbsp; %3").arg(m_schema.toHtmlEscaped(), name.toHtmlEscaped(),
                                                                 t.value(QStringLiteral("type")).toString());
    const QString remarks = t.value(QStringLiteral("remarks")).toString();
    if (!remarks.isEmpty())
        title += QStringLiteral("<br>") + remarks.toHtmlEscaped();
    m_tableTitle->setText(title);

    QHash<QString, QString> references;
    for (const QCborValue &value : t.value(QStringLiteral("foreignKeys")).toArray()) {
        const QCborMap fk = value.toMap();
        const QStringList columns = strings(fk.value(QStringLiteral("columns")));
        const QStringList refColumns = strings(fk.value(QStringLiteral("refColumns")));
        const QString refSchema = fk.value(QStringLiteral("refSchema")).toString();
        const QString refTable = (refSchema.isEmpty() || refSchema == m_schema ? QString() : refSchema + u'.')
                                 + fk.value(QStringLiteral("refTable")).toString();
        for (int i = 0; i < columns.size(); ++i)
            references.insert(columns.at(i), refTable + u'.' + refColumns.value(i));
    }
    QSet<QString> unique;
    for (const QCborValue &value : t.value(QStringLiteral("uniqueKeys")).toArray()) {
        for (const QString &column : strings(value.toMap().value(QStringLiteral("columns"))))
            unique.insert(column);
    }

    const QCborArray columns = t.value(QStringLiteral("columns")).toArray();
    m_columns->setRowCount(int(columns.size()));
    for (int row = 0; row < columns.size(); ++row) {
        const QCborMap c = columns.at(row).toMap();
        const QString column = c.value(QStringLiteral("name")).toString();
        QStringList keys;
        if (c.value(QStringLiteral("primaryKey")).toBool())
            keys << QStringLiteral("PK");
        if (c.value(QStringLiteral("foreignKey")).toBool())
            keys << QStringLiteral("FK");
        if (unique.contains(column))
            keys << QStringLiteral("UK");
        auto *no = new QTableWidgetItem(QString::number(row + 1));
        no->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *nameItem = new QTableWidgetItem(column);
        if (c.value(QStringLiteral("primaryKey")).toBool()) {
            QFont font = nameItem->font();
            font.setBold(true);
            nameItem->setFont(font);
        }
        m_columns->setItem(row, 0, no);
        m_columns->setItem(row, 1, nameItem);
        m_columns->setItem(row, 2, new QTableWidgetItem(c.value(QStringLiteral("type")).toString()));
        m_columns->setItem(row, 3, new QTableWidgetItem(c.value(QStringLiteral("nullable")).toBool() ? QStringLiteral("Y") : QStringLiteral("N")));
        m_columns->setItem(row, 4, new QTableWidgetItem(c.value(QStringLiteral("default")).toString()));
        m_columns->setItem(row, 5, new QTableWidgetItem(keys.join(QStringLiteral(", "))));
        m_columns->setItem(row, 6, new QTableWidgetItem(references.value(column)));
        m_columns->setItem(row, 7, new QTableWidgetItem(c.value(QStringLiteral("remarks")).toString()));
    }

    const QCborArray indexes = t.value(QStringLiteral("indexes")).toArray();
    m_indexes->setRowCount(int(indexes.size()));
    for (int row = 0; row < indexes.size(); ++row) {
        const QCborMap index = indexes.at(row).toMap();
        m_indexes->setItem(row, 0, new QTableWidgetItem(index.value(QStringLiteral("name")).toString()));
        m_indexes->setItem(row, 1, new QTableWidgetItem(index.value(QStringLiteral("unique")).toBool() ? QStringLiteral("Y") : QString()));
        m_indexes->setItem(row, 2, new QTableWidgetItem(strings(index.value(QStringLiteral("columns"))).join(QStringLiteral(", "))));
    }
    const QCborArray fks = t.value(QStringLiteral("foreignKeys")).toArray();
    m_foreignKeys->setRowCount(int(fks.size()));
    for (int row = 0; row < fks.size(); ++row) {
        const QCborMap fk = fks.at(row).toMap();
        m_foreignKeys->setItem(row, 0, new QTableWidgetItem(fk.value(QStringLiteral("name")).toString()));
        m_foreignKeys->setItem(row, 1, new QTableWidgetItem(strings(fk.value(QStringLiteral("columns"))).join(QStringLiteral(", "))));
        m_foreignKeys->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(
                                           fk.value(QStringLiteral("refTable")).toString(),
                                           strings(fk.value(QStringLiteral("refColumns"))).join(QStringLiteral(", ")))));
    }
}

QByteArray ReverseTab::tableDefinition(ExportOptions::Format format) const
{
    const QList<ExportColumn> columns = {
        {tr("Table"), QStringLiteral("VARCHAR"), false},   {tr("Table Comment"), QStringLiteral("VARCHAR"), false},
        {tr("No"), QStringLiteral("INTEGER"), true},       {tr("Column"), QStringLiteral("VARCHAR"), false},
        {tr("Type"), QStringLiteral("VARCHAR"), false},    {tr("Null"), QStringLiteral("VARCHAR"), false},
        {tr("Default"), QStringLiteral("VARCHAR"), false}, {tr("Key"), QStringLiteral("VARCHAR"), false},
        {tr("References"), QStringLiteral("VARCHAR"), false}, {tr("Comment"), QStringLiteral("VARCHAR"), false},
    };
    QList<QCborArray> rows;
    QCborArray tables = m_model.value(QStringLiteral("tables")).toArray();
    QList<QCborMap> sorted;
    for (const QCborValue &value : tables)
        sorted << value.toMap();
    std::sort(sorted.begin(), sorted.end(), [](const QCborMap &a, const QCborMap &b) {
        return a.value(QStringLiteral("name")).toString() < b.value(QStringLiteral("name")).toString();
    });
    for (const QCborMap &t : std::as_const(sorted)) {
        QHash<QString, QString> references;
        for (const QCborValue &value : t.value(QStringLiteral("foreignKeys")).toArray()) {
            const QCborMap fk = value.toMap();
            const QStringList fkColumns = strings(fk.value(QStringLiteral("columns")));
            const QStringList refColumns = strings(fk.value(QStringLiteral("refColumns")));
            for (int i = 0; i < fkColumns.size(); ++i)
                references.insert(fkColumns.at(i), fk.value(QStringLiteral("refTable")).toString() + u'.' + refColumns.value(i));
        }
        QSet<QString> unique;
        for (const QCborValue &value : t.value(QStringLiteral("uniqueKeys")).toArray()) {
            for (const QString &column : strings(value.toMap().value(QStringLiteral("columns"))))
                unique.insert(column);
        }
        int no = 0;
        for (const QCborValue &value : t.value(QStringLiteral("columns")).toArray()) {
            const QCborMap c = value.toMap();
            const QString name = c.value(QStringLiteral("name")).toString();
            QStringList keys;
            if (c.value(QStringLiteral("primaryKey")).toBool())
                keys << QStringLiteral("PK");
            if (c.value(QStringLiteral("foreignKey")).toBool())
                keys << QStringLiteral("FK");
            if (unique.contains(name))
                keys << QStringLiteral("UK");
            QCborArray row;
            row << t.value(QStringLiteral("name")).toString() << t.value(QStringLiteral("remarks")).toString() << ++no << name
                << c.value(QStringLiteral("type")).toString()
                << (c.value(QStringLiteral("nullable")).toBool() ? QStringLiteral("Y") : QStringLiteral("N"))
                << c.value(QStringLiteral("default")).toString() << keys.join(QStringLiteral(", ")) << references.value(name)
                << c.value(QStringLiteral("remarks")).toString();
            rows << row;
        }
    }
    ExportOptions options;
    options.format = format;
    options.header = true;
    options.dialect = sqlDialectFor(m_profile.type);
    return ResultExport::build(columns, rows, options);
}

void ReverseTab::exportDefinition()
{
    if (!m_loaded)
        return;
    const QString excel = tr("Excel (*.xlsx)");
    const QString csv = tr("CSV (*.csv)");
    const QString markdown = tr("Markdown (*.md)");
    QString filter = excel;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Table Definition"),
                                                      QDir(lastDirectory()).filePath(m_schema + QStringLiteral("_tables.xlsx")),
                                                      QStringList{excel, csv, markdown}.join(QStringLiteral(";;")), &filter);
    if (path.isEmpty())
        return;
    rememberDirectory(path);
    const QString suffix = QFileInfo(path).suffix().toLower();
    const ExportOptions::Format format = suffix == u"csv" || filter == csv           ? ExportOptions::Format::Csv
                                         : suffix == u"md" || filter == markdown      ? ExportOptions::Format::Markdown
                                                                                       : ExportOptions::Format::Excel;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(tableDefinition(format)) < 0) {
        setStatus(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()), true);
        return;
    }
    setStatus(tr("Saved the table definition to %1").arg(QDir::toNativeSeparators(path)));
}

void ReverseTab::exportImage()
{
    if (!m_loaded)
        return;
    const QString png = tr("PNG image (*.png)");
    const QString svg = tr("SVG image (*.svg)");
    QString filter = png;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export ERD"), QDir(lastDirectory()).filePath(m_schema + QStringLiteral("_erd.png")),
                                                      png + QStringLiteral(";;") + svg, &filter);
    if (path.isEmpty())
        return;
    rememberDirectory(path);
    const bool asSvg = QFileInfo(path).suffix().compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0 || filter == svg;
    const bool ok = asSvg ? m_erd->exportSvg(path) : m_erd->exportPng(path);
    if (ok)
        setStatus(tr("Saved the ERD to %1").arg(QDir::toNativeSeparators(path)));
    else
        setStatus(tr("Could not save the ERD to %1").arg(QDir::toNativeSeparators(path)), true);
}

void ReverseTab::generateDdl()
{
    if (m_ddlLoading || !m_loaded)
        return;
    m_ddlLoading = true;
    const int count = int(m_model.value(QStringLiteral("tables")).toArray().size());
    m_ddlBusy->start(tr("Generating the DDL"), tr("%1 · %n table(s) and view(s)", nullptr, count).arg(m_schema), true);
    QPointer<ReverseTab> self(this);
    auto failed = [self] {
        if (!self)
            return;
        self->m_ddlLoading = false;
        self->m_ddlBusy->stop();
    };
    connectThen([self] {
        if (!self)
            return;
        QCborMap params;
        params.insert(QStringLiteral("schema"), self->m_schema);
        params.insert(QStringLiteral("tables"), QCborArray::fromStringList(self->m_only));
        params.insert(QStringLiteral("includeViews"), self->m_includeViews);
        // 고른 테이블만이면 시퀀스·루틴은 넣지 않는다
        params.insert(QStringLiteral("includeRoutines"), self->m_only.isEmpty());
        self->m_session->call(QStringLiteral("schemaDdl"), params, [self](const QCborMap &result, const EngineError &error) {
            if (!self)
                return;
            self->m_ddlLoading = false;
            self->m_ddlBusy->stop();
            if (error.isError()) {
                self->m_ddlInfo->setText(tr("Could not generate the DDL: %1").arg(error.message.section(u'\n', 0, 0)));
                return;
            }
            self->m_ddlLoaded = true;
            self->m_ddl->setPlainText(result.value(QStringLiteral("script")).toString());
            QString info = tr("%n object(s)", nullptr, int(result.value(QStringLiteral("objects")).toInteger()));
            const QCborArray failedObjects = result.value(QStringLiteral("failed")).toArray();
            if (!failedObjects.isEmpty())
                info += tr(" · %n could not be generated (see the comments in the script)", nullptr, int(failedObjects.size()));
            if (!result.value(QStringLiteral("cycle")).toArray().isEmpty())
                info += tr(" · some tables refer to each other (noted at the top)");
            if (result.value(QStringLiteral("cancelled")).toBool())
                info += tr(" · stopped before the end");
            info += tr(" · referenced tables come first · %1 ms").arg(result.value(QStringLiteral("elapsedMs")).toInteger());
            self->m_ddlInfo->setText(info);
            self->m_saveDdl->setEnabled(true);
            self->m_openDdl->setEnabled(true);
        });
    }, failed);
}

void ReverseTab::saveDdl()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save DDL"), QDir(lastDirectory()).filePath(m_schema + QStringLiteral("_ddl.sql")),
                                                      tr("SQL files (*.sql);;All files (*)"));
    if (path.isEmpty())
        return;
    rememberDirectory(path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(m_ddl->toPlainText().toUtf8()) < 0) {
        setStatus(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()), true);
        return;
    }
    setStatus(tr("Saved the DDL to %1").arg(QDir::toNativeSeparators(path)));
}
