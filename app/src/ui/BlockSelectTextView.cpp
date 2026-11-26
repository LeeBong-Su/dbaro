#include "BlockSelectTextView.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>

BlockSelectTextView::BlockSelectTextView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setReadOnly(true);
    // 글을 새로 넣으면 고른 사각형은 의미가 없다
    connect(this, &QPlainTextEdit::textChanged, this, &BlockSelectTextView::clearBlockSelection);
}

void BlockSelectTextView::selectBlock(const QPoint &from, const QPoint &to)
{
    const int scroll = horizontalScrollBar()->value();
    m_anchorLine = cursorForPosition(from).blockNumber();
    m_anchorX = from.x() + scroll;
    m_headLine = cursorForPosition(to).blockNumber();
    m_headX = to.x() + scroll;
    updateBlockSelection();
}

void BlockSelectTextView::updateBlockSelection()
{
    // 줄마다 화면의 같은 가로 범위에 들어오는 글자
    m_blockCursors.clear();
    const int first = qMin(m_anchorLine, m_headLine);
    const int last = qMax(m_anchorLine, m_headLine);
    const int scroll = horizontalScrollBar()->value();
    const int left = int(qMin(m_anchorX, m_headX)) - scroll;
    const int right = int(qMax(m_anchorX, m_headX)) - scroll;
    QList<QTextEdit::ExtraSelection> selections;
    for (int number = first; number <= last; ++number) {
        const QTextBlock block = document()->findBlockByNumber(number);
        if (!block.isValid())
            break;
        const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
        const int y = int(geometry.top() + geometry.height() / 2);
        const int lineEnd = block.position() + block.length() - 1;
        const int start = qBound(block.position(), cursorForPosition(QPoint(left, y)).position(), lineEnd);
        const int end = qBound(block.position(), cursorForPosition(QPoint(right, y)).position(), lineEnd);
        QTextCursor line(document());
        line.setPosition(start);
        line.setPosition(end, QTextCursor::KeepAnchor);
        m_blockCursors << line;
        if (line.hasSelection()) {
            QTextEdit::ExtraSelection selection;
            selection.cursor = line;
            selection.format.setBackground(palette().color(QPalette::Highlight));
            selection.format.setForeground(palette().color(QPalette::HighlightedText));
            selections << selection;
        }
    }
    setExtraSelections(selections);
}

QString BlockSelectTextView::blockSelectionText() const
{
    QStringList lines;
    for (const QTextCursor &line : m_blockCursors)
        lines << line.selectedText();
    return lines.join(u'\n');
}

void BlockSelectTextView::clearBlockSelection()
{
    if (m_blockCursors.isEmpty() && !m_blockDragging)
        return;
    m_blockCursors.clear();
    m_blockDragging = false;
    setExtraSelections({});
}

void BlockSelectTextView::copySelection()
{
    if (hasBlockSelection())
        QGuiApplication::clipboard()->setText(blockSelectionText());
    else if (textCursor().hasSelection())
        copy();
}

void BlockSelectTextView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        return;
    }
    if (event->key() == Qt::Key_Escape && hasBlockSelection()) {
        clearBlockSelection();
        return;
    }
    // 수식 키만 누른 것이 아니면 사각형 선택은 푼다
    if (event->key() != Qt::Key_Control && event->key() != Qt::Key_Shift && event->key() != Qt::Key_Alt
        && event->key() != Qt::Key_Meta)
        clearBlockSelection();
    QPlainTextEdit::keyPressEvent(event);
}

void BlockSelectTextView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier)) {
        // Alt+드래그: 사각형 선택. 보통 선택은 푼다
        QTextCursor cursor = textCursor();
        if (cursor.hasSelection()) {
            cursor.clearSelection();
            setTextCursor(cursor);
        }
        m_blockDragging = true;
        selectBlock(event->position().toPoint(), event->position().toPoint());
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    clearBlockSelection();
    QPlainTextEdit::mousePressEvent(event);
}

void BlockSelectTextView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_blockDragging) {
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }
    // 화면 밖으로 끌면 그쪽으로 조금씩 스크롤한다
    const QPoint position = event->position().toPoint();
    const QRect area = viewport()->rect();
    if (position.y() < area.top())
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 1);
    else if (position.y() > area.bottom())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 1);
    if (position.x() < area.left())
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - fontMetrics().averageCharWidth());
    else if (position.x() > area.right())
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + fontMetrics().averageCharWidth());
    const QPoint clamped(qBound(area.left(), position.x(), area.right()), qBound(area.top(), position.y(), area.bottom()));
    m_headLine = cursorForPosition(clamped).blockNumber();
    m_headX = clamped.x() + horizontalScrollBar()->value();
    updateBlockSelection();
    event->accept();
}

void BlockSelectTextView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_blockDragging) {
        m_blockDragging = false;
        event->accept();
        return;
    }
    QPlainTextEdit::mouseReleaseEvent(event);
}

void BlockSelectTextView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, &BlockSelectTextView::copySelection)
        ->setEnabled(hasBlockSelection() || textCursor().hasSelection());
    menu.addAction(tr("Select All"), QKeySequence::SelectAll, this, [this] {
        clearBlockSelection();
        selectAll();
    });
    menu.exec(event->globalPos());
}
