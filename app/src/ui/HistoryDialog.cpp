#include "HistoryDialog.h"

#include "SqlHighlighter.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kEntryRole = Qt::UserRole + 1;

QString resultText(const HistoryEntry &entry)
{
    QString text;
    if (!entry.ok)
        text = QObject::tr("Error");
    else if (entry.rows >= 0)
        text = QObject::tr("%1 row(s)").arg(QLocale().toString(entry.rows));
    else
        text = QObject::tr("Done");
    text += QStringLiteral(" · %1 ms").arg(QLocale().toString(entry.elapsedMs));
    if (entry.count > 1)
        text += QStringLiteral(" · ×%1").arg(entry.count);
    return text;
}
} // namespace

HistoryDialog::HistoryDialog(const QList<HistoryEntry> &entries, const QString &currentConnectionId, SqlDialect dialect,
                             QWidget *parent)
    : QDialog(parent)
    , m_connectionId(currentConnectionId)
{
    setWindowTitle(tr("Execution History"));
    resize(980, 640);
    for (auto it = entries.crbegin(); it != entries.crend(); ++it)
        m_entries.append(*it);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("historyFilter"));
    m_filter->setPlaceholderText(tr("Search SQL (all words must match)"));
    m_filter->setClearButtonEnabled(true);
    m_currentOnly = new QCheckBox(tr("This connection only"), this);
    m_currentOnly->setEnabled(!currentConnectionId.isEmpty());
    m_currentOnly->setChecked(!currentConnectionId.isEmpty());

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("historyList"));
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setHeaderLabels({tr("Time"), tr("Connection"), tr("SQL"), tr("Result")});
    m_list->setAlternatingRowColors(true);
    const QLocale locale;
    for (int i = 0; i < m_entries.size(); ++i) {
        const HistoryEntry &entry = m_entries[i];
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        item->setText(1, entry.connectionName);
        item->setText(2, entry.sql.simplified().left(300));
        item->setText(3, resultText(entry));
        item->setData(0, kEntryRole, i);
        if (!entry.ok) {
            item->setForeground(3, QColor(0xe0, 0x6c, 0x6c));
            item->setToolTip(3, entry.error);
        }
    }
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_list->setColumnWidth(0, 150);
    m_list->setColumnWidth(1, 150);
    m_list->setColumnWidth(3, 170);

    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("historyPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *highlighter = new SqlHighlighter(m_preview->document());
    highlighter->setDialect(dialect);
    highlighter->setDarkTheme(Theme::current() == Theme::Kind::Dark);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_list);
    splitter->addWidget(m_preview);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto *buttons = new QDialogButtonBox(this);
    m_insert = buttons->addButton(tr("Insert"), QDialogButtonBox::AcceptRole);
    m_insert->setDefault(true);
    QPushButton *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    QPushButton *clear = buttons->addButton(tr("Clear History..."), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Close);

    auto *top = new QHBoxLayout;
    top->addWidget(m_filter, 1);
    top->addWidget(m_currentOnly);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &HistoryDialog::applyFilter);
    connect(m_currentOnly, &QCheckBox::toggled, this, &HistoryDialog::applyFilter);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &HistoryDialog::updatePreview);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(copy, &QPushButton::clicked, this, [this] {
        if (!selectedSql().isEmpty())
            QApplication::clipboard()->setText(selectedSql());
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::warning(this, tr("Clear History"),
                                                 tr("Delete the whole execution history? This cannot be undone."),
                                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        m_clearRequested = true;
        reject();
    });

    applyFilter();
    m_filter->setFocus();
}

void HistoryDialog::setFilterText(const QString &text)
{
    m_filter->setText(text);
}

void HistoryDialog::setCurrentConnectionOnly(bool on)
{
    m_currentOnly->setChecked(on);
}

void HistoryDialog::applyFilter()
{
    const QStringList words = m_filter->text().split(u' ', Qt::SkipEmptyParts);
    const bool currentOnly = m_currentOnly->isChecked() && !m_connectionId.isEmpty();
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        const HistoryEntry &entry = m_entries[item->data(0, kEntryRole).toInt()];
        bool visible = !currentOnly || entry.connectionId == m_connectionId;
        for (const QString &word : words) {
            if (!visible)
                break;
            visible = entry.sql.contains(word, Qt::CaseInsensitive);
        }
        item->setHidden(!visible);
        if (visible && !firstVisible)
            firstVisible = item;
    }
    if (!m_list->currentItem() || m_list->currentItem()->isHidden())
        m_list->setCurrentItem(firstVisible);
    updatePreview();
}

int HistoryDialog::visibleCount() const
{
    int count = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        count += m_list->topLevelItem(i)->isHidden() ? 0 : 1;
    return count;
}

void HistoryDialog::selectVisible(int index)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (!item->isHidden() && index-- == 0) {
            m_list->setCurrentItem(item);
            return;
        }
    }
}

QString HistoryDialog::selectedSql() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    if (!item || item->isHidden())
        return {};
    return m_entries[item->data(0, kEntryRole).toInt()].sql;
}

void HistoryDialog::updatePreview()
{
    const QString sql = selectedSql();
    m_preview->setPlainText(sql);
    m_insert->setEnabled(!sql.isEmpty());
}
