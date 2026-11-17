#include "ResultPanel.h"

#include "ResultModel.h"

#include <QAction>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QPlainTextEdit>
#include <QSet>
#include <QTableView>
#include <QTime>

#include <climits>

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
    m_grid->verticalHeader()->setDefaultSectionSize(m_grid->fontMetrics().height() + 8);
    m_grid->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    // 복사: Ctrl+C는 값만, Ctrl+Shift+C는 컬럼 이름 포함 (탭으로 구분 → 엑셀에 바로 붙여넣기)
    auto *copy = new QAction(tr("Copy"), m_grid);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    connect(copy, &QAction::triggered, this, [this] { copySelection(false); });
    auto *copyWithHeader = new QAction(tr("Copy with Header"), m_grid);
    copyWithHeader->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyWithHeader->setShortcutContext(Qt::WidgetShortcut);
    connect(copyWithHeader, &QAction::triggered, this, [this] { copySelection(true); });
    m_grid->addActions({copy, copyWithHeader});
    m_grid->setContextMenuPolicy(Qt::ActionsContextMenu);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("resultLog"));
    m_log->setFrameShape(QFrame::NoFrame);
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setPlaceholderText(tr("Execution log will appear here."));

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("resultOutput"));
    m_output->setFrameShape(QFrame::NoFrame);
    m_output->setReadOnly(true);
    m_output->setFont(mono);
    m_output->setPlaceholderText(
        tr("Server output (DBMS_OUTPUT, RAISE NOTICE, warnings) will appear here."));

    addTab(m_grid, tr("Result"));
    addTab(m_log, tr("Log"));
    addTab(m_output, tr("Output"));
}

void ResultPanel::startResult(const QCborArray &columns)
{
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
}

void ResultPanel::appendLog(const QString &message, bool isError)
{
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->appendPlainText(QStringLiteral("[%1] %2%3").arg(time, isError ? QStringLiteral("ERROR ") : QString(), message));
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
    // 처음 받은 행 기준으로 폭을 맞추되, 너무 넓은 컬럼은 줄인다.
    m_grid->resizeColumnsToContents();
    const int maxWidth = qMax(240, m_grid->viewport()->width() / 2);
    for (int column = 0; column < m_model->columnCount(); ++column) {
        if (m_grid->columnWidth(column) > maxWidth)
            m_grid->setColumnWidth(column, maxWidth);
    }
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
