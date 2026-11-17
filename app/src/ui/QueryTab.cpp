#include "QueryTab.h"

#include "ResultPanel.h"
#include "SqlEditor.h"

#include <QColor>
#include <QFrame>
#include <QSplitter>
#include <QTextCursor>
#include <QVBoxLayout>

QueryTab::QueryTab(QWidget *parent)
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
