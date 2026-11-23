#include "ResultPanel.h"

#include "ExportDialog.h"
#include "PlanView.h"
#include "ResultModel.h"
#include "ScrollHelpers.h"
#include "SqlHighlighter.h"
#include "Theme.h"

#include <QAction>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <functional>

namespace {
constexpr int kCellPadding = 10; // 결과 셀 글자 좌우 여백 (px). Theme 스타일시트의 #resultGrid 여백과 같게 둔다

bool isNullValue(const QCborValue &value)
{
    return value.isNull() || value.isUndefined();
}
} // namespace

// 결과 표의 거르기·정렬. 원본(ResultModel) 순서와 값은 바꾸지 않는다.
class ResultProxyModel : public QSortFilterProxyModel
{
public:
    ResultProxyModel(ResultModel *model, QObject *parent)
        : QSortFilterProxyModel(parent)
        , m_model(model)
    {
        setSourceModel(model);
        setDynamicSortFilter(true);
    }

    void setWords(const QString &text)
    {
        m_words = text.split(u' ', Qt::SkipEmptyParts);
        invalidateRowsFilter();
    }

    void addColumnFilter(int column, const QCborValue &value)
    {
        m_columnFilters.append({column, value});
        invalidateRowsFilter();
    }

    void clear()
    {
        m_words.clear();
        m_columnFilters.clear();
        invalidateRowsFilter();
    }

    bool isFiltering() const { return !m_words.isEmpty() || !m_columnFilters.isEmpty(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &) const override
    {
        if (m_model->rowState(row) == ResultModel::RowState::Inserted)
            return true; // 새로 더한 행은 늘 보인다
        for (const auto &[column, value] : m_columnFilters) {
            if (m_model->value(row, column) != value)
                return false;
        }
        for (const QString &word : m_words) {
            bool found = false;
            for (int column = 0; column < m_model->columnCount() && !found; ++column)
                found = ResultExport::valueText(m_model->value(row, column)).contains(word, Qt::CaseInsensitive);
            if (!found)
                return false;
        }
        return true;
    }

    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const QCborValue a = m_model->value(left.row(), left.column());
        const QCborValue b = m_model->value(right.row(), right.column());
        if (isNullValue(a) || isNullValue(b))
            return isNullValue(a) && !isNullValue(b); // NULL이 앞
        if (a.isInteger() && b.isInteger())
            return a.toInteger() < b.toInteger();
        const QString x = ResultExport::valueText(a);
        const QString y = ResultExport::valueText(b);
        if (m_model->columns().value(left.column()).numeric || (a.isDouble() && b.isDouble())) {
            bool okX = false, okY = false;
            const double dx = x.toDouble(&okX);
            const double dy = y.toDouble(&okY);
            if (okX && okY)
                return dx < dy;
        }
        return QString::localeAwareCompare(x, y) < 0;
    }

private:
    ResultModel *m_model;
    QStringList m_words;
    QList<QPair<int, QCborValue>> m_columnFilters;
};

ResultPanel::ResultPanel(QWidget *parent)
    : QTabWidget(parent)
{
    setDocumentMode(true);

    m_model = new ResultModel(this);
    connect(m_model, &ResultModel::moreRowsRequested, this, &ResultPanel::moreRowsRequested);

    m_proxy = new ResultProxyModel(m_model, this);
    m_grid = new QTableView(this);
    m_grid->setObjectName(QStringLiteral("resultGrid"));
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setModel(m_proxy);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setAlternatingRowColors(true);
    m_grid->setWordWrap(false);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->horizontalHeader()->setHighlightSections(false);
    m_grid->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 셀이 답답하지 않게 행 높이와 좌우 여백을 넉넉히 둔다.
    const int rowHeight = m_grid->fontMetrics().height() + 14;
    m_grid->verticalHeader()->setDefaultSectionSize(rowHeight);
    m_grid->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_grid->verticalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_grid->horizontalHeader()->setMinimumHeight(rowHeight + 2); // 셀·머리글 좌우 여백은 Theme 스타일시트 (#resultGrid)

    // 복사: Ctrl+C는 값만, Ctrl+Shift+C는 컬럼 이름 포함 (탭으로 구분 → 엑셀에 바로 붙여넣기)
    auto *copy = new QAction(tr("Copy"), m_grid);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    connect(copy, &QAction::triggered, this, [this] { copySelection(false); });
    auto *copyWithHeader = new QAction(tr("Copy with Header"), m_grid);
    copyWithHeader->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyWithHeader->setShortcutContext(Qt::WidgetShortcut);
    connect(copyWithHeader, &QAction::triggered, this, [this] { copySelection(true); });
    auto separator = [this] {
        auto *action = new QAction(m_grid);
        action->setSeparator(true);
        return action;
    };
    m_filterByValueAction = new QAction(tr("Filter by This Value"), m_grid);
    connect(m_filterByValueAction, &QAction::triggered, this, [this] {
        const QModelIndex current = m_proxy->mapToSource(m_grid->currentIndex());
        if (current.isValid())
            filterByValue(current.row(), current.column());
    });
    m_clearFiltersAction = new QAction(tr("Clear Filters"), m_grid);
    connect(m_clearFiltersAction, &QAction::triggered, this, &ResultPanel::clearFilters);
    auto *exportAction = new QAction(tr("Export..."), m_grid);
    connect(exportAction, &QAction::triggered, this, &ResultPanel::showExportDialog);
    createResultPage(); // 편집 동작을 만든다 (오른쪽 클릭 메뉴에도 넣는다)
    m_grid->addActions({copy, copyWithHeader, separator(), m_filterByValueAction, m_clearFiltersAction, separator(),
                        m_setNullAction, m_deleteRowsAction, separator(), exportAction});
    m_grid->setContextMenuPolicy(Qt::ActionsContextMenu);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("resultLog"));
    m_log->setFrameShape(QFrame::NoFrame);
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setPlaceholderText(tr("Execution log will appear here."));
    m_log->setMaximumBlockCount(50000); // 실행한 문장도 남으므로 오래된 줄부터 지운다

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("resultOutput"));
    m_output->setFrameShape(QFrame::NoFrame);
    m_output->setReadOnly(true);
    m_output->setFont(mono);
    m_output->setPlaceholderText(
        tr("Server output (DBMS_OUTPUT, RAISE NOTICE, warnings) will appear here."));

    // Log·Output 오른쪽 클릭 메뉴: 기본 메뉴(복사, 모두 선택) + 비우기
    const auto addClearMenu = [this](QPlainTextEdit *view, const std::function<void()> &clear) {
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QWidget::customContextMenuRequested, view, [view, clear](const QPoint &pos) {
            QMenu *menu = view->createStandardContextMenu(pos);
            menu->addSeparator();
            QAction *clearAction = menu->addAction(tr("Clear"));
            clearAction->setEnabled(!view->document()->isEmpty());
            connect(clearAction, &QAction::triggered, view, clear);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(view->viewport()->mapToGlobal(pos));
        });
    };
    addClearMenu(m_log, [this] { clearLog(); });
    addClearMenu(m_output, [this] { clearOutput(); });

    for (QAbstractScrollArea *area : {static_cast<QAbstractScrollArea *>(m_grid), static_cast<QAbstractScrollArea *>(m_log),
                                      static_cast<QAbstractScrollArea *>(m_output)})
        enableShiftWheelHorizontalScroll(area);

    addTab(m_resultPage, tr("Result"));
    addTab(m_log, tr("Log"));
    addTab(m_output, tr("Output"));

    m_plan = new PlanView(this);
    m_plan->setObjectName(QStringLiteral("resultPlan"));
    m_plan->hide(); // 처음 Ctrl+E를 누를 때 탭으로 붙인다
    connect(m_plan, &PlanView::objectRequested, this, &ResultPanel::planObjectRequested);

    // 오른쪽 위: 행 수와 걸린 시간
    // 탭 위젯의 모서리 위젯으로 넣으면 탭 줄 테두리가 그 자리에서 끊겨서, 탭 줄의 빈 오른쪽 끝에 얹는다.
    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("resultInfo"));
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setAttribute(Qt::WA_TranslucentBackground);

    m_runningTimer = new QTimer(this);
    m_runningTimer->setInterval(100);
    connect(m_runningTimer, &QTimer::timeout, this, &ResultPanel::updateRunningText);
}

void ResultPanel::createResultPage()
{
    m_resultPage = new QWidget(this);
    m_resultPage->setObjectName(QStringLiteral("resultPage"));

    m_toolbar = new QToolBar(m_resultPage);
    m_toolbar->setObjectName(QStringLiteral("resultToolbar"));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolbar->setMovable(false);

    m_filter = new QLineEdit(m_toolbar);
    m_filter->setObjectName(QStringLiteral("resultFilter"));
    m_filter->setPlaceholderText(tr("Filter rows"));
    m_filter->setToolTip(tr("Show only rows that contain every word (in the rows loaded so far)"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setFixedWidth(240);
    connect(m_filter, &QLineEdit::textChanged, this, &ResultPanel::setQuickFilter);
    m_toolbar->addWidget(m_filter);

    m_fetchAllAction = m_toolbar->addAction(tr("Fetch All"), this, &ResultPanel::fetchAll);
    m_fetchAllAction->setToolTip(tr("Load every remaining row (filtering and sorting use the loaded rows)"));
    m_toolbar->addSeparator();

    m_recordAction = m_toolbar->addAction(tr("Record"));
    m_recordAction->setCheckable(true);
    m_recordAction->setToolTip(tr("Show the current row as a list and the whole value of the current cell"));
    connect(m_recordAction, &QAction::toggled, this, &ResultPanel::setRecordViewVisible);
    m_toolbar->addSeparator();

    m_editAction = m_toolbar->addAction(tr("Edit"));
    m_editAction->setCheckable(true);
    m_editAction->setToolTip(tr("Edit a single-table result and save the changes as UPDATE, INSERT and DELETE"));
    connect(m_editAction, &QAction::toggled, this, &ResultPanel::setEditMode);
    m_addRowAction = m_toolbar->addAction(tr("+ Row"), this, &ResultPanel::addRow);
    m_addRowAction->setToolTip(tr("Add a row"));
    m_deleteRowsAction = new QAction(tr("Delete Rows"), this);
    m_deleteRowsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Delete));
    m_deleteRowsAction->setShortcutContext(Qt::WidgetShortcut);
    m_deleteRowsAction->setToolTip(tr("Mark the selected rows for deletion (again to undo)"));
    connect(m_deleteRowsAction, &QAction::triggered, this, &ResultPanel::deleteRows);
    m_toolbar->addAction(m_deleteRowsAction);
    m_setNullAction = new QAction(tr("Set NULL"), this);
    connect(m_setNullAction, &QAction::triggered, this, &ResultPanel::setSelectedNull);
    m_toolbar->addAction(m_setNullAction);
    m_revertAction = m_toolbar->addAction(tr("Revert"), this, [this] { m_model->revertChanges(); });
    m_revertAction->setToolTip(tr("Undo every change that is not applied yet"));
    m_applyAction = m_toolbar->addAction(tr("Apply..."), this, &ResultPanel::previewAndApply);
    m_applyAction->setToolTip(tr("Review the UPDATE, INSERT and DELETE statements and run them"));

    auto *spacer = new QWidget(m_toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);
    m_rowsLabel = new QLabel(m_toolbar);
    m_rowsLabel->setObjectName(QStringLiteral("resultRowsLabel"));
    // 도구 막대는 안의 글자가 바뀌어도 폭을 다시 잡지 않으므로 처음부터 넉넉히 둔다
    m_rowsLabel->setMinimumWidth(m_rowsLabel->fontMetrics().horizontalAdvance(QStringLiteral("999,999 of 9,999,999+ rows")) + 16);
    m_rowsLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_toolbar->addWidget(m_rowsLabel);
    QAction *exportAction = m_toolbar->addAction(tr("Export..."), this, &ResultPanel::showExportDialog);
    exportAction->setToolTip(tr("Save the result as Excel (.xlsx), CSV, SQL INSERT, JSON or Markdown, or copy it"));

    // 오른쪽: 지금 행을 세로로, 지금 셀의 값을 통째로
    m_record = new QTableWidget(0, 2, m_resultPage);
    m_record->setObjectName(QStringLiteral("resultRecord"));
    m_record->setHorizontalHeaderLabels({tr("Column"), tr("Value")});
    m_record->horizontalHeader()->setStretchLastSection(true);
    m_record->verticalHeader()->hide();
    m_record->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_record->setWordWrap(false);
    m_record->setFrameShape(QFrame::NoFrame);
    m_value = new QPlainTextEdit(m_resultPage);
    m_value->setObjectName(QStringLiteral("resultValue"));
    m_value->setReadOnly(true);
    m_value->setFrameShape(QFrame::NoFrame);
    m_value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_side = new QTabWidget(m_resultPage);
    m_side->setDocumentMode(true);
    m_side->addTab(m_record, tr("Record"));
    m_side->addTab(m_value, tr("Value"));
    m_side->hide();

    m_splitter = new QSplitter(Qt::Horizontal, m_resultPage);
    m_splitter->addWidget(m_grid);
    m_splitter->addWidget(m_side);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(2);

    auto *layout = new QVBoxLayout(m_resultPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_splitter, 1);

    connect(m_grid->horizontalHeader(), &QHeaderView::sectionClicked, this, &ResultPanel::onHeaderClicked);
    connect(m_grid->selectionModel(), &QItemSelectionModel::currentChanged, this, &ResultPanel::updateSidePanel);
    connect(m_model, &ResultModel::changesChanged, this, &ResultPanel::updateToolbar);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &ResultPanel::updateSidePanel);
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(m_proxy, signal, this, &ResultPanel::updateToolbar);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &ResultPanel::updateToolbar);
    updateToolbar();
}

QSortFilterProxyModel *ResultPanel::viewModel() const
{
    return m_proxy;
}

void ResultPanel::updateToolbar()
{
    const bool hasResult = m_model->columnCount() > 0;
    const bool editing = m_model->isEditing();
    const int pending = m_model->pendingChangeCount();
    m_filter->setEnabled(hasResult);
    m_fetchAllAction->setVisible(m_model->hasMore() || m_fetchAllPending);
    m_fetchAllAction->setEnabled(!m_fetchAllPending);
    m_editAction->setEnabled(hasResult);
    {
        QSignalBlocker blocker(m_editAction);
        m_editAction->setChecked(editing || m_editWaiting);
    }
    for (QAction *action : {m_addRowAction, m_deleteRowsAction, m_setNullAction, m_revertAction, m_applyAction}) {
        action->setVisible(editing);
        action->setEnabled(editing);
    }
    m_revertAction->setEnabled(editing && pending > 0);
    m_applyAction->setEnabled(editing && pending > 0);
    m_clearFiltersAction->setEnabled(m_proxy->isFiltering());
    m_filterByValueAction->setEnabled(hasResult);

    const QLocale locale;
    QString rows;
    if (m_editWaiting)
        rows = tr("Checking the table...");
    else if (editing && pending > 0)
        rows = tr("%n change(s) not applied", nullptr, pending);
    else if (hasResult && m_proxy->isFiltering())
        rows = tr("%1 of %2%3 rows").arg(locale.toString(m_proxy->rowCount()), locale.toString(m_model->rowCount()),
                                        m_model->hasMore() ? QStringLiteral("+") : QString());
    m_rowsLabel->setText(rows);
    m_editAction->setToolTip(editing ? tr("Editing %1. Click again to stop editing").arg(m_editTable)
                                     : tr("Edit a single-table result and save the changes as UPDATE, INSERT and DELETE"));
}

void ResultPanel::setQuickFilter(const QString &text)
{
    if (m_filter->text() != text) {
        QSignalBlocker blocker(m_filter);
        m_filter->setText(text);
    }
    m_proxy->setWords(text);
    updateToolbar();
}

void ResultPanel::filterByValue(int row, int column)
{
    if (row < 0 || column < 0 || column >= m_model->columnCount())
        return;
    m_proxy->addColumnFilter(column, m_model->value(row, column));
    updateToolbar();
}

void ResultPanel::clearFilters()
{
    {
        QSignalBlocker blocker(m_filter);
        m_filter->clear();
    }
    m_proxy->clear();
    updateToolbar();
}

void ResultPanel::sortByColumn(int column, Qt::SortOrder order)
{
    m_sortColumn = column;
    m_sortOrder = order;
    m_proxy->sort(column, order);
    QHeaderView *header = m_grid->horizontalHeader();
    header->setSortIndicatorShown(column >= 0);
    if (column >= 0)
        header->setSortIndicator(column, order);
}

void ResultPanel::onHeaderClicked(int column)
{
    // 오름차순 → 내림차순 → 원래 순서
    if (column != m_sortColumn)
        sortByColumn(column, Qt::AscendingOrder);
    else if (m_sortOrder == Qt::AscendingOrder)
        sortByColumn(column, Qt::DescendingOrder);
    else
        sortByColumn(-1, Qt::AscendingOrder);
}

void ResultPanel::fetchAll()
{
    if (!m_model->hasMore() || m_fetchAllPending)
        return;
    m_fetchAllPending = true;
    setInfo(tr("Fetching all rows: %1 ...").arg(QLocale().toString(m_model->rowCount())));
    updateToolbar();
    emit fetchAllRequested();
}

void ResultPanel::setRecordViewVisible(bool visible)
{
    m_side->setVisible(visible);
    if (m_recordAction->isChecked() != visible) {
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(visible);
    }
    if (visible && m_splitter->sizes().value(1) < 200)
        m_splitter->setSizes({qMax(200, m_splitter->width() - 340), 340});
    updateSidePanel();
}

void ResultPanel::updateSidePanel()
{
    if (!m_side->isVisible())
        return;
    const QModelIndex current = m_proxy->mapToSource(m_grid->currentIndex());
    const int columns = m_model->columnCount();
    m_record->setRowCount(current.isValid() ? columns : 0);
    if (!current.isValid()) {
        m_value->clear();
        return;
    }
    const QColor dim = palette().color(QPalette::PlaceholderText);
    for (int c = 0; c < columns; ++c) {
        auto *name = new QTableWidgetItem(m_model->columnName(c));
        const QCborValue value = m_model->value(current.row(), c);
        QString text = ResultExport::valueText(value);
        text.replace(u'\n', QStringLiteral(" ⏎ "));
        auto *item = new QTableWidgetItem(isNullValue(value) ? QStringLiteral("<null>") : text.left(2000));
        if (isNullValue(value))
            item->setForeground(dim);
        item->setToolTip(ResultExport::valueText(value).left(4000));
        m_record->setItem(c, 0, name);
        m_record->setItem(c, 1, item);
        if (c == current.column()) {
            name->setSelected(true);
            item->setSelected(true);
        }
    }
    m_record->resizeColumnToContents(0);

    // 지금 셀의 값을 통째로. JSON이면 보기 좋게 펼친다.
    const QCborValue value = m_model->value(current.row(), current.column());
    QString full = isNullValue(value) ? QStringLiteral("<null>") : ResultExport::valueText(value);
    const QString trimmed = full.trimmed();
    if (trimmed.startsWith(u'{') || trimmed.startsWith(u'[')) {
        QJsonParseError error;
        const QJsonDocument json = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError)
            full = QString::fromUtf8(json.toJson(QJsonDocument::Indented)).trimmed();
    }
    m_value->setPlainText(full);
}

void ResultPanel::setEditMode(bool on)
{
    if (on) {
        if (m_model->isEditing() || m_editWaiting)
            return;
        if (m_model->columnCount() == 0) {
            updateToolbar();
            return;
        }
        m_editWaiting = true;
        updateToolbar();
        emit editRequested(); // QueryTab이 테이블과 기본 키를 알아낸다
        return;
    }
    if (m_editWaiting) {
        m_editWaiting = false;
        updateToolbar();
        return;
    }
    if (!m_model->isEditing())
        return;
    if (hasPendingEdits()) {
        const auto answer = QMessageBox::question(
            this, tr("Edit Result"), tr("Discard %n change(s) that are not applied?", nullptr, m_model->pendingChangeCount()),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) {
            updateToolbar();
            return;
        }
    }
    discardEdits();
}

void ResultPanel::beginEditing(const QString &table, const QList<bool> &editableColumns, const QList<int> &keyColumns)
{
    m_editWaiting = false;
    m_editTable = table;
    m_editKeys = keyColumns;
    m_model->setEditing(true, editableColumns);
    m_grid->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::AnyKeyPressed);
    updateToolbar();
}

void ResultPanel::editUnavailable(const QString &reason)
{
    m_editWaiting = false;
    updateToolbar();
    QMessageBox::information(this, tr("Edit Result"), reason);
}

bool ResultPanel::isEditing() const
{
    return m_model->isEditing();
}

bool ResultPanel::hasPendingEdits() const
{
    return m_model->isEditing() && m_model->pendingChangeCount() > 0;
}

QStringList ResultPanel::editStatements(QString *error) const
{
    QList<ExportColumn> columns;
    QList<bool> editable;
    for (int c = 0; c < m_model->columnCount(); ++c) {
        const ResultModel::Column &column = m_model->columns().at(c);
        columns.append({column.name, column.typeName, column.numeric});
        editable.append(m_model->isColumnEditable(c));
    }
    return ResultEdits::statements(m_editTable, columns, editable, m_editKeys, m_model->changes(), m_dialect, error);
}

void ResultPanel::acceptEdits()
{
    m_model->acceptChanges();
    updateToolbar();
}

void ResultPanel::discardEdits()
{
    m_model->revertChanges();
    m_model->setEditing(false);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_editTable.clear();
    m_editKeys.clear();
    updateToolbar();
}

void ResultPanel::previewAndApply()
{
    QString error;
    const QStringList statements = editStatements(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Apply Changes"), error);
        return;
    }
    if (statements.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("applyEditsDialog"));
    dialog.setWindowTitle(tr("Apply Changes"));
    auto *intro = new QLabel(tr("These %n statement(s) will run in this tab's session. With manual commit they stay "
                                "uncommitted until you commit.", nullptr, int(statements.size())),
                             &dialog);
    intro->setWordWrap(true);
    auto *view = new QPlainTextEdit(&dialog);
    view->setObjectName(QStringLiteral("applyEditsSql"));
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *highlighter = new SqlHighlighter(view->document());
    highlighter->setDialect(m_dialect);
    highlighter->setDarkTheme(Theme::current() == Theme::Kind::Dark);
    view->setPlainText(statements.join(QStringLiteral(";\n")) + u';');
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(intro);
    layout->addWidget(view, 1);
    layout->addWidget(buttons);
    dialog.resize(820, 440);
    if (dialog.exec() == QDialog::Accepted)
        emit applyEditsRequested(statements);
}

void ResultPanel::addRow()
{
    if (!m_model->isEditing())
        return;
    const int row = m_model->appendEmptyRow();
    int column = 0;
    while (column < m_model->columnCount() && !m_model->isColumnEditable(column))
        ++column;
    const QModelIndex index = m_proxy->mapFromSource(m_model->index(row, qMin(column, m_model->columnCount() - 1)));
    m_grid->scrollTo(index);
    m_grid->setCurrentIndex(index);
    m_grid->setFocus();
}

QList<int> ResultPanel::selectedSourceRows() const
{
    QSet<int> rows;
    for (const QModelIndex &index : m_grid->selectionModel()->selectedIndexes())
        rows.insert(m_proxy->mapToSource(index).row());
    if (rows.isEmpty() && m_grid->currentIndex().isValid())
        rows.insert(m_proxy->mapToSource(m_grid->currentIndex()).row());
    QList<int> list(rows.begin(), rows.end());
    std::sort(list.begin(), list.end());
    return list;
}

void ResultPanel::deleteRows()
{
    if (m_model->isEditing())
        m_model->markDeleted(selectedSourceRows());
}

void ResultPanel::setSelectedNull()
{
    if (!m_model->isEditing())
        return;
    QModelIndexList selected = m_grid->selectionModel()->selectedIndexes();
    if (selected.isEmpty() && m_grid->currentIndex().isValid())
        selected << m_grid->currentIndex();
    for (const QModelIndex &index : std::as_const(selected)) {
        const QModelIndex source = m_proxy->mapToSource(index);
        if (m_model->isColumnEditable(source.column()))
            m_model->setCellValue(source.row(), source.column(), QCborValue(QCborValue::Null));
    }
}

QString ResultPanel::formatDuration(qint64 ms)
{
    if (ms < 1000)
        return tr("%1 ms").arg(ms);
    if (ms < 60'000)
        return tr("%1 s").arg(QString::number(ms / 1000.0, 'f', ms < 10'000 ? 2 : 1));
    const qint64 seconds = ms / 1000;
    if (seconds < 3600)
        return tr("%1 min %2 s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return tr("%1 h %2 min").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'));
}

void ResultPanel::setInfo(const QString &text, const QString &toolTip)
{
    m_info->setText(text);
    m_info->setToolTip(toolTip);
    placeInfo();
}

void ResultPanel::placeInfo()
{
    m_info->adjustSize();
    const QRect bar = tabBar()->geometry();
    const int x = qMax(bar.left() + tabBar()->sizeHint().width() + 12, width() - m_info->width() - 10);
    m_info->move(x, bar.top() + (bar.height() - m_info->height()) / 2);
    m_info->raise();
}

void ResultPanel::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    placeInfo();
}

QString ResultPanel::infoText() const
{
    return m_info->text();
}

void ResultPanel::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    if (running) {
        m_infoDuringRun = false;
        m_runClock.start();
        m_runningTimer->start();
        updateRunningText();
        return;
    }
    m_runningTimer->stop();
    if (m_infoDuringRun) {
        showExecutionInfo(m_lastInfo);
    } else {
        // 오류나 취소로 끝나서 결과 요약이 없으면 걸린 시간만 보여 준다.
        setInfo(tr("Elapsed: %1").arg(formatDuration(m_runClock.elapsed())));
    }
}

void ResultPanel::updateRunningText()
{
    setInfo(tr("Running...  %1").arg(formatDuration(m_runClock.elapsed())));
}

void ResultPanel::showExecutionInfo(const ExecutionInfo &info)
{
    m_lastInfo = info;
    if (m_running) {
        m_infoDuringRun = true; // 스크립트가 끝나면 마지막 문장의 요약을 보여 준다
        return;
    }

    const QLocale locale;
    const QString elapsed = tr("Elapsed: %1").arg(formatDuration(info.totalMs()));
    QString text;
    QString tip;
    switch (info.kind) {
    case ExecutionInfo::Kind::ResultSet:
        text = tr("Rows: %1%2").arg(locale.toString(info.rows), info.hasMore ? QStringLiteral("+") : QString());
        tip = tr("Execution: %1, fetching: %2").arg(formatDuration(info.executeMs), formatDuration(info.fetchMs));
        if (info.hasMore)
            tip += u'\n' + tr("%1 rows loaded so far. Scroll down to load more.").arg(locale.toString(info.rows));
        break;
    case ExecutionInfo::Kind::Update:
        text = tr("Affected rows: %1").arg(locale.toString(info.rows));
        break;
    case ExecutionInfo::Kind::Plan:
        text = tr("Estimated plan");
        break;
    case ExecutionInfo::Kind::Other:
    case ExecutionInfo::Kind::None:
        break;
    }
    setInfo(text.isEmpty() ? elapsed : text + QStringLiteral("     ") + elapsed, tip);
}

void ResultPanel::showPlan(const QCborMap &plan)
{
    if (indexOf(m_plan) < 0) {
        addTab(m_plan, tr("Plan"));
        placeInfo(); // 탭이 늘었으니 자리를 다시 잡는다
    }
    m_plan->setPlan(plan);
    setCurrentWidget(m_plan);
}

void ResultPanel::startResult(const QCborArray &columns)
{
    if (m_exportPending)
        cancelExport(tr("Export cancelled: a new result replaced the one being exported."));
    // 새 결과: 편집·거르기·정렬은 처음부터
    m_fetchAllPending = false;
    m_editWaiting = false;
    m_editTable.clear();
    m_editKeys.clear();
    m_model->reset(columns);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clearFilters();
    sortByColumn(-1, Qt::AscendingOrder);
    m_columnsFitted = false;
    updateToolbar();
    updateSidePanel();
    showResult();
}

void ResultPanel::appendRows(const QCborArray &rows, bool hasMore)
{
    m_model->appendRows(rows, hasMore);
    if (!m_columnsFitted) {
        fitColumns();
        m_columnsFitted = true;
    }
    if (m_fetchAllPending) {
        if (m_model->hasMore()) {
            setInfo(tr("Fetching all rows: %1 ...").arg(QLocale().toString(m_model->rowCount())));
            emit fetchAllRequested();
        } else {
            m_fetchAllPending = false;
            showExecutionInfo(m_lastInfo);
        }
    }
    updateToolbar();
    if (!m_exportPending)
        return;
    if (m_model->hasMore()) {
        setInfo(tr("Fetching rows for export: %1 ...").arg(QLocale().toString(m_model->rowCount())));
        emit fetchAllRequested();
    } else {
        m_exportPending = false;
        finishExport();
    }
}

QString ResultPanel::suggestedTableName() const
{
    // 결과를 만든 문장의 FROM 뒤 첫 이름 (SELECT * FROM hr.emp → hr.emp)
    static const QRegularExpression from(QStringLiteral("\\bFROM\\s+((?:\"[^\"]+\"|`[^`]+`|[\\w$#]+)(?:\\.(?:\"[^\"]+\"|`[^`]+`|[\\w$#]+))?)"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = from.match(m_resultSql);
    return match.hasMatch() ? match.captured(1) : QString();
}

void ResultPanel::showExportDialog()
{
    if (m_model->columnCount() == 0) {
        appendLog(tr("There is no result to export. Run a query first."), true);
        showLog();
        return;
    }
    const bool hasSelection = m_grid->selectionModel() && m_grid->selectionModel()->hasSelection();
    ExportDialog dialog(hasSelection, m_model->hasMore(), suggestedTableName(), m_dialect, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    exportResults(dialog.options(), dialog.selectionOnly(), dialog.toClipboard() ? QString() : dialog.filePath());
}

void ResultPanel::exportResults(const ExportOptions &options, bool selectionOnly, const QString &filePath)
{
    if (m_model->columnCount() == 0) {
        emit exportFinished(tr("There is no result to export."), false);
        return;
    }
    if (filePath.isEmpty() && ResultExport::isBinary(options.format)) {
        emit exportFinished(tr("%1 can only be exported to a file.").arg(ResultExport::formatName(options.format)), false);
        return;
    }
    m_exportOptions = options;
    m_exportSelectionOnly = selectionOnly;
    m_exportPath = filePath;
    if (!selectionOnly && m_model->hasMore()) {
        m_exportPending = true;
        setInfo(tr("Fetching rows for export: %1 ...").arg(QLocale().toString(m_model->rowCount())));
        emit fetchAllRequested();
        return;
    }
    finishExport();
}

void ResultPanel::cancelExport(const QString &reason)
{
    m_exportPending = false;
    appendLog(reason, true);
    emit exportFinished(reason, false);
}

void ResultPanel::finishExport()
{
    // 고른 범위의 컬럼·행만 모은다 (선택한 셀이 걸친 컬럼과 행). 행은 보이는(거르고 정렬한) 순서대로.
    QList<int> columnIndexes, viewRows;
    if (m_exportSelectionOnly && m_grid->selectionModel()) {
        QSet<int> columns, rows;
        for (const QModelIndex &index : m_grid->selectionModel()->selectedIndexes()) {
            columns.insert(index.column());
            rows.insert(index.row());
        }
        columnIndexes = QList<int>(columns.begin(), columns.end());
        viewRows = QList<int>(rows.begin(), rows.end());
        std::sort(columnIndexes.begin(), columnIndexes.end());
        std::sort(viewRows.begin(), viewRows.end());
    } else {
        for (int c = 0; c < m_model->columnCount(); ++c)
            columnIndexes << c;
        for (int r = 0; r < m_proxy->rowCount(); ++r)
            viewRows << r;
    }
    QList<int> rowIndexes;
    for (int r : std::as_const(viewRows))
        rowIndexes << m_proxy->mapToSource(m_proxy->index(r, 0)).row();

    QList<ExportColumn> columns;
    for (int c : std::as_const(columnIndexes)) {
        const ResultModel::Column &column = m_model->columns().at(c);
        columns.append({column.name, column.typeName, column.numeric});
    }
    QList<QCborArray> rows;
    rows.reserve(rowIndexes.size());
    for (int r : std::as_const(rowIndexes)) {
        const QCborArray &source = m_model->rows().at(r);
        QCborArray row;
        for (int c : std::as_const(columnIndexes))
            row.append(source.at(c));
        rows.append(row);
    }

    const QByteArray bytes = ResultExport::build(columns, rows, m_exportOptions);
    const QString what = tr("%n row(s)", nullptr, int(rows.size()));
    const QString format = ResultExport::formatName(m_exportOptions.format);
    QString message;
    bool ok = true;
    if (m_exportPath.isEmpty()) {
        QByteArray text = bytes;
        if (text.startsWith("\xEF\xBB\xBF"))
            text.remove(0, 3);
        QGuiApplication::clipboard()->setText(QString::fromUtf8(text));
        message = tr("Copied %1 as %2 to the clipboard.").arg(what, format);
    } else {
        QSaveFile file(m_exportPath);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
            message = tr("Exported %1 as %2 to %3").arg(what, format, QDir::toNativeSeparators(m_exportPath));
        } else {
            ok = false;
            message = tr("Could not export to %1: %2").arg(QDir::toNativeSeparators(m_exportPath), file.errorString());
        }
    }
    appendLog(message, !ok);
    showExecutionInfo(m_lastInfo);
    emit exportFinished(message, ok);
}

void ResultPanel::appendLog(const QString &message, bool isError)
{
    if (isError && m_exportPending) {
        // 남은 행을 가져오다 실패했다 (연결 끊김 등)
        m_exportPending = false;
        emit exportFinished(tr("Export cancelled: %1").arg(message), false);
    }
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    const QString prefix = QStringLiteral("[%1] %2").arg(time, isError ? QStringLiteral("ERROR ") : QString());
    // 여러 줄 메시지(SQL 문장, 여러 줄 오류)는 이어지는 줄을 메시지 시작 위치에 맞춘다.
    QString text = message;
    text.remove(u'\r');
    text.replace(u'\n', u'\n' + QString(prefix.size(), u' '));
    m_log->appendPlainText(prefix + text);
}

void ResultPanel::appendOutput(const QStringList &lines)
{
    for (const QString &line : lines)
        m_output->appendPlainText(line);
    setTabText(indexOf(m_output), tr("Output (%1)").arg(m_output->blockCount()));
}

void ResultPanel::clearLog()
{
    m_log->clear();
}

void ResultPanel::clearOutput()
{
    m_output->clear();
    setTabText(indexOf(m_output), tr("Output"));
}

void ResultPanel::showLog()
{
    setCurrentWidget(m_log);
}

void ResultPanel::showResult()
{
    setCurrentWidget(m_resultPage);
}

void ResultPanel::fitColumns()
{
    // 처음 받은 행 기준으로 폭을 맞추되(셀·머리글 여백 포함), 너무 좁거나 넓은 컬럼은 맞춘다.
    m_grid->resizeColumnsToContents();
    const int minWidth = m_grid->fontMetrics().horizontalAdvance(QStringLiteral("0000000")) + 2 * kCellPadding;
    const int maxWidth = qMax(280, m_grid->viewport()->width() / 2);
    for (int column = 0; column < m_model->columnCount(); ++column)
        m_grid->setColumnWidth(column, qBound(minWidth, m_grid->columnWidth(column), maxWidth));
}

void ResultPanel::copySelection(bool withHeader)
{
    const QModelIndexList selected = m_grid->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return;

    // 보이는(거르고 정렬한) 순서대로 복사하고, 값은 원본에서 읽는다
    int top = INT_MAX, bottom = -1, left = INT_MAX, right = -1;
    QSet<QPair<int, int>> cells;
    for (const QModelIndex &index : selected) {
        top = qMin(top, index.row());
        bottom = qMax(bottom, index.row());
        left = qMin(left, index.column());
        right = qMax(right, index.column());
        cells.insert({index.row(), index.column()});
    }
    auto sourceRow = [this](int viewRow) { return m_proxy->mapToSource(m_proxy->index(viewRow, 0)).row(); };

    QStringList lines;
    if (withHeader) {
        QStringList header;
        for (int column = left; column <= right; ++column)
            header << m_model->columnName(column);
        lines << header.join(u'\t');
    }
    for (int row = top; row <= bottom; ++row) {
        QStringList values;
        for (int column = left; column <= right; ++column) {
            QString text = cells.contains({row, column}) ? m_model->copyText(sourceRow(row), column) : QString();
            text.replace(u'\t', u' ').replace(u'\n', u' ').replace(u'\r', u' ');
            values << text;
        }
        lines << values.join(u'\t');
    }
    QGuiApplication::clipboard()->setText(lines.join(u'\n'));
}
