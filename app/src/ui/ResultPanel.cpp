#include "ResultPanel.h"

#include "ExportDialog.h"
#include "PlanView.h"
#include "ResultModel.h"
#include "ScrollHelpers.h"

#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSet>
#include <QTableView>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <climits>

namespace {
constexpr int kCellPadding = 10; // 결과 셀 글자 좌우 여백 (px). Theme 스타일시트의 #resultGrid 여백과 같게 둔다
} // namespace

ResultPanel::ResultPanel(QWidget *parent)
    : QTabWidget(parent)
{
    setDocumentMode(true);

    m_model = new ResultModel(this);
    connect(m_model, &ResultModel::moreRowsRequested, this, &ResultPanel::moreRowsRequested);

    m_grid = new QTableView(this);
    m_grid->setObjectName(QStringLiteral("resultGrid"));
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setModel(m_model);
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
    auto *separator = new QAction(m_grid);
    separator->setSeparator(true);
    auto *exportAction = new QAction(tr("Export..."), m_grid);
    connect(exportAction, &QAction::triggered, this, &ResultPanel::showExportDialog);
    m_grid->addActions({copy, copyWithHeader, separator, exportAction});
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

    for (QAbstractScrollArea *area : {static_cast<QAbstractScrollArea *>(m_grid), static_cast<QAbstractScrollArea *>(m_log),
                                      static_cast<QAbstractScrollArea *>(m_output)})
        enableShiftWheelHorizontalScroll(area);

    addTab(m_grid, tr("Result"));
    addTab(m_log, tr("Log"));
    addTab(m_output, tr("Output"));

    m_plan = new PlanView(this);
    m_plan->setObjectName(QStringLiteral("resultPlan"));
    m_plan->hide(); // 처음 Ctrl+E를 누를 때 탭으로 붙인다

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
    m_model->reset(columns);
    m_columnsFitted = false;
    showResult();
}

void ResultPanel::appendRows(const QCborArray &rows, bool hasMore)
{
    m_model->appendRows(rows, hasMore);
    if (!m_columnsFitted) {
        fitColumns();
        m_columnsFitted = true;
    }
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
    // 고른 범위의 컬럼·행만 모은다 (선택한 셀이 걸친 컬럼과 행).
    QList<int> columnIndexes, rowIndexes;
    if (m_exportSelectionOnly && m_grid->selectionModel()) {
        QSet<int> columns, rows;
        for (const QModelIndex &index : m_grid->selectionModel()->selectedIndexes()) {
            columns.insert(index.column());
            rows.insert(index.row());
        }
        columnIndexes = QList<int>(columns.begin(), columns.end());
        rowIndexes = QList<int>(rows.begin(), rows.end());
        std::sort(columnIndexes.begin(), columnIndexes.end());
        std::sort(rowIndexes.begin(), rowIndexes.end());
    } else {
        for (int c = 0; c < m_model->columnCount(); ++c)
            columnIndexes << c;
        for (int r = 0; r < m_model->rowCount(); ++r)
            rowIndexes << r;
    }

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

void ResultPanel::showLog()
{
    setCurrentWidget(m_log);
}

void ResultPanel::showResult()
{
    setCurrentWidget(m_grid);
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

    int top = INT_MAX, bottom = -1, left = INT_MAX, right = -1;
    QSet<QPair<int, int>> cells;
    for (const QModelIndex &index : selected) {
        top = qMin(top, index.row());
        bottom = qMax(bottom, index.row());
        left = qMin(left, index.column());
        right = qMax(right, index.column());
        cells.insert({index.row(), index.column()});
    }

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
            QString text = cells.contains({row, column}) ? m_model->copyText(row, column) : QString();
            text.replace(u'\t', u' ').replace(u'\n', u' ').replace(u'\r', u' ');
            values << text;
        }
        lines << values.join(u'\t');
    }
    QGuiApplication::clipboard()->setText(lines.join(u'\n'));
}
