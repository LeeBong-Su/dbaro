#include "ResultPanel.h"

#include <QFontDatabase>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTableView>
#include <QTime>

ResultPanel::ResultPanel(QWidget *parent)
    : QTabWidget(parent)
{
    setDocumentMode(true);

    // 임시 그리드. 대용량 결과용 가상 스크롤 그리드로 교체한다.
    m_grid = new QTableView(this);
    m_grid->setAlternatingRowColors(true);
    m_grid->horizontalHeader()->setHighlightSections(false);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setPlaceholderText(tr("Execution log will appear here."));

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(mono);
    m_output->setPlaceholderText(
        tr("Server output (DBMS_OUTPUT, RAISE NOTICE, warnings) will appear here."));

    addTab(m_grid, tr("Result"));
    addTab(m_log, tr("Log"));
    addTab(m_output, tr("Output"));
}

void ResultPanel::appendLog(const QString &message)
{
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(time, message));
}

void ResultPanel::showLog()
{
    setCurrentWidget(m_log);
}
