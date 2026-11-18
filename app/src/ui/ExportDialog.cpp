#include "ExportDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {
const auto kFormatKey = QStringLiteral("export/format");
const auto kHeaderKey = QStringLiteral("export/header");
const auto kBomKey = QStringLiteral("export/bom");
const auto kNullKey = QStringLiteral("export/nullText");
const auto kDirKey = QStringLiteral("export/dir");
const auto kClipboardKey = QStringLiteral("export/clipboard");
} // namespace

ExportDialog::ExportDialog(bool hasSelection, bool hasMoreRows, const QString &suggestedTable, SqlDialect dialect,
                           QWidget *parent)
    : QDialog(parent)
    , m_dialect(dialect)
{
    setWindowTitle(tr("Export Results"));
    setObjectName(QStringLiteral("exportDialog"));
    QSettings settings;

    m_format = new QComboBox(this);
    m_format->setObjectName(QStringLiteral("exportFormat"));
    for (const auto format : {ExportOptions::Format::Csv, ExportOptions::Format::Excel, ExportOptions::Format::Tsv,
                              ExportOptions::Format::SqlInsert, ExportOptions::Format::Json, ExportOptions::Format::Markdown})
        m_format->addItem(ResultExport::formatName(format), int(format));
    m_format->setCurrentIndex(qMax(0, m_format->findData(settings.value(kFormatKey, int(ExportOptions::Format::Csv)).toInt())));

    m_allRows = new QRadioButton(hasMoreRows ? tr("All rows (the remaining rows are fetched first)") : tr("All rows"), this);
    m_selection = new QRadioButton(tr("Selected cells only"), this);
    m_selection->setEnabled(hasSelection);
    // 같은 부모의 라디오 버튼은 서로 배타적이 되므로 묶음을 따로 둔다.
    auto *rowsGroup = new QButtonGroup(this);
    rowsGroup->addButton(m_allRows);
    rowsGroup->addButton(m_selection);
    m_allRows->setChecked(true);

    m_header = new QCheckBox(tr("Column names in the first row"), this);
    m_header->setChecked(settings.value(kHeaderKey, true).toBool());
    m_bom = new QCheckBox(tr("UTF-8 BOM (lets Excel read Korean and other non-ASCII text)"), this);
    m_bom->setChecked(settings.value(kBomKey, true).toBool());
    m_nullText = new QLineEdit(settings.value(kNullKey).toString(), this);
    m_nullText->setPlaceholderText(tr("empty"));
    m_table = new QLineEdit(suggestedTable, this);
    m_table->setObjectName(QStringLiteral("exportTable"));
    m_table->setPlaceholderText(QStringLiteral("TABLE_NAME"));

    auto *options = new QGroupBox(tr("Options"), this);
    auto *optionsLayout = new QFormLayout(options);
    optionsLayout->addRow(m_header);
    optionsLayout->addRow(m_bom);
    m_nullLabel = new QLabel(tr("NULL as:"), options);
    optionsLayout->addRow(m_nullLabel, m_nullText);
    m_tableLabel = new QLabel(tr("Table name:"), options);
    optionsLayout->addRow(m_tableLabel, m_table);

    m_toFile = new QRadioButton(tr("File:"), this);
    m_toClipboard = new QRadioButton(tr("Clipboard"), this);
    auto *targetGroup = new QButtonGroup(this);
    targetGroup->addButton(m_toFile);
    targetGroup->addButton(m_toClipboard);
    const bool clipboard = settings.value(kClipboardKey, false).toBool();
    (clipboard ? m_toClipboard : m_toFile)->setChecked(true);
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("exportPath"));
    const QString dir = settings.value(kDirKey, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
    m_path->setText(QDir(dir).filePath(QStringLiteral("result.") + ResultExport::extension(format())));
    auto *browse = new QPushButton(tr("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString ext = ResultExport::extension(this->format());
        const QString path = QFileDialog::getSaveFileName(this, tr("Export To"), m_path->text(),
                                                          tr("%1 files (*.%2);;All files (*)").arg(ext.toUpper(), ext));
        if (!path.isEmpty()) {
            m_path->setText(QDir::toNativeSeparators(path));
            m_toFile->setChecked(true);
        }
    });

    auto *rowsBox = new QGroupBox(tr("Rows"), this);
    auto *rowsLayout = new QVBoxLayout(rowsBox);
    rowsLayout->addWidget(m_allRows);
    rowsLayout->addWidget(m_selection);

    auto *targetBox = new QGroupBox(tr("Destination"), this);
    auto *targetLayout = new QVBoxLayout(targetBox);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_toFile);
    fileRow->addWidget(m_path, 1);
    fileRow->addWidget(browse);
    targetLayout->addLayout(fileRow);
    targetLayout->addWidget(m_toClipboard);

    auto *formatRow = new QFormLayout;
    formatRow->addRow(tr("Format:"), m_format);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(formatRow);
    layout->addWidget(rowsBox);
    layout->addWidget(options);
    layout->addWidget(targetBox);
    layout->addWidget(buttons);
    resize(560, sizeHint().height());

    connect(m_format, &QComboBox::currentIndexChanged, this, &ExportDialog::updateForFormat);
    updateForFormat();
}

ExportOptions::Format ExportDialog::format() const
{
    return ExportOptions::Format(m_format->currentData().toInt());
}

void ExportDialog::updateForFormat()
{
    const ExportOptions::Format f = format();
    const bool delimited = f == ExportOptions::Format::Csv || f == ExportOptions::Format::Tsv;
    m_header->setVisible(delimited || f == ExportOptions::Format::Excel);
    m_bom->setVisible(delimited);
    const bool nullText = delimited || f == ExportOptions::Format::Markdown;
    m_nullLabel->setVisible(nullText);
    m_nullText->setVisible(nullText);
    m_tableLabel->setVisible(f == ExportOptions::Format::SqlInsert);
    m_table->setVisible(f == ExportOptions::Format::SqlInsert);

    // Excel은 파일로만 보낼 수 있다.
    const bool binary = ResultExport::isBinary(f);
    m_toClipboard->setEnabled(!binary);
    if (binary)
        m_toFile->setChecked(true);

    // 파일 이름의 확장자를 형식에 맞춘다.
    const QFileInfo info(m_path->text());
    if (!info.fileName().isEmpty())
        m_path->setText(QDir::toNativeSeparators(QDir(info.path()).filePath(info.completeBaseName() + u'.' + ResultExport::extension(f))));
}

ExportOptions ExportDialog::options() const
{
    ExportOptions options;
    options.format = format();
    options.header = m_header->isChecked();
    options.bom = m_bom->isChecked();
    options.nullText = m_nullText->text();
    options.tableName = m_table->text();
    options.dialect = m_dialect;
    return options;
}

bool ExportDialog::selectionOnly() const
{
    return m_selection->isChecked();
}

bool ExportDialog::toClipboard() const
{
    return m_toClipboard->isChecked() && m_toClipboard->isEnabled();
}

QString ExportDialog::filePath() const
{
    return QDir::fromNativeSeparators(m_path->text().trimmed());
}

void ExportDialog::accept()
{
    if (!toClipboard()) {
        const QFileInfo info(filePath());
        if (filePath().isEmpty() || !info.dir().exists()) {
            QMessageBox::warning(this, windowTitle(), tr("Choose a file in an existing folder."));
            return;
        }
        if (info.exists()) {
            const auto answer = QMessageBox::question(this, windowTitle(),
                                                      tr("%1 already exists. Replace it?").arg(info.fileName()),
                                                      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes)
                return;
        }
    }
    QSettings settings;
    settings.setValue(kFormatKey, int(format()));
    settings.setValue(kHeaderKey, m_header->isChecked());
    settings.setValue(kBomKey, m_bom->isChecked());
    settings.setValue(kNullKey, m_nullText->text());
    settings.setValue(kClipboardKey, toClipboard());
    if (!toClipboard())
        settings.setValue(kDirKey, QFileInfo(filePath()).absolutePath());
    QDialog::accept();
}
