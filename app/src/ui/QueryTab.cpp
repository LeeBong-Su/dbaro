#include "QueryTab.h"

#include "QuerySession.h"
#include "ResultPanel.h"
#include "SqlEditor.h"

#include <QColor>
#include <QFrame>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

QueryTab::QueryTab(EngineClient *engine, QWidget *parent)
    : QWidget(parent)
{
    m_colorStrip = new QFrame(this);
    m_colorStrip->setFixedHeight(3);
    m_colorStrip->hide();

    m_editor = new SqlEditor(this);
    m_editor->setObjectName(QStringLiteral("sqlEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setPlaceholderText(tr("Write SQL here. Ctrl+Enter runs the current statement, F5 runs the script."));

    m_results = new ResultPanel(this);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(m_editor);
    m_splitter->addWidget(m_results);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(2);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_colorStrip);
    layout->addWidget(m_splitter);

    connect(m_editor, &SqlEditor::cursorPositionChanged, this, [this] {
        const QTextCursor cursor = m_editor->textCursor();
        emit cursorPositionChanged(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
    });

    // 세션 → 결과 패널
    m_session = new QuerySession(engine, this);
    connect(m_session, &QuerySession::message, m_results, &ResultPanel::appendLog);
    connect(m_session, &QuerySession::serverOutput, m_results, &ResultPanel::appendOutput);
    connect(m_session, &QuerySession::resultStarted, m_results, &ResultPanel::startResult);
    connect(m_session, &QuerySession::rowsReceived, m_results, &ResultPanel::appendRows);
    connect(m_session, &QuerySession::statementFailed, this, &QueryTab::showError);
    connect(m_results, &ResultPanel::moreRowsRequested, m_session, [this] { m_session->fetchMore(); });
}

void QueryTab::setDialect(SqlDialect dialect)
{
    m_editor->setDialect(dialect);
}

void QueryTab::setConnectionColor(const QColor &color)
{
    if (!color.isValid()) {
        m_colorStrip->hide();
        return;
    }
    m_colorStrip->setStyleSheet(QStringLiteral("background-color: %1;").arg(color.name()));
    m_colorStrip->show();
}

void QueryTab::showError(const SqlStatement &statement, const EngineError &error)
{
    // 오류 위치(없으면 문장 시작)로 커서를 옮기고 그 단어를 선택한다.
    QTextDocument *document = m_editor->document();
    int position = statement.start;
    if (error.position >= 0) {
        position = statement.start + error.position;
    } else if (error.line >= 1) {
        const QTextBlock first = document->findBlock(statement.start);
        const QTextBlock target = document->findBlockByNumber(first.blockNumber() + error.line - 1);
        if (target.isValid())
            position = qMax(statement.start, target.position());
    }
    position = qBound(0, position, qMax(0, document->characterCount() - 1));

    QTextCursor cursor(document);
    cursor.setPosition(position);
    cursor.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
    m_results->showLog();
}
