#include "QueryTab.h"

#include "ResultPanel.h"

#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFrame>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTextCursor>
#include <QVBoxLayout>

QueryTab::QueryTab(QWidget *parent)
    : QWidget(parent)
{
    m_colorStrip = new QFrame(this);
    m_colorStrip->setFixedHeight(4);
    m_colorStrip->hide();

    // 임시 에디터. 에디터 구현 방식(직접 구현 또는 KTextEditor)이 정해지면 교체한다.
    m_editor = new QPlainTextEdit(this);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(11);
    m_editor->setFont(mono);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
    m_editor->setPlaceholderText(tr("Write SQL here. Ctrl+Enter runs the current statement."));

    m_results = new ResultPanel(this);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(m_editor);
    m_splitter->addWidget(m_results);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_colorStrip);
    layout->addWidget(m_splitter);

    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const QTextCursor cursor = m_editor->textCursor();
        emit cursorPositionChanged(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
    });
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
