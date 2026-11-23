#include "ImportDialog.h"

#include "MetadataService.h"
#include "core/SqlIdentifiers.h"

#include <QButtonGroup>
#include <QCborArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
constexpr int kPreviewRows = 50;
const auto kDirKey = QStringLiteral("import/dir");

QString columnLabel(int index, const QString &header)
{
    QString letters;
    for (int n = index + 1; n > 0; n = (n - 1) / 26)
        letters.prepend(QChar(u'A' + (n - 1) % 26));
    return header.isEmpty() ? letters : QStringLiteral("%1: %2").arg(letters, header);
}

bool looksNumeric(const QString &type)
{
    static const QStringList words{QStringLiteral("NUMBER"), QStringLiteral("INT"), QStringLiteral("DECIMAL"),
                                   QStringLiteral("NUMERIC"), QStringLiteral("FLOAT"), QStringLiteral("DOUBLE"),
                                   QStringLiteral("REAL"), QStringLiteral("SERIAL")};
    const QString upper = type.toUpper();
    for (const QString &word : words) {
        if (upper.contains(word))
            return true;
    }
    return false;
}

bool looksTemporal(const QString &type)
{
    const QString upper = type.toUpper();
    return upper.startsWith(u"DATE") || upper.startsWith(u"TIMESTAMP") || upper.startsWith(u"DATETIME");
}
} // namespace

ImportDialog::ImportDialog(MetadataService *metadata, const QString &connectionId, SqlDialect dialect, const QString &schema,
                           const QString &table, QWidget *parent)
    : QDialog(parent)
    , m_metadata(metadata)
    , m_connectionId(connectionId)
    , m_dialect(dialect)
    , m_defaultSchema(schema)
{
    setWindowTitle(tr("Import Data"));
    setObjectName(QStringLiteral("importDialog"));
    resize(1000, 720);

    // 1) 파일
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("importPath"));
    m_path->setPlaceholderText(tr("Excel (.xlsx) or CSV / TSV file"));
    auto *browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &ImportDialog::browse);
    connect(m_path, &QLineEdit::editingFinished, this, [this] {
        if (!m_path->text().isEmpty())
            loadFile(m_path->text());
    });
    m_sheet = new QComboBox(this);
    m_header = new QCheckBox(tr("First row has column names"), this);
    m_header->setChecked(true);
    m_emptyAsNull = new QCheckBox(tr("Empty cells as NULL"), this);
    m_emptyAsNull->setChecked(true);
    m_trim = new QCheckBox(tr("Trim spaces"), this);
    m_trim->setChecked(true);
    connect(m_sheet, &QComboBox::currentIndexChanged, this, &ImportDialog::rebuild);
    connect(m_header, &QCheckBox::toggled, this, &ImportDialog::rebuild);
    connect(m_emptyAsNull, &QCheckBox::toggled, this, &ImportDialog::rebuildPreview);
    connect(m_trim, &QCheckBox::toggled, this, &ImportDialog::rebuildPreview);

    auto *fileBox = new QGroupBox(tr("File"), this);
    auto *fileLayout = new QGridLayout(fileBox);
    fileLayout->addWidget(m_path, 0, 0, 1, 3);
    fileLayout->addWidget(browseButton, 0, 3);
    fileLayout->addWidget(new QLabel(tr("Sheet:"), fileBox), 1, 0);
    fileLayout->addWidget(m_sheet, 1, 1);
    auto *checks = new QHBoxLayout;
    checks->addWidget(m_header);
    checks->addWidget(m_emptyAsNull);
    checks->addWidget(m_trim);
    checks->addStretch();
    fileLayout->addLayout(checks, 1, 2, 1, 2);
    fileLayout->setColumnStretch(2, 1);

    // 2) 대상
    m_existing = new QRadioButton(tr("Existing table:"), this);
    m_new = new QRadioButton(tr("New table:"), this);
    auto *targetGroup = new QButtonGroup(this);
    targetGroup->addButton(m_existing);
    targetGroup->addButton(m_new);
    m_table = new QLineEdit(this);
    m_table->setObjectName(QStringLiteral("importTable"));
    m_table->setPlaceholderText(tr("schema.table"));
    m_loadColumns = new QPushButton(tr("Load Columns"), this);
    m_newTable = new QLineEdit(this);
    m_newTable->setObjectName(QStringLiteral("importNewTable"));
    m_newTable->setPlaceholderText(tr("name for the new table"));
    connect(m_loadColumns, &QPushButton::clicked, this, &ImportDialog::loadTargetColumns);
    connect(m_table, &QLineEdit::returnPressed, this, &ImportDialog::loadTargetColumns);
    connect(m_existing, &QRadioButton::toggled, this, &ImportDialog::rebuild);
    connect(m_newTable, &QLineEdit::textChanged, this, [this] { setStatus(statusText()); });

    auto *targetBox = new QGroupBox(tr("Target"), this);
    auto *targetLayout = new QGridLayout(targetBox);
    targetLayout->addWidget(m_existing, 0, 0);
    targetLayout->addWidget(m_table, 0, 1);
    targetLayout->addWidget(m_loadColumns, 0, 2);
    targetLayout->addWidget(m_new, 1, 0);
    targetLayout->addWidget(m_newTable, 1, 1);
    targetLayout->setColumnStretch(1, 1);

    // 3) 짝과 미리보기
    m_mapping = new QTableWidget(0, 3, this);
    m_mapping->setObjectName(QStringLiteral("importMapping"));
    m_mapping->setHorizontalHeaderLabels({tr("Table column"), tr("Type"), tr("From file column")});
    m_mapping->horizontalHeader()->setStretchLastSection(true);
    m_mapping->verticalHeader()->hide();
    m_preview = new QTableWidget(0, 0, this);
    m_preview->setObjectName(QStringLiteral("importPreview"));
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_preview->verticalHeader()->setDefaultSectionSize(m_preview->fontMetrics().height() + 8);
    connect(m_mapping, &QTableWidget::itemChanged, this, [this] {
        if (!m_rebuilding)
            rebuildPreview(); // 새 테이블의 이름·타입을 고쳤다
    });
    auto *splitter = new QSplitter(Qt::Vertical, this);
    auto *mappingBox = new QGroupBox(tr("Columns"), this);
    (new QVBoxLayout(mappingBox))->addWidget(m_mapping);
    auto *previewBox = new QGroupBox(tr("Preview (first %1 rows)").arg(kPreviewRows), this);
    (new QVBoxLayout(previewBox))->addWidget(m_preview);
    splitter->addWidget(mappingBox);
    splitter->addWidget(previewBox);

    m_stopOnError = new QCheckBox(tr("Stop at the first error"), this);
    m_stopOnError->setChecked(true);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("importStatus"));
    m_status->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(this);
    m_import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    m_import->setDefault(true);
    m_asSql = buttons->addButton(tr("Open as SQL"), QDialogButtonBox::ActionRole);
    m_asSql->setToolTip(tr("Put the statements in a new editor tab instead of running them"));
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_asSql, &QPushButton::clicked, this, [this] {
        m_openAsSql = true;
        accept();
    });

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_stopOnError);
    bottom->addWidget(m_status, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(fileBox);
    layout->addWidget(targetBox);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);
    layout->addWidget(buttons);

    if (!table.isEmpty()) {
        m_existing->setChecked(true);
        m_table->setText(schema.isEmpty() ? SqlIdentifiers::quote(table, dialect)
                                          : SqlIdentifiers::qualified(schema, table, dialect));
        loadTargetColumns();
    } else {
        m_new->setChecked(true);
    }
    rebuild();
}

void ImportDialog::browse()
{
    QSettings settings;
    const QString dir = settings.value(kDirKey, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Import From"), dir,
                                                      tr("Excel and text files (*.xlsx *.csv *.tsv *.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    settings.setValue(kDirKey, QFileInfo(path).absolutePath());
    loadFile(path);
}

bool ImportDialog::loadFile(const QString &path)
{
    m_path->setText(QDir::toNativeSeparators(path));
    QString error;
    QList<ImportSheet> sheets;
    if (!DataImport::read(path, &sheets, &error)) {
        m_sheets.clear();
        m_sheet->clear();
        setStatus(tr("Could not read the file: %1").arg(error), true);
        rebuild();
        return false;
    }
    m_sheets = sheets;
    {
        QSignalBlocker blocker(m_sheet);
        m_sheet->clear();
        for (const ImportSheet &sheet : std::as_const(m_sheets))
            m_sheet->addItem(sheet.name);
    }
    m_sheet->setEnabled(m_sheets.size() > 1);
    // 새 테이블 이름의 기본값은 파일 이름
    if (m_newTable->text().isEmpty()) {
        QString base = QFileInfo(path).completeBaseName();
        base.replace(QRegularExpression(QStringLiteral("[^\\w]"), QRegularExpression::UseUnicodePropertiesOption), QStringLiteral("_"));
        m_newTable->setText(m_dialect == SqlDialect::PostgreSql ? base.toLower() : m_dialect == SqlDialect::MySql ? base : base.toUpper());
    }
    rebuild();
    return true;
}

void ImportDialog::setTargetTable(const QString &qualified)
{
    m_existing->setChecked(true);
    m_table->setText(qualified);
    loadTargetColumns();
}

void ImportDialog::setNewTable(const QString &name)
{
    m_new->setChecked(true);
    m_newTable->setText(name);
    rebuild();
}

void ImportDialog::setHasHeader(bool on)
{
    m_header->setChecked(on);
}

void ImportDialog::loadTargetColumns()
{
    const std::optional<QualifiedName> name = SqlIdentifiers::parseQualified(m_table->text());
    if (!name || !m_metadata) {
        setStatus(tr("Type the table name as schema.table."), true);
        return;
    }
    m_existing->setChecked(true);
    m_existingColumns.clear();
    m_existingName.clear();
    setStatus(tr("Reading the table columns..."));
    QCborMap params;
    const QString schema = name->schema.isEmpty() ? m_defaultSchema : name->schema;
    if (!schema.isEmpty())
        params.insert(QStringLiteral("schema"), schema);
    params.insert(QStringLiteral("name"), name->name);
    params.insert(QStringLiteral("schemaQuoted"), name->schema.isEmpty() ? !m_defaultSchema.isEmpty() : name->schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), name->nameQuoted);
    QPointer<ImportDialog> self(this);
    m_metadata->request(
        m_connectionId, QStringLiteral("tableInfo"), params,
        [self](const QCborMap &table, const QString &error) {
            if (!self)
                return;
            if (!error.isEmpty()) {
                self->setStatus(tr("Could not read the table: %1").arg(error), true);
                self->rebuild();
                return;
            }
            for (const QCborValue &value : table.value(QStringLiteral("columns")).toArray()) {
                const QCborMap column = value.toMap();
                self->m_existingColumns.append({column.value(QStringLiteral("name")).toString(),
                                                column.value(QStringLiteral("type")).toString(),
                                                column.value(QStringLiteral("numeric")).toBool(),
                                                column.value(QStringLiteral("temporal")).toBool()});
            }
            self->m_existingName = SqlIdentifiers::qualified(table.value(QStringLiteral("schema")).toString(),
                                                             table.value(QStringLiteral("name")).toString(), self->m_dialect);
            self->rebuild();
        },
        true, true);
}

QStringList ImportDialog::header() const
{
    const int sheet = m_sheet->currentIndex();
    if (sheet < 0 || sheet >= m_sheets.size() || m_sheets[sheet].rows.isEmpty())
        return {};
    if (m_header->isChecked())
        return m_sheets[sheet].rows.first();
    int columns = 0;
    for (const QStringList &row : m_sheets[sheet].rows)
        columns = qMax(columns, int(row.size()));
    QStringList names;
    for (int c = 0; c < columns; ++c)
        names << QString();
    return names;
}

QList<QStringList> ImportDialog::dataRows() const
{
    const int sheet = m_sheet->currentIndex();
    if (sheet < 0 || sheet >= m_sheets.size())
        return {};
    QList<QStringList> rows = m_sheets[sheet].rows;
    if (m_header->isChecked() && !rows.isEmpty())
        rows.removeFirst();
    // 끝의 빈 줄은 뺀다
    while (!rows.isEmpty()) {
        bool empty = true;
        for (const QString &cell : rows.last())
            empty = empty && cell.trimmed().isEmpty();
        if (!empty)
            break;
        rows.removeLast();
    }
    return rows;
}

void ImportDialog::rebuild()
{
    m_rebuilding = true;
    const QStringList fileHeader = header();
    int fileColumns = int(fileHeader.size());
    for (const QStringList &row : dataRows())
        fileColumns = qMax(fileColumns, int(row.size()));

    QList<ImportColumn> columns;
    QList<int> map;
    const bool newTable = m_new->isChecked();
    if (newTable) {
        QStringList names = fileHeader;
        while (names.size() < fileColumns)
            names << QString();
        columns = DataImport::guessColumns(names, dataRows(), m_dialect);
        for (int i = 0; i < columns.size(); ++i)
            map << i;
    } else {
        columns = m_existingColumns;
        map = DataImport::autoMap(columns, fileHeader, m_header->isChecked(), fileColumns);
    }
    m_table->setEnabled(!newTable);
    m_loadColumns->setEnabled(!newTable);
    m_newTable->setEnabled(newTable);

    m_mapping->setRowCount(int(columns.size()));
    for (int r = 0; r < columns.size(); ++r) {
        auto *name = new QTableWidgetItem(columns[r].name);
        auto *type = new QTableWidgetItem(columns[r].typeName);
        if (!newTable) { // 있는 테이블의 컬럼은 고칠 수 없다
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            type->setFlags(type->flags() & ~Qt::ItemIsEditable);
        }
        m_mapping->setItem(r, 0, name);
        m_mapping->setItem(r, 1, type);
        auto *combo = new QComboBox(m_mapping);
        combo->addItem(tr("(skip)"), -1);
        for (int c = 0; c < fileColumns; ++c)
            combo->addItem(columnLabel(c, fileHeader.value(c)), c);
        combo->setCurrentIndex(qMax(0, combo->findData(map.value(r, -1))));
        connect(combo, &QComboBox::currentIndexChanged, this, &ImportDialog::rebuildPreview);
        m_mapping->setCellWidget(r, 2, combo);
    }
    m_mapping->resizeColumnsToContents();
    // 새 콤보 상자는 다음 배치 때 자리를 잡는다. 그 전에 맨 위에 겹쳐 그려지지 않게 지금 배치한다.
    m_mapping->scrollToTop();
    m_mapping->doItemsLayout();
    m_rebuilding = false;
    rebuildPreview();
}

QList<ImportColumn> ImportDialog::targetColumns() const
{
    if (!m_new->isChecked())
        return m_existingColumns;
    QList<ImportColumn> columns;
    for (int r = 0; r < m_mapping->rowCount(); ++r) {
        ImportColumn column;
        column.name = m_mapping->item(r, 0) ? m_mapping->item(r, 0)->text().trimmed() : QString();
        column.typeName = m_mapping->item(r, 1) ? m_mapping->item(r, 1)->text().trimmed() : QString();
        column.numeric = looksNumeric(column.typeName);
        column.temporal = looksTemporal(column.typeName);
        columns.append(column);
    }
    return columns;
}

QList<int> ImportDialog::mapping() const
{
    QList<int> map;
    for (int r = 0; r < m_mapping->rowCount(); ++r) {
        auto *combo = qobject_cast<QComboBox *>(m_mapping->cellWidget(r, 2));
        map << (combo ? combo->currentData().toInt() : -1);
    }
    return map;
}

void ImportDialog::rebuildPreview()
{
    const QList<ImportColumn> columns = targetColumns();
    const QList<int> map = mapping();
    QList<int> used;
    QStringList headers;
    for (int i = 0; i < columns.size(); ++i) {
        if (map.value(i, -1) >= 0) {
            used << i;
            headers << columns[i].name;
        }
    }
    const QList<QStringList> rows = dataRows();
    const int shown = int(qMin<qsizetype>(rows.size(), kPreviewRows));
    m_preview->clear();
    m_preview->setColumnCount(int(headers.size()));
    m_preview->setHorizontalHeaderLabels(headers);
    m_preview->setRowCount(shown);
    const QColor dim = palette().color(QPalette::PlaceholderText);
    for (int r = 0; r < shown; ++r) {
        for (int k = 0; k < used.size(); ++k) {
            QString text = rows[r].value(map[used[k]]);
            if (m_trim->isChecked())
                text = text.trimmed();
            auto *item = new QTableWidgetItem(text.isEmpty() && m_emptyAsNull->isChecked() ? QStringLiteral("<null>") : text);
            if (text.isEmpty() && m_emptyAsNull->isChecked())
                item->setForeground(dim);
            m_preview->setItem(r, k, item);
        }
    }
    m_preview->resizeColumnsToContents();
    setStatus(statusText());
}

QString ImportDialog::targetName() const
{
    if (!m_new->isChecked())
        return m_existingName;
    const std::optional<QualifiedName> name = SqlIdentifiers::parseQualified(m_newTable->text());
    if (!name)
        return {};
    const QString schema = name->schema.isEmpty() ? m_defaultSchema : name->schema;
    return schema.isEmpty() ? SqlIdentifiers::quote(name->name, m_dialect) : SqlIdentifiers::qualified(schema, name->name, m_dialect);
}

QString ImportDialog::targetDisplay() const
{
    return targetName();
}

bool ImportDialog::createsTable() const
{
    return m_new->isChecked();
}

bool ImportDialog::stopOnError() const
{
    return m_stopOnError->isChecked();
}

int ImportDialog::dataRowCount() const
{
    int count = 0;
    for (const QStringList &row : dataRows()) {
        bool empty = true;
        for (const QString &cell : row)
            empty = empty && cell.trimmed().isEmpty();
        count += empty ? 0 : 1;
    }
    return count;
}

bool ImportDialog::isReady() const
{
    if (m_sheets.isEmpty() || targetName().isEmpty() || targetColumns().isEmpty())
        return false;
    for (int column : mapping()) {
        if (column >= 0)
            return true;
    }
    return false;
}

QString ImportDialog::statusText() const
{
    if (m_sheets.isEmpty())
        return tr("Choose an Excel or CSV file.");
    if (!m_new->isChecked() && m_existingColumns.isEmpty())
        return tr("Load the columns of the target table.");
    if (targetName().isEmpty())
        return tr("Type a name for the new table.");
    if (!isReady())
        return tr("Pick at least one file column.");
    return tr("%1 rows will be inserted into %2%3.")
        .arg(QLocale().toString(dataRowCount()), targetName(), m_new->isChecked() ? tr(" (created first)") : QString());
}

void ImportDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("error", error);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_import->setEnabled(isReady());
    m_asSql->setEnabled(isReady());
}

QStringList ImportDialog::statements() const
{
    ImportOptions options;
    options.header = m_header->isChecked();
    options.emptyAsNull = m_emptyAsNull->isChecked();
    options.trim = m_trim->isChecked();
    const QList<ImportColumn> columns = targetColumns();
    QStringList out;
    if (m_new->isChecked()) {
        // 넣지 않는 컬럼은 새 테이블에도 만들지 않는다
        QList<ImportColumn> created;
        const QList<int> map = mapping();
        for (int i = 0; i < columns.size(); ++i) {
            if (map.value(i, -1) >= 0)
                created << columns[i];
        }
        out << DataImport::createTable(targetName(), created, m_dialect);
    }
    out << DataImport::insertStatements(targetName(), columns, mapping(), dataRows(), options, m_dialect);
    return out;
}

void ImportDialog::accept()
{
    if (!isReady()) {
        m_openAsSql = false;
        QMessageBox::information(this, windowTitle(), statusText());
        return;
    }
    QDialog::accept();
}
