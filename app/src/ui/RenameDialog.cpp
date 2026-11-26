#include "RenameDialog.h"

#include "MetadataService.h"
#include "Theme.h"

#include <QApplication>
#include <QCborArray>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kSourceRole = Qt::UserRole + 1; // m_sources 위치
constexpr int kMatchRole = Qt::UserRole + 2;  // matches 위치 (바꿀 곳 항목), -1이면 묶음·객체 항목
constexpr int kMaxFiles = 5000;
constexpr qint64 kMaxFileSize = 5 * 1024 * 1024;

const QStringList &sqlPatterns()
{
    static const QStringList patterns = {QStringLiteral("*.sql"), QStringLiteral("*.pls"), QStringLiteral("*.pks"),
                                         QStringLiteral("*.pkb"), QStringLiteral("*.prc"), QStringLiteral("*.fnc"),
                                         QStringLiteral("*.trg"), QStringLiteral("*.vw")};
    return patterns;
}

// 미리보기: "  12 - 원래 줄" 빨강, "  12 + 바뀐 줄" 초록
class DiffHighlighter : public QSyntaxHighlighter
{
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

protected:
    void highlightBlock(const QString &text) override
    {
        static const QRegularExpression line(QStringLiteral("^\\s*\\d+ ([-+]) "));
        const QRegularExpressionMatch m = line.match(text);
        if (!m.hasMatch()) {
            if (text.startsWith(QLatin1String("--"))) {
                QTextCharFormat dim;
                dim.setForeground(QColor(0x8c, 0x91, 0x99));
                setFormat(0, int(text.size()), dim);
            }
            return;
        }
        QTextCharFormat format;
        const bool removed = m.captured(1) == u"-";
        format.setBackground(removed ? QColor(220, 60, 60, 55) : QColor(60, 170, 90, 55));
        setFormat(0, int(text.size()), format);
    }
};

QString lineText(const QString &text, int position)
{
    const qsizetype start = text.lastIndexOf(u'\n', position - 1) + 1;
    qsizetype end = text.indexOf(u'\n', position);
    if (end < 0)
        end = text.size();
    QString line = text.mid(start, end - start);
    line.remove(u'\r');
    return line;
}

} // namespace

RenameDialog::RenameDialog(MetadataService *metadata, const QString &metadataId, SqlDialect dialect, const QString &schema,
                           const QString &table, const QString &column, const QList<OpenText> &openTabs,
                           const QStringList &folders, QWidget *parent)
    : QDialog(parent)
    , m_metadata(metadata)
    , m_metadataId(metadataId)
    , m_openTabs(openTabs)
    , m_folders(folders)
{
    m_target.dialect = dialect;
    m_target.schema = schema;
    m_target.table = table;
    m_target.column = column;
    setWindowTitle(column.isEmpty() ? tr("Rename Table") : tr("Rename Column"));
    resize(1100, 720);

    auto *what = new QLabel(this);
    what->setTextFormat(Qt::RichText);
    what->setText(column.isEmpty() ? tr("Rename table <b>%1.%2</b>").arg(schema.toHtmlEscaped(), table.toHtmlEscaped())
                                   : tr("Rename column <b>%1</b> of %2.%3")
                                         .arg(column.toHtmlEscaped(), schema.toHtmlEscaped(), table.toHtmlEscaped()));
    m_name = new QLineEdit(column.isEmpty() ? table : column, this);
    m_name->setObjectName(QStringLiteral("renameNewName"));
    m_name->selectAll();
    m_find = new QPushButton(tr("Find References"), this);
    m_find->setDefault(true);
    connect(m_find, &QPushButton::clicked, this, &RenameDialog::findReferences);
    connect(m_name, &QLineEdit::returnPressed, this, &RenameDialog::findReferences);

    m_includeRename = new QCheckBox(tr("Rename it in the database too (the script starts with the rename statement)"), this);
    m_includeRename->setChecked(true);
    m_searchDatabase = new QCheckBox(tr("Database objects (views, routines, triggers, synonyms)"), this);
    m_searchDatabase->setChecked(true);
    m_searchFiles = new QCheckBox(tr("SQL files in"), this);
    m_searchFiles->setChecked(!m_folders.isEmpty());
    m_foldersLabel = new QLabel(this);
    m_foldersLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *addFolder = new QPushButton(tr("Add Folder..."), this);
    connect(addFolder, &QPushButton::clicked, this, &RenameDialog::addFolder);
    m_searchTabs = new QCheckBox(tr("Open query tabs (%n)", nullptr, int(m_openTabs.size())), this);
    m_searchTabs->setChecked(true);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("renameReferences"));
    m_tree->setHeaderLabels({tr("Where"), tr("Note")});
    m_tree->setColumnWidth(0, 420);
    m_tree->setUniformRowHeights(true);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &RenameDialog::showPreview);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int column) {
        if (column == 0) {
            updateSummary();
            showPreview();
        }
    });

    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("renamePreview"));
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setPlaceholderText(tr("Select a place to see the line before and after."));
    new DiffHighlighter(m_preview->document());

    m_status = new QLabel(tr("Enter the new name and press Find References."), this);
    m_notes = new QLabel(this);
    m_notes->setWordWrap(true);
    m_notes->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(this);
    m_apply = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setEnabled(false);
    m_apply->setToolTip(tr("Write the checked changes to the files and tabs, and open the database script in a new query tab"));
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(m_apply, &QPushButton::clicked, this, [this] {
        if (applyChanges())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("New name"), this));
    nameRow->addWidget(m_name, 1);
    nameRow->addWidget(m_find);
    auto *filesRow = new QHBoxLayout;
    filesRow->addWidget(m_searchFiles);
    filesRow->addWidget(m_foldersLabel, 1);
    filesRow->addWidget(addFolder);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_preview);
    splitter->setSizes({520, 560});

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(what);
    layout->addLayout(nameRow);
    layout->addWidget(m_includeRename);
    layout->addWidget(m_searchDatabase);
    layout->addLayout(filesRow);
    layout->addWidget(m_searchTabs);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_notes);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    m_foldersLabel->setText(m_folders.isEmpty() ? tr("(no script folder for this DBMS: Database → Script Folders...)")
                                                : QDir::toNativeSeparators(m_folders.join(QStringLiteral("; "))));
}

void RenameDialog::addFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Search SQL Files In"), m_folders.value(0));
    if (folder.isEmpty() || m_folders.contains(folder))
        return;
    m_folders << folder;
    m_searchFiles->setChecked(true);
    m_foldersLabel->setText(QDir::toNativeSeparators(m_folders.join(QStringLiteral("; "))));
}

QTreeWidgetItem *RenameDialog::group(Kind kind)
{
    if (QTreeWidgetItem *item = m_groups.value(int(kind)))
        return item;
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, kind == Kind::Database ? tr("Database objects") : kind == Kind::File ? tr("SQL files") : tr("Open query tabs"));
    item->setData(0, kSourceRole, -1);
    item->setData(0, kMatchRole, -1);
    QFont font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
    item->setExpanded(true);
    m_groups.insert(int(kind), item);
    return item;
}

void RenameDialog::findReferences()
{
    const QString problem = SqlRename::validate(m_name->text());
    if (!problem.isEmpty()) {
        m_status->setText(problem);
        return;
    }
    m_target.newName = SqlRename::storedName(m_name->text(), m_target.dialect);
    if (m_target.newName == m_target.oldName()) {
        m_status->setText(tr("The new name is the same as the old one."));
        return;
    }
    ++m_generation;
    m_sources.clear();
    m_groups.clear();
    m_tree->blockSignals(true);
    m_tree->clear();
    m_tree->blockSignals(false);
    m_preview->clear();
    m_notes->clear();
    m_apply->setEnabled(false);
    m_status->setText(tr("Searching..."));

    // 열린 탭
    QSet<QString> openFiles;
    for (const OpenText &tab : std::as_const(m_openTabs)) {
        if (!tab.filePath.isEmpty())
            openFiles.insert(QFileInfo(tab.filePath).canonicalFilePath());
        if (!m_searchTabs->isChecked())
            continue;
        Source source;
        source.kind = Kind::Tab;
        source.key = tab.key;
        source.title = tab.title;
        source.text = tab.text;
        source.matches = SqlRename::find(tab.text, m_target);
        if (!source.matches.isEmpty())
            addSource(source);
    }

    // 파일 (열린 탭의 파일은 탭에서 고친다)
    if (m_searchFiles->isChecked()) {
        int scanned = 0;
        for (const QString &folder : std::as_const(m_folders)) {
            QDirIterator it(folder, sqlPatterns(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && scanned < kMaxFiles) {
                const QString path = it.next();
                ++scanned;
                if (scanned % 50 == 0) {
                    m_status->setText(tr("Searching files... %1").arg(scanned));
                    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                }
                const QFileInfo info(path);
                if (info.size() > kMaxFileSize || openFiles.contains(info.canonicalFilePath()))
                    continue;
                Source source;
                source.kind = Kind::File;
                source.key = path;
                source.title = QDir::toNativeSeparators(QDir(folder).relativeFilePath(path));
                if (!TextFile::read(path, &source.text, &source.format) || !SqlRename::mentions(source.text, m_target.oldName()))
                    continue;
                source.matches = SqlRename::find(source.text, m_target);
                if (!source.matches.isEmpty())
                    addSource(source);
            }
        }
        if (scanned >= kMaxFiles)
            m_notes->setText(tr("Only the first %1 files were searched.").arg(kMaxFiles));
    }

    // DB 객체 (메타데이터 세션)
    if (m_searchDatabase->isChecked() && m_metadata) {
        ++m_pending;
        QCborMap params;
        params.insert(QStringLiteral("schema"), m_target.schema);
        params.insert(QStringLiteral("table"), m_target.table);
        if (m_target.isColumn())
            params.insert(QStringLiteral("column"), m_target.column);
        QPointer<RenameDialog> self(this);
        const int generation = m_generation;
        m_metadata->request(
            m_metadataId, QStringLiteral("renameImpact"), params,
            [self, generation](const QCborMap &result, const QString &error) {
                if (!self || generation != self->m_generation)
                    return;
                --self->m_pending;
                self->showDatabase(result, error);
            },
            true, true);
    }
    updateSummary();
}

void RenameDialog::showDatabase(const QCborMap &result, const QString &error)
{
    if (!error.isEmpty()) {
        m_notes->setText(tr("Could not search the database: %1").arg(error.section(u'\n', 0, 0)));
        updateSummary();
        return;
    }
    QStringList notes;
    for (const QCborValue &note : result.value(QStringLiteral("notes")).toArray())
        notes << note.toString();
    for (const QCborValue &value : result.value(QStringLiteral("objects")).toArray()) {
        const QCborMap o = value.toMap();
        Source source;
        source.kind = Kind::Database;
        source.type = o.value(QStringLiteral("type")).toString();
        source.key = o.value(QStringLiteral("schema")).toString() + u'.' + o.value(QStringLiteral("name")).toString();
        source.title = QStringLiteral("%1 %2").arg(source.type, source.key);
        source.text = o.value(QStringLiteral("source")).toString();
        source.terminator = o.value(QStringLiteral("terminator")).toString();
        source.autoUpdated = o.value(QStringLiteral("autoUpdated")).toBool();
        source.note = o.value(QStringLiteral("note")).toString();
        for (const QCborValue &statement : o.value(QStringLiteral("before")).toArray())
            source.before << statement.toString();
        if (!source.text.isEmpty())
            source.matches = SqlRename::find(source.text, m_target);
        if (!source.text.isEmpty() && source.matches.isEmpty() && source.note.isEmpty())
            source.note = tr("No reference found in its text (it may use the table through another object).");
        addSource(source);
    }
    const QString existing = m_notes->text();
    m_notes->setText((existing.isEmpty() ? QString() : existing + u'\n') + notes.join(u' '));
    updateSummary();
}

void RenameDialog::addSource(const Source &source)
{
    const int index = int(m_sources.size());
    m_sources << source;
    m_tree->blockSignals(true);
    auto *item = new QTreeWidgetItem(group(source.kind));
    item->setText(0, source.title);
    item->setToolTip(0, source.key);
    item->setData(0, kSourceRole, index);
    item->setData(0, kMatchRole, -1);
    if (source.matches.isEmpty()) {
        // 바꿀 곳이 없는 것: 안내만 (DB가 스스로 고침, 소스를 못 읽음 ...)
        item->setText(1, source.note);
        item->setForeground(0, palette().color(QPalette::PlaceholderText));
    } else {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        item->setText(1, tr("%n place(s)", nullptr, int(source.matches.size())));
        for (int i = 0; i < source.matches.size(); ++i) {
            const RenameMatch &m = source.matches.at(i);
            auto *child = new QTreeWidgetItem(item);
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, m.suggested ? Qt::Checked : Qt::Unchecked);
            child->setText(0, tr("Line %1: %2").arg(m.line).arg(lineText(source.text, m.start).trimmed().left(160)));
            child->setText(1, m.reason);
            child->setData(0, kSourceRole, index);
            child->setData(0, kMatchRole, i);
        }
        item->setExpanded(source.kind != Kind::File || source.matches.size() <= 5);
    }
    m_tree->blockSignals(false);
}

QList<RenameMatch> RenameDialog::checkedMatches(int sourceIndex) const
{
    QList<RenameMatch> list;
    const Source &source = m_sources.at(sourceIndex);
    QTreeWidgetItem *groupItem = m_groups.value(int(source.kind));
    if (!groupItem)
        return list;
    for (int i = 0; i < groupItem->childCount(); ++i) {
        QTreeWidgetItem *item = groupItem->child(i);
        if (item->data(0, kSourceRole).toInt() != sourceIndex)
            continue;
        for (int j = 0; j < item->childCount(); ++j) {
            QTreeWidgetItem *child = item->child(j);
            if (child->checkState(0) == Qt::Checked)
                list << source.matches.at(child->data(0, kMatchRole).toInt());
        }
    }
    return list;
}

int RenameDialog::changeCount() const
{
    int count = 0;
    for (int i = 0; i < m_sources.size(); ++i)
        count += int(checkedMatches(i).size());
    return count;
}

void RenameDialog::updateSummary()
{
    int files = 0, tabs = 0, objects = 0, places = 0;
    for (int i = 0; i < m_sources.size(); ++i) {
        const int n = int(checkedMatches(i).size());
        if (n == 0)
            continue;
        places += n;
        switch (m_sources.at(i).kind) {
        case Kind::File: ++files; break;
        case Kind::Tab: ++tabs; break;
        case Kind::Database: ++objects; break;
        }
    }
    if (m_pending > 0) {
        m_status->setText(tr("Searching the database... (%n place(s) found so far)", nullptr, places));
    } else if (!m_target.newName.isEmpty()) {
        m_status->setText(tr("%1 → %2: %n place(s) checked in %3 database object(s), %4 file(s), %5 tab(s)", nullptr, places)
                              .arg(m_target.oldName(), m_target.newName)
                              .arg(objects)
                              .arg(files)
                              .arg(tabs));
    }
    m_apply->setEnabled(m_pending == 0 && !m_target.newName.isEmpty() && (places > 0 || m_includeRename->isChecked()));
}

void RenameDialog::showPreview()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || item->data(0, kSourceRole).toInt() < 0) {
        m_preview->clear();
        return;
    }
    const int index = item->data(0, kSourceRole).toInt();
    const Source &source = m_sources.at(index);
    if (source.matches.isEmpty()) {
        m_preview->setPlainText(QStringLiteral("-- %1\n-- %2\n\n%3").arg(source.title, source.note, source.text));
        return;
    }
    // 바꿀 곳이 있는 줄만: 원래 줄과 바뀐 줄
    const QList<RenameMatch> checked = checkedMatches(index);
    const QString after = SqlRename::apply(source.text, checked);
    const QStringList before = source.text.split(u'\n');
    const QStringList changed = after.split(u'\n');
    QSet<int> lines;
    for (const RenameMatch &m : source.matches)
        lines.insert(m.line);
    QList<int> sorted(lines.cbegin(), lines.cend());
    std::sort(sorted.begin(), sorted.end());
    QString text = QStringLiteral("-- %1\n").arg(source.title);
    if (!source.note.isEmpty())
        text += QStringLiteral("-- %1\n").arg(source.note);
    text += u'\n';
    for (int line : std::as_const(sorted)) {
        const QString old = before.value(line - 1).trimmed();
        const QString now = changed.size() == before.size() ? changed.value(line - 1).trimmed() : old;
        if (old == now) {
            text += QStringLiteral("%1   %2   (not changed)\n").arg(line, 5).arg(old);
            continue;
        }
        text += QStringLiteral("%1 - %2\n%1 + %3\n").arg(line, 5).arg(old, now);
    }
    m_preview->setPlainText(text);
    const int match = item->data(0, kMatchRole).toInt();
    if (match >= 0) {
        // 고른 줄로 옮긴다
        const QString needle = QStringLiteral("%1 ").arg(source.matches.at(match).line, 5);
        const QTextCursor found = m_preview->document()->find(needle);
        if (!found.isNull()) {
            m_preview->setTextCursor(found);
            m_preview->centerCursor();
        }
    }
}

QString RenameDialog::buildScript() const
{
    QStringList parts;
    const QString what = m_target.isColumn()
                             ? QStringLiteral("%1.%2.%3").arg(m_target.schema, m_target.table, m_target.column)
                             : QStringLiteral("%1.%2").arg(m_target.schema, m_target.table);
    parts << tr("-- Rename %1 to %2 (made by DBaro). Review it before running.").arg(what, m_target.newName);
    if (m_includeRename->isChecked())
        parts << SqlRename::renameStatement(m_target);
    for (int i = 0; i < m_sources.size(); ++i) {
        const Source &source = m_sources.at(i);
        if (source.kind != Kind::Database)
            continue;
        if (source.autoUpdated) {
            parts << tr("-- %1: updated by the database itself").arg(source.title);
            continue;
        }
        const QList<RenameMatch> checked = checkedMatches(i);
        if (checked.isEmpty())
            continue;
        QString body = SqlRename::apply(source.text, checked).trimmed();
        QString block = QStringLiteral("-- %1\n").arg(source.title);
        if (!source.before.isEmpty()) {
            // 지우고 다시 만드는 객체: 다시 만들기가 실패해도 되살릴 수 있게 원래 정의를 주석으로 남긴다
            block += tr("-- Original definition (to restore it if the CREATE below fails):\n");
            for (const QString &line : source.text.trimmed().split(u'\n'))
                block += QStringLiteral("--   ") + QString(line).remove(u'\r') + u'\n';
        }
        for (const QString &statement : source.before)
            block += statement + u'\n';
        if (source.terminator == u"delimiter") {
            block += QStringLiteral("DELIMITER $$\n%1$$\nDELIMITER ;").arg(body);
        } else {
            if (source.terminator == u";" && !body.endsWith(u';'))
                body += u';';
            block += body;
        }
        parts << block;
    }
    return parts.join(QStringLiteral("\n\n")) + u'\n';
}

bool RenameDialog::applyChanges(bool confirm)
{
    if (m_target.newName.isEmpty())
        return false;
    int files = 0, tabs = 0, objects = 0;
    for (int i = 0; i < m_sources.size(); ++i) {
        if (checkedMatches(i).isEmpty())
            continue;
        switch (m_sources.at(i).kind) {
        case Kind::File: ++files; break;
        case Kind::Tab: ++tabs; break;
        case Kind::Database: ++objects; break;
        }
    }
    const bool script = m_includeRename->isChecked() || objects > 0;
    if (confirm) {
        QStringList lines;
        if (files > 0)
            lines << tr("• %n SQL file(s) are written now.", nullptr, files);
        if (tabs > 0)
            lines << tr("• %n open tab(s) are changed (Ctrl+Z undoes it in each tab).", nullptr, tabs);
        if (script)
            lines << tr("• A script opens in a new query tab: the rename statement and %n database object(s). "
                        "The database changes only when you run it.",
                        nullptr, objects);
        if (QMessageBox::question(this, windowTitle(), tr("Rename %1 to %2?\n\n%3").arg(m_target.oldName(), m_target.newName, lines.join(u'\n')),
                                  QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
            != QMessageBox::Ok)
            return false;
    }

    m_written.clear();
    m_failed.clear();
    m_tabEdits.clear();
    for (int i = 0; i < m_sources.size(); ++i) {
        const Source &source = m_sources.at(i);
        const QList<RenameMatch> checked = checkedMatches(i);
        if (checked.isEmpty())
            continue;
        if (source.kind == Kind::Tab) {
            m_tabEdits.insert(source.key, checked);
        } else if (source.kind == Kind::File) {
            // 찾은 뒤에 파일이 바뀌었으면 쓰지 않는다
            QString current;
            TextFileFormat format;
            QString error;
            if (!TextFile::read(source.key, &current, &format, &error) || current != source.text) {
                m_failed << QDir::toNativeSeparators(source.key) + tr(" (changed since the search)");
                continue;
            }
            if (TextFile::write(source.key, SqlRename::apply(source.text, checked), source.format, &error))
                m_written << source.key;
            else
                m_failed << QDir::toNativeSeparators(source.key) + QStringLiteral(" (") + error + u')';
        }
    }
    m_script = script ? buildScript() : QString();
    if (!m_failed.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("These files were not changed:\n%1").arg(m_failed.join(u'\n')));
    return true;
}
