#include "SqlEditor.h"
#include "Theme.h"

#include "SqlCompleter.h"

#include "FontZoom.h"
#include "ScrollHelpers.h"
#include "SqlHighlighter.h"

#include <QClipboard>
#include <QDropEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTimer>
#include <QWheelEvent>

namespace {

constexpr int kIndentWidth = 4;
constexpr int kMarkerWidth = 3;

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(SqlEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
    }

    QSize sizeHint() const override { return QSize(m_editor->lineNumberAreaWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->paintLineNumberArea(event); }

private:
    SqlEditor *m_editor;
};

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightness() < 128;
}

QString leadingWhitespace(const QString &line)
{
    int i = 0;
    while (i < line.size() && (line[i] == u' ' || line[i] == u'\t'))
        ++i;
    return line.left(i);
}

bool isOpenBracket(QChar c)
{
    return c == u'(' || c == u'[' || c == u'{';
}

bool isCloseBracket(QChar c)
{
    return c == u')' || c == u']' || c == u'}';
}

QChar matchingBracket(QChar c)
{
    switch (c.unicode()) {
    case u'(': return u')';
    case u')': return u'(';
    case u'[': return u']';
    case u']': return u'[';
    case u'{': return u'}';
    case u'}': return u'{';
    default: return {};
    }
}

} // namespace

SqlEditor::SqlEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    // 코딩용 고정폭 글꼴. 한글 코딩 글꼴(D2Coding)이 있으면 먼저 쓴다.
    QFont font;
    font.setFamilies({QStringLiteral("D2Coding"), QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                      QStringLiteral("Menlo"), QStringLiteral("DejaVu Sans Mono")});
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSizeF(FontZoom::instance()->pointSize(FontZoom::Target::Editor, 11)); // Ctrl + 휠로 바꾼 크기
    setFont(font);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * kIndentWidth);
    connect(FontZoom::instance(), &FontZoom::changed, this, [this](FontZoom::Target target, qreal pointSize) {
        if (target == FontZoom::Target::Editor)
            applyFontSize(pointSize);
    });

    m_lineNumberArea = new LineNumberArea(this);
    m_highlighter = new SqlHighlighter(document());
    enableShiftWheelHorizontalScroll(this);
    m_completer = new SqlCompleter(this);
    m_highlighter->setDarkTheme(isDark(palette()));

    m_parseTimer = new QTimer(this);
    m_parseTimer->setSingleShot(true);
    m_parseTimer->setInterval(150);
    connect(m_parseTimer, &QTimer::timeout, this, &SqlEditor::reparse);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &SqlEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &SqlEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &SqlEditor::onCursorPositionChanged);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        m_statementsDirty = true;
        m_parseTimer->start();
    });

    updateLineNumberAreaWidth();
    updateExtraSelections();
}

void SqlEditor::setDialect(SqlDialect dialect)
{
    if (m_dialect == dialect)
        return;
    m_dialect = dialect;
    m_highlighter->setDialect(dialect);
    reparse();
}

QList<SqlStatement> SqlEditor::statements()
{
    if (m_statementsDirty)
        reparse();
    return m_statements;
}

QList<SqlStatement> SqlEditor::statementsToExecute(bool wholeScript)
{
    if (m_statementsDirty)
        reparse();
    if (wholeScript)
        return m_statements;

    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection() && !hasExtraCursors()) {
        const int from = cursor.selectionStart();
        const int to = cursor.selectionEnd();
        QList<SqlStatement> part = SqlSplitter::split(toPlainText().mid(from, to - from), m_dialect);
        for (SqlStatement &statement : part) {
            statement.start += from;
            statement.end += from;
            statement.terminatorEnd += from;
        }
        return part;
    }

    const int index = SqlSplitter::statementIndexAt(m_statements, cursor.position());
    if (index < 0)
        return {};
    return {m_statements[index]};
}

// ---------------------------------------------------------------------------
// 줄 번호 영역과 강조

int SqlEditor::lineNumberAreaWidth() const
{
    int digits = 2;
    for (int max = qMax(1, blockCount()); max >= 100; max /= 10)
        ++digits;
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits + kMarkerWidth;
}

void SqlEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void SqlEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void SqlEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    m_lineNumberArea->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));
}

void SqlEditor::paintLineNumberArea(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    const QPalette pal = palette();
    painter.fillRect(event->rect(), pal.color(QPalette::Base)); // 거터는 에디터와 같은 바탕

    // 현재 문장이 걸친 줄 범위 (Ctrl+Enter로 실행될 범위)
    int statementFirst = -1;
    int statementLast = -1;
    if (m_currentStatement >= 0 && m_currentStatement < m_statements.size()) {
        const SqlStatement &statement = m_statements[m_currentStatement];
        statementFirst = document()->findBlock(statement.start).blockNumber();
        statementLast = document()->findBlock(qMax(statement.start, statement.end - 1)).blockNumber();
    }

    const int currentLine = textCursor().blockNumber();
    const int width = m_lineNumberArea->width();
    const int lineHeight = fontMetrics().height();
    painter.setFont(font());

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            if (number >= statementFirst && number <= statementLast)
                painter.fillRect(width - kMarkerWidth, top, kMarkerWidth, bottom - top, pal.color(QPalette::Link));
            painter.setPen(pal.color(number == currentLine ? QPalette::Text : QPalette::PlaceholderText));
            painter.drawText(0, top, width - kMarkerWidth - 6, lineHeight, Qt::AlignRight | Qt::AlignTop,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void SqlEditor::updateExtraSelections()
{
    QList<QTextEdit::ExtraSelection> selections;
    const QPalette pal = palette();

    if (!hasExtraCursors()) {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(pal.color(QPalette::AlternateBase));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        selections.append(line);
    }

    for (const QTextCursor &mark : std::as_const(m_wordMarks)) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = mark;
        selection.format.setBackground(Theme::searchMatchColor());
        selections.append(selection);
    }

    for (int i = 0; i < m_searchMatches.size(); ++i) {
        QTextEdit::ExtraSelection match;
        match.cursor = m_searchMatches[i];
        match.format.setBackground(i == m_currentSearchMatch ? Theme::currentMatchColor() : Theme::searchMatchColor());
        selections.append(match);
    }

    appendBracketMatch(selections);

    for (const QTextCursor &cursor : m_cursors) {
        if (!cursor.hasSelection())
            continue;
        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;
        selection.format.setBackground(pal.color(QPalette::Highlight));
        selection.format.setForeground(pal.color(QPalette::HighlightedText));
        selections.append(selection);
    }

    setExtraSelections(selections);
}

void SqlEditor::setSearchMatches(const QList<QTextCursor> &matches, int current)
{
    m_searchMatches = matches;
    m_currentSearchMatch = current;
    updateExtraSelections();
}

void SqlEditor::appendBracketMatch(QList<QTextEdit::ExtraSelection> &selections) const
{
    const QTextDocument *doc = document();
    const int position = textCursor().position();

    int bracketPos = -1;
    if (QChar c = doc->characterAt(position); isOpenBracket(c) || isCloseBracket(c))
        bracketPos = position;
    else if (QChar before = doc->characterAt(position - 1); isOpenBracket(before) || isCloseBracket(before))
        bracketPos = position - 1;
    if (bracketPos < 0)
        return;

    const QChar bracket = doc->characterAt(bracketPos);
    const QChar match = matchingBracket(bracket);
    const int step = isOpenBracket(bracket) ? 1 : -1;
    const int limit = doc->characterCount();

    int depth = 0;
    int matchPos = -1;
    for (int p = bracketPos, scanned = 0; p >= 0 && p < limit && scanned < 20000; p += step, ++scanned) {
        const QChar c = doc->characterAt(p);
        if (c == bracket)
            ++depth;
        else if (c == match && --depth == 0) {
            matchPos = p;
            break;
        }
    }
    if (matchPos < 0)
        return;

    QColor color = palette().color(QPalette::Highlight);
    color.setAlpha(isDark(palette()) ? 90 : 60);
    for (int p : {bracketPos, matchPos}) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(const_cast<QTextDocument *>(doc));
        selection.cursor.setPosition(p);
        selection.cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
        selection.format.setBackground(color);
        selection.format.setFontWeight(QFont::Bold);
        selections.append(selection);
    }
}

void SqlEditor::onCursorPositionChanged()
{
    // 두 번 클릭한 단어에서 선택이 바뀌면(다른 곳 클릭, 입력, Shift+방향키) 표시를 지운다.
    if (!m_wordMarks.isEmpty() && textCursor().selectedText().compare(m_markedWord, Qt::CaseInsensitive) != 0)
        clearWordMarks();
    // 우리 코드가 아닌 곳에서 커서가 움직였으면 세로 커서를 끝낸다.
    if (!m_updatingCursors && !m_columnDragging && hasExtraCursors()) {
        m_cursors.clear();
        m_columnRect = false;
        viewport()->update();
    }
    updateCurrentStatement();
    updateExtraSelections();
    m_lineNumberArea->update();
}

// ---------------------------------------------------------------------------
// 문장 분석

void SqlEditor::reparse()
{
    m_parseTimer->stop();
    m_statements = SqlSplitter::split(toPlainText(), m_dialect);
    m_statementsDirty = false;
    m_currentStatement = -2; // 강제로 다시 계산
    updateCurrentStatement();
}

void SqlEditor::updateCurrentStatement()
{
    const int index = SqlSplitter::statementIndexAt(m_statements, textCursor().position());
    if (index != m_currentStatement) {
        m_currentStatement = index;
        m_lineNumberArea->update();
    }
}

// ---------------------------------------------------------------------------
// 키 입력

QString SqlEditor::objectNameAtCursor() const
{
    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        QString text = cursor.selectedText();
        text.replace(QChar::ParagraphSeparator, u' ');
        return text.trimmed();
    }
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    auto nameChar = [](QChar c) {
        return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#' || c == u'.' || c == u'"' || c == u'`';
    };
    int start = at;
    while (start > 0 && nameChar(line.at(start - 1)))
        --start;
    int end = at;
    while (end < line.size() && nameChar(line.at(end)))
        ++end;
    return line.mid(start, end - start);
}

void SqlEditor::keyPressEvent(QKeyEvent *event)
{
    // Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgUp/PgDn: 탭 옮겨 가기 (창이 처리하도록 넘긴다)
    if ((event->modifiers() & Qt::ControlModifier)
        && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown)) {
        event->ignore();
        return;
    }
    // 자동완성 목록이 열려 있으면 Enter/Tab/Esc는 목록이 처리한다.
    if (m_completer->keyPressBefore(event))
        return;
    if (event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier)) {
        m_completer->trigger(true);
        return;
    }
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier && !m_wordMarks.isEmpty()
        && !hasExtraCursors()) {
        clearWordMarks();
        return;
    }
    processKey(event);
    m_completer->keyPressAfter(event);
}

void SqlEditor::processKey(QKeyEvent *event)
{
    // 수식 키와 한/영·한자 키만 눌렀을 때는 세로 커서와 사각형 선택 상태를 그대로 둔다.
    switch (event->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Hangul:
    case Qt::Key_Hangul_Hanja:
        QPlainTextEdit::keyPressEvent(event);
        return;
    default:
        break;
    }

    if (handleColumnSelectionKey(event))
        return;
    if (hasExtraCursors() && handleMultiCursorKey(event))
        return;
    m_columnRect = false;

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrlOrAlt = mods & (Qt::ControlModifier | Qt::AltModifier);

    if (key == Qt::Key_Tab && !ctrlOrAlt) {
        if (selectionSpansLines()) {
            indentLines(false);
        } else {
            const int column = textCursor().positionInBlock();
            insertPlainText(QString(kIndentWidth - column % kIndentWidth, u' '));
        }
        return;
    }
    if (key == Qt::Key_Backtab && !ctrlOrAlt) { // Ctrl+Shift+Tab은 이전 탭으로 (창의 단축키)
        indentLines(true);
        return;
    }
    if (key == Qt::Key_Slash && (mods & Qt::ControlModifier)) {
        toggleLineComment();
        return;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
        // 새 줄도 윗줄과 같은 들여쓰기로 시작한다 (실행 취소 한 번에 되돌아가게 묶는다).
        QTextCursor cursor = textCursor();
        const QString indent = leadingWhitespace(cursor.block().text());
        cursor.beginEditBlock();
        if (cursor.hasSelection())
            cursor.removeSelectedText();
        cursor.insertBlock();
        cursor.insertText(indent);
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

bool SqlEditor::handleColumnSelectionKey(QKeyEvent *event)
{
    const int key = event->key();
    if (key != Qt::Key_Up && key != Qt::Key_Down && key != Qt::Key_Left && key != Qt::Key_Right)
        return false;

    const Qt::KeyboardModifiers mods =
        event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
    const bool boxSelect = mods == (Qt::AltModifier | Qt::ShiftModifier);
    const bool addCaret = mods == (Qt::ControlModifier | Qt::AltModifier) && (key == Qt::Key_Up || key == Qt::Key_Down);
    if (!boxSelect && !addCaret)
        return false;

    if (!m_columnRect) {
        // 지금 커서 위치에서 사각형 선택을 시작한다.
        const QTextCursor cursor = hasExtraCursors() ? m_cursors[m_primaryIndex] : textCursor();
        m_columnAnchorLine = cursor.blockNumber();
        m_columnAnchorX = xOf(cursor);
        m_columnHeadLine = m_columnAnchorLine;
        m_columnHeadX = m_columnAnchorX;
        m_columnRect = true;
    }

    int headLine = m_columnHeadLine;
    qreal headX = m_columnHeadX;
    const qreal space = fontMetrics().horizontalAdvance(QLatin1Char(' '));

    if (key == Qt::Key_Up) {
        headLine = qMax(0, headLine - 1);
    } else if (key == Qt::Key_Down) {
        headLine = qMin(blockCount() - 1, headLine + 1);
    } else {
        QTextCursor head(document());
        head.setPosition(positionAt(headLine, headX));
        const qreal currentX = xOf(head);
        if (key == Qt::Key_Left) {
            if (headX > currentX + 0.5)
                headX = qMax(currentX, headX - space); // 줄 끝 너머(빈 공간)에서 왼쪽으로
            else if (head.positionInBlock() > 0) {
                head.movePosition(QTextCursor::Left);
                headX = xOf(head);
            }
        } else {
            if (!head.atBlockEnd() && headX <= currentX + 0.5) {
                head.movePosition(QTextCursor::Right);
                headX = xOf(head);
            } else {
                headX += space; // 줄 끝 너머로 넓힌다 (짧은 줄은 줄 끝에 맞춰진다)
            }
        }
    }

    updateColumnSelection(headLine, headX);
    event->accept();
    return true;
}

bool SqlEditor::handleMultiCursorKey(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const QTextCursor::MoveMode mode = shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

    if (key == Qt::Key_Escape) {
        clearExtraCursors();
        return true;
    }
    if (event->matches(QKeySequence::Copy)) {
        copyFromCursors();
        return true;
    }
    if (event->matches(QKeySequence::Cut)) {
        copyFromCursors();
        editAllCursors([](QTextCursor &c) { c.removeSelectedText(); });
        return true;
    }
    if (event->matches(QKeySequence::Paste)) {
        pasteToCursors();
        return true;
    }

    if (!ctrl && !alt) {
        switch (key) {
        case Qt::Key_Backspace:
            editAllCursors([](QTextCursor &c) {
                if (c.hasSelection())
                    c.removeSelectedText();
                else if (c.positionInBlock() > 0)
                    c.deletePreviousChar();
            });
            return true;
        case Qt::Key_Delete:
            editAllCursors([](QTextCursor &c) {
                if (c.hasSelection())
                    c.removeSelectedText();
                else if (!c.atBlockEnd())
                    c.deleteChar();
            });
            return true;
        case Qt::Key_Left:
            moveAllCursors([shift, mode](QTextCursor &c) {
                if (c.hasSelection() && !shift)
                    c.setPosition(c.selectionStart());
                else if (c.positionInBlock() > 0)
                    c.movePosition(QTextCursor::Left, mode);
            });
            return true;
        case Qt::Key_Right:
            moveAllCursors([shift, mode](QTextCursor &c) {
                if (c.hasSelection() && !shift)
                    c.setPosition(c.selectionEnd());
                else if (!c.atBlockEnd())
                    c.movePosition(QTextCursor::Right, mode);
            });
            return true;
        case Qt::Key_Home:
            moveAllCursors([mode](QTextCursor &c) { c.movePosition(QTextCursor::StartOfBlock, mode); });
            return true;
        case Qt::Key_End:
            moveAllCursors([mode](QTextCursor &c) { c.movePosition(QTextCursor::EndOfBlock, mode); });
            return true;
        case Qt::Key_Tab:
            insertAtAllCursors(QString(kIndentWidth, u' '));
            return true;
        default:
            break;
        }
    }

    // 글자 입력 (Windows의 AltGr은 Ctrl+Alt로 들어온다)
    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isPrint() && (!ctrl || alt)) {
        insertAtAllCursors(text);
        return true;
    }

    // 그 밖의 키(위/아래, Enter, 실행 취소 등)는 세로 커서를 끝내고 일반 동작으로 처리한다.
    clearExtraCursors();
    return false;
}

void SqlEditor::inputMethodEvent(QInputMethodEvent *event)
{
    if (!hasExtraCursors()) {
        QPlainTextEdit::inputMethodEvent(event);
        return;
    }

    // 조합이 끝난 글자는 모든 커서에 넣는다. 조합 중인 글자는 주 커서 위치에만 보인다.
    if (!event->commitString().isEmpty())
        insertAtAllCursors(event->commitString());

    QInputMethodEvent preeditOnly(event->preeditString(), event->attributes());
    m_updatingCursors = true;
    QPlainTextEdit::inputMethodEvent(&preeditOnly);
    m_updatingCursors = false;
    event->accept();
}

// ---------------------------------------------------------------------------
// 마우스: Alt(+Shift)+드래그로 사각형 선택

void SqlEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier)) {
        const QTextCursor hit = cursorForPosition(event->position().toPoint());
        m_columnDragging = true;
        startColumnSelection(hit.blockNumber(), event->position().x() + horizontalScrollBar()->value());
        event->accept();
        return;
    }
    if (hasExtraCursors())
        clearExtraCursors();
    m_columnRect = false;
    QPlainTextEdit::mousePressEvent(event);
}

void SqlEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_columnDragging) {
        const QTextCursor hit = cursorForPosition(event->position().toPoint());
        updateColumnSelection(hit.blockNumber(), event->position().x() + horizontalScrollBar()->value());
        event->accept();
        return;
    }
    QPlainTextEdit::mouseMoveEvent(event);
}

void SqlEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_columnDragging) {
        m_columnDragging = false;
        event->accept();
        return;
    }
    QPlainTextEdit::mouseReleaseEvent(event);
}

void SqlEditor::dropEvent(QDropEvent *event)
{
    // 탐색기에서 끌어다 놓은 이름: 놓은 자리에 들어가고, 바로 이어서 입력할 수 있게 에디터로 초점을 옮긴다.
    QPlainTextEdit::dropEvent(event);
    activateWindow();
    setFocus(Qt::MouseFocusReason);
}

void SqlEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseDoubleClickEvent(event);
    if (event->button() != Qt::LeftButton || (event->modifiers() & Qt::AltModifier) || hasExtraCursors())
        return;
    markWordAt(cursorForPosition(event->position().toPoint()).position());
}

namespace {

bool isWordChar(QChar ch, SqlDialect dialect)
{
    if (ch.isLetterOrNumber() || ch == u'_' || ch == u'$')
        return true;
    return ch == u'#' && dialect == SqlDialect::Oracle; // Oracle·Tibero
}

} // namespace

void SqlEditor::markWordAt(int position)
{
    const QString text = document()->toPlainText(); // 블록 사이는 줄바꿈 한 글자라 위치가 문서 위치와 같다
    int at = position;
    if (at >= text.size() || !isWordChar(text.at(at), m_dialect))
        --at; // 단어 끝 바로 뒤를 눌렀을 때
    if (at < 0 || at >= text.size() || !isWordChar(text.at(at), m_dialect)) {
        clearWordMarks();
        return;
    }
    int start = at;
    while (start > 0 && isWordChar(text.at(start - 1), m_dialect))
        --start;
    int end = at + 1;
    while (end < text.size() && isWordChar(text.at(end), m_dialect))
        ++end;
    const QString word = text.mid(start, end - start);

    // 같은 단어를 찾는다 (대소문자 무시, 앞뒤가 단어 글자가 아닌 곳). 아주 긴 스크립트는 앞쪽 일부만.
    constexpr int kMaxMarks = 10000;
    QList<QTextCursor> marks;
    for (int from = 0; marks.size() < kMaxMarks;) {
        const int found = text.indexOf(word, from, Qt::CaseInsensitive);
        if (found < 0)
            break;
        from = found + word.size();
        if (found > 0 && isWordChar(text.at(found - 1), m_dialect))
            continue;
        if (from < text.size() && isWordChar(text.at(from), m_dialect))
            continue;
        QTextCursor mark(document());
        mark.setPosition(found);
        mark.setPosition(from, QTextCursor::KeepAnchor);
        marks.append(mark);
    }

    // 선택을 SQL 단어 전체로 맞춘다 (Qt 기본 단어 선택은 $, #에서 끊긴다).
    m_markedWord = word;
    m_wordMarks.clear(); // 선택을 바꾸는 동안 onCursorPositionChanged가 지우지 않게
    QTextCursor cursor = textCursor();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    setTextCursor(cursor);

    m_wordMarks = marks;
    updateExtraSelections();
    emit wordMarksChanged(word, int(marks.size()));
}

void SqlEditor::clearWordMarks()
{
    if (m_wordMarks.isEmpty() && m_markedWord.isEmpty())
        return;
    m_wordMarks.clear();
    m_markedWord.clear();
    updateExtraSelections();
    emit wordMarksChanged(QString(), 0);
}

void SqlEditor::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    if (!hasExtraCursors())
        return;

    // 주 커서는 QPlainTextEdit가 그리고, 나머지 커서는 여기서 그린다.
    QPainter painter(viewport());
    const QColor caret = palette().color(QPalette::Text);
    for (int k = 0; k < m_cursors.size(); ++k) {
        if (k == m_primaryIndex)
            continue;
        const QRect rect = cursorRect(m_cursors[k]);
        painter.fillRect(QRect(rect.left(), rect.top(), qMax(1, cursorWidth()), rect.height()), caret);
    }
}

void SqlEditor::wheelEvent(QWheelEvent *event)
{
    // Ctrl + 휠: 글자 크기 (모든 SQL 탭에 같이 적용되고 다음에도 유지된다)
    if (FontZoom::instance()->handleWheel(event, FontZoom::Target::Editor, font().pointSizeF()))
        return;
    QPlainTextEdit::wheelEvent(event);
}

void SqlEditor::applyFontSize(qreal pointSize)
{
    if (qFuzzyCompare(font().pointSizeF(), pointSize))
        return;
    QFont changed = font();
    changed.setPointSizeF(pointSize);
    setFont(changed);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * kIndentWidth);
    updateLineNumberAreaWidth();
    m_lineNumberArea->update();
    ensureCursorVisible();
}

void SqlEditor::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        m_highlighter->setDarkTheme(isDark(palette()));
        updateExtraSelections();
    }
}

// ---------------------------------------------------------------------------
// 세로 커서

void SqlEditor::startColumnSelection(int line, qreal x)
{
    m_columnAnchorLine = line;
    m_columnAnchorX = x;
    m_columnRect = true;
    updateColumnSelection(line, x);
}

void SqlEditor::updateColumnSelection(int headLine, qreal headX)
{
    m_columnHeadLine = headLine;
    m_columnHeadX = headX;

    const int first = qMin(m_columnAnchorLine, headLine);
    const int last = qMax(m_columnAnchorLine, headLine);

    QList<QTextCursor> cursors;
    for (int line = first; line <= last; ++line) {
        QTextCursor cursor(document());
        cursor.setPosition(positionAt(line, m_columnAnchorX));
        cursor.setPosition(positionAt(line, headX), QTextCursor::KeepAnchor);
        cursors.append(cursor);
    }

    if (cursors.size() <= 1) {
        // 한 줄이면 일반 선택으로 보여 준다.
        m_cursors.clear();
        m_updatingCursors = true;
        if (!cursors.isEmpty())
            setTextCursor(cursors.first());
        m_updatingCursors = false;
        updateExtraSelections();
        viewport()->update();
        return;
    }

    m_cursors = cursors;
    m_primaryIndex = headLine - first;
    syncPrimaryCursor();
}

void SqlEditor::clearExtraCursors()
{
    m_columnRect = false;
    if (m_cursors.isEmpty())
        return;

    const int position = m_cursors.value(m_primaryIndex).position();
    m_cursors.clear();

    m_updatingCursors = true;
    QTextCursor cursor(document());
    cursor.setPosition(position);
    setTextCursor(cursor);
    m_updatingCursors = false;

    updateExtraSelections();
    viewport()->update();
}

void SqlEditor::syncPrimaryCursor()
{
    if (m_cursors.isEmpty())
        return;
    m_primaryIndex = qBound(0, m_primaryIndex, int(m_cursors.size()) - 1);

    // 주 커서는 선택 없이 위치만 맞춘다 (선택 영역은 updateExtraSelections가 그린다).
    m_updatingCursors = true;
    QTextCursor primary(document());
    primary.setPosition(m_cursors[m_primaryIndex].position());
    setTextCursor(primary);
    m_updatingCursors = false;

    updateExtraSelections();
    viewport()->update();
}

void SqlEditor::editAllCursors(const std::function<void(QTextCursor &)> &edit)
{
    m_updatingCursors = true;
    QTextCursor group(document());
    group.beginEditBlock(); // 실행 취소 한 번에 모든 커서의 편집이 되돌아간다
    for (QTextCursor &cursor : m_cursors)
        edit(cursor);
    group.endEditBlock();
    m_updatingCursors = false;

    m_columnRect = false;
    syncPrimaryCursor();
}

void SqlEditor::moveAllCursors(const std::function<void(QTextCursor &)> &move)
{
    for (QTextCursor &cursor : m_cursors)
        move(cursor);
    m_columnRect = false;
    syncPrimaryCursor();
}

void SqlEditor::insertAtAllCursors(const QString &text)
{
    editAllCursors([&text](QTextCursor &cursor) { cursor.insertText(text); });
}

void SqlEditor::copyFromCursors() const
{
    QStringList parts;
    bool any = false;
    for (const QTextCursor &cursor : m_cursors) {
        parts << cursor.selectedText();
        any = any || cursor.hasSelection();
    }
    if (any)
        QGuiApplication::clipboard()->setText(parts.join(u'\n'));
}

void SqlEditor::pasteToCursors()
{
    QString text = QGuiApplication::clipboard()->text();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList lines = text.split(u'\n');
    if (lines.size() > 1 && lines.last().isEmpty())
        lines.removeLast();

    // 줄 수가 커서 수와 같으면 한 줄씩 나눠 넣는다 (블록 복사 → 블록 붙여넣기).
    if (lines.size() == m_cursors.size()) {
        int k = 0;
        editAllCursors([&lines, &k](QTextCursor &cursor) { cursor.insertText(lines[k++]); });
    } else {
        insertAtAllCursors(text);
    }
}

int SqlEditor::positionAt(int line, qreal x) const
{
    const QTextBlock block = document()->findBlockByNumber(line);
    if (!block.isValid())
        return qMax(0, document()->characterCount() - 1);

    const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
    const QPoint point(qRound(x - horizontalScrollBar()->value()), qRound(geometry.center().y()));
    QTextCursor cursor = cursorForPosition(point);
    if (cursor.block() != block) {
        cursor = QTextCursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock);
    }
    return cursor.position();
}

qreal SqlEditor::xOf(const QTextCursor &cursor) const
{
    return cursorRect(cursor).left() + horizontalScrollBar()->value();
}

// ---------------------------------------------------------------------------
// 편집 도우미

bool SqlEditor::selectionSpansLines() const
{
    const QTextCursor cursor = textCursor();
    return cursor.hasSelection()
           && document()->findBlock(cursor.selectionStart()) != document()->findBlock(cursor.selectionEnd());
}

void SqlEditor::indentLines(bool outdent)
{
    const QTextCursor cursor = textCursor();
    const QTextBlock first = document()->findBlock(cursor.selectionStart());
    QTextBlock last = document()->findBlock(cursor.selectionEnd());
    if (cursor.hasSelection() && last != first && last.position() == cursor.selectionEnd())
        last = last.previous(); // 다음 줄 맨 앞까지 선택한 경우 그 줄은 제외

    QTextCursor edit(document());
    edit.beginEditBlock();
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        QTextCursor line(block);
        if (!outdent) {
            line.insertText(QString(kIndentWidth, u' '));
        } else {
            const QString text = block.text();
            int remove = 0;
            if (text.startsWith(u'\t'))
                remove = 1;
            else
                while (remove < kIndentWidth && remove < text.size() && text[remove] == u' ')
                    ++remove;
            if (remove > 0) {
                line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
                line.removeSelectedText();
            }
        }
        if (block == last)
            break;
    }
    edit.endEditBlock();

    if (cursor.hasSelection()) {
        QTextCursor selection(document());
        selection.setPosition(first.position());
        selection.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(selection);
    }
}

void SqlEditor::toggleLineComment()
{
    const QTextCursor cursor = textCursor();
    const QTextBlock first = document()->findBlock(cursor.selectionStart());
    QTextBlock last = document()->findBlock(cursor.selectionEnd());
    if (cursor.hasSelection() && last != first && last.position() == cursor.selectionEnd())
        last = last.previous();

    // 내용이 있는 줄이 모두 주석이면 주석을 풀고, 아니면 모두 주석으로 만든다.
    bool anyCode = false;
    bool allCommented = true;
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        const QString trimmed = block.text().trimmed();
        if (!trimmed.isEmpty()) {
            anyCode = true;
            allCommented = allCommented && trimmed.startsWith(QStringLiteral("--"));
        }
        if (block == last)
            break;
    }
    if (!anyCode)
        return;

    QTextCursor edit(document());
    edit.beginEditBlock();
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        const QString text = block.text();
        QTextCursor line(block);
        if (allCommented) {
            const int index = text.indexOf(QStringLiteral("--"));
            if (index >= 0 && text.left(index).trimmed().isEmpty()) {
                const int length = (index + 2 < text.size() && text[index + 2] == u' ') ? 3 : 2;
                line.setPosition(block.position() + index);
                line.setPosition(block.position() + index + length, QTextCursor::KeepAnchor);
                line.removeSelectedText();
            }
        } else if (!text.trimmed().isEmpty()) {
            line.insertText(QStringLiteral("-- "));
        }
        if (block == last)
            break;
    }
    edit.endEditBlock();
}
