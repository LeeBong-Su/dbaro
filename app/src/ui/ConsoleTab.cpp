#include "ConsoleTab.h"

#include "FontZoom.h"
#include "ScrollHelpers.h"
#include "ScriptFoldersDialog.h"
#include "cli/CliApp.h"
#include "core/ScriptFolders.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

// 한글이 영문 두 칸 너비인 코딩용 고정폭 글꼴 (있으면 화면 전체에 쓴다)
QString codingFontWithHangul()
{
    const QStringList families = QFontDatabase::families();
    for (const QString &name : {QStringLiteral("D2Coding"), QStringLiteral("NanumGothicCoding"), QStringLiteral("Sarasa Mono K")}) {
        if (families.contains(name))
            return name;
    }
    return {};
}

// CLI 표의 줄 (+----+, | 값 |)
bool isTableLine(const QString &line)
{
    return line.startsWith(u'|') || (line.startsWith(u'+') && line.contains(QStringLiteral("--")));
}

} // namespace

// ===========================================================================
// ConsoleView

ConsoleView::ConsoleView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setUndoRedoEnabled(false); // 출력까지 되돌리지 않게
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &ConsoleView::updateCaret);
    updateCaret();
}

void ConsoleView::updateCaret()
{
    // 깜박이는 커서는 입력 자리에만 (지난 출력을 고르는 동안에는 숨긴다)
    const bool atInput = m_promptShown && textCursor().position() >= m_inputStart;
    setCursorWidth(atInput ? 1 : 0);
}

QString ConsoleView::input() const
{
    if (!m_promptShown)
        return {};
    QTextCursor cursor(document());
    cursor.setPosition(m_inputStart);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    QString text = cursor.selectedText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    return text;
}

void ConsoleView::setInput(const QString &text)
{
    if (!m_promptShown)
        return;
    QTextCursor cursor(document());
    cursor.setPosition(m_inputStart);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(text, m_inputFormat);
    setTextCursor(cursor);
    setCurrentCharFormat(m_inputFormat);
    ensureCursorVisible();
}

void ConsoleView::appendOutput(const QString &text, const QTextCharFormat &format)
{
    if (text.isEmpty())
        return;
    QScrollBar *bar = verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    QTextCursor cursor(document());
    QString inserted = text;
    if (m_promptShown) {
        // 프롬프트 줄 위에 끼워 넣는다 (쓰던 입력은 그대로)
        cursor.setPosition(m_promptStart);
        if (!inserted.endsWith(u'\n'))
            inserted += u'\n';
    } else {
        cursor.movePosition(QTextCursor::End);
    }
    const int before = document()->characterCount();
    cursor.insertText(inserted, format);
    if (m_promptShown) {
        const int added = document()->characterCount() - before;
        m_promptStart += added;
        m_inputStart += added;
    }
    trim();
    if (atBottom)
        bar->setValue(bar->maximum());
}

void ConsoleView::showPrompt(const QString &prompt, const QTextCharFormat &format)
{
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (!cursor.block().text().isEmpty())
        cursor.insertText(QStringLiteral("\n"), QTextCharFormat());
    m_promptStart = cursor.position();
    cursor.insertText(prompt, format);
    m_inputStart = cursor.position();
    m_prompt = prompt;
    m_promptShown = true;
    setTextCursor(cursor);
    setCurrentCharFormat(m_inputFormat);
    updateCaret();
    ensureCursorVisible();
}

void ConsoleView::commitInput(const QString &suffix, const QTextCharFormat &suffixFormat)
{
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (!suffix.isEmpty())
        cursor.insertText(suffix, suffixFormat);
    cursor.insertText(QStringLiteral("\n"), QTextCharFormat());
    m_promptShown = false;
    setTextCursor(cursor);
    updateCaret();
    ensureCursorVisible();
}

void ConsoleView::clearScreen()
{
    const bool prompt = m_promptShown;
    const QString typed = input();
    const QString promptText = m_prompt;
    QTextCharFormat promptFormat;
    if (prompt) {
        QTextCursor cursor(document());
        cursor.setPosition(m_promptStart + 1);
        promptFormat = cursor.charFormat();
    }
    clear();
    m_promptShown = false;
    if (prompt) {
        showPrompt(promptText, promptFormat);
        setInput(typed);
    }
}

void ConsoleView::trim()
{
    const int extra = document()->blockCount() - kMaxBlocks;
    if (extra <= 0)
        return;
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, extra);
    const int removed = cursor.selectionEnd() - cursor.selectionStart();
    cursor.removeSelectedText();
    m_promptStart = qMax(0, m_promptStart - removed);
    m_inputStart = qMax(0, m_inputStart - removed);
}

bool ConsoleView::selectionInInput() const
{
    return m_promptShown && textCursor().selectionStart() >= m_inputStart;
}

void ConsoleView::moveToInputEnd(bool keepScroll)
{
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    setCurrentCharFormat(m_inputFormat);
    if (keepScroll) {
        // 지난 출력을 눌렀을 때: 커서만 입력 자리로, 보던 곳은 그대로
        verticalScrollBar()->setValue(vertical);
        horizontalScrollBar()->setValue(horizontal);
    }
}

// ---------------------------------------------------------------------------
// 사각형(세로) 선택

void ConsoleView::selectBlock(const QPoint &from, const QPoint &to)
{
    const int scroll = horizontalScrollBar()->value();
    m_blockAnchorLine = cursorForPosition(from).blockNumber();
    m_blockAnchorX = from.x() + scroll;
    m_blockHeadLine = cursorForPosition(to).blockNumber();
    m_blockHeadX = to.x() + scroll;
    updateBlockSelection();
}

void ConsoleView::updateBlockSelection()
{
    // 줄마다 화면의 같은 가로 범위에 들어오는 글자 (글자 가운데가 범위 안이면 포함)
    m_blockCursors.clear();
    const int first = qMin(m_blockAnchorLine, m_blockHeadLine);
    const int last = qMax(m_blockAnchorLine, m_blockHeadLine);
    const int scroll = horizontalScrollBar()->value();
    const int left = int(qMin(m_blockAnchorX, m_blockHeadX)) - scroll;
    const int right = int(qMax(m_blockAnchorX, m_blockHeadX)) - scroll;
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

QString ConsoleView::blockSelectionText() const
{
    QStringList lines;
    for (const QTextCursor &line : m_blockCursors)
        lines << line.selectedText();
    return lines.join(u'\n');
}

void ConsoleView::clearBlockSelection()
{
    if (m_blockCursors.isEmpty() && !m_blockDragging)
        return;
    m_blockCursors.clear();
    m_blockDragging = false;
    setExtraSelections({});
}

void ConsoleView::copySelection()
{
    if (hasBlockSelection())
        QGuiApplication::clipboard()->setText(blockSelectionText());
    else if (textCursor().hasSelection())
        copy();
}

void ConsoleView::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    // Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgUp/PgDn: 탭 옮겨 가기 (창이 처리하도록 넘긴다)
    if ((mods & Qt::ControlModifier)
        && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown)) {
        event->ignore();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        // 고른 글자(사각형 포함)가 있으면 복사, 없으면 터미널처럼 끊기
        if (hasBlockSelection() || textCursor().hasSelection())
            copySelection();
        else
            emit interruptRequested();
        return;
    }
    // 수식 키만 누른 것이 아니면 사각형 선택은 푼다
    if (event->key() != Qt::Key_Control && event->key() != Qt::Key_Shift && event->key() != Qt::Key_Alt
        && event->key() != Qt::Key_Meta)
        clearBlockSelection();
    if (event->key() == Qt::Key_L && mods == Qt::ControlModifier) {
        clearScreen();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    // 화면 이동·고르기만 하는 키
    static const QList<QKeySequence::StandardKey> navigation{
        QKeySequence::MoveToPreviousPage, QKeySequence::MoveToNextPage,       QKeySequence::SelectPreviousPage,
        QKeySequence::SelectNextPage,     QKeySequence::MoveToStartOfDocument, QKeySequence::MoveToEndOfDocument,
        QKeySequence::SelectStartOfDocument, QKeySequence::SelectEndOfDocument};
    for (const QKeySequence::StandardKey key : navigation) {
        if (event->matches(key)) {
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
    }
    if (!m_promptShown) {
        event->accept(); // 실행 중에는 입력을 받지 않는다 (Ctrl+C로 취소)
        return;
    }

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        moveToInputEnd();
        emit submitted();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods == Qt::NoModifier) {
            emit historyRequested(event->key() == Qt::Key_Up ? -1 : 1);
            return;
        }
        break;
    case Qt::Key_Escape:
        setInput({});
        return;
    case Qt::Key_Home:
        if (!(mods & Qt::ControlModifier)) {
            if (!selectionInInput())
                moveToInputEnd();
            QTextCursor cursor = textCursor();
            cursor.setPosition(m_inputStart, (mods & Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
            setTextCursor(cursor);
            return;
        }
        break;
    case Qt::Key_Backspace: {
        if (!selectionInInput())
            moveToInputEnd();
        QTextCursor cursor = textCursor();
        if (cursor.hasSelection()) {
            cursor.removeSelectedText();
            setCurrentCharFormat(m_inputFormat);
            return;
        }
        if (cursor.position() <= m_inputStart)
            return; // 프롬프트는 지우지 않는다
        // 한 글자(Ctrl이면 한 단어) 지우되 프롬프트 앞으로 넘어가지 않게
        const int end = cursor.position();
        cursor.movePosition((mods & Qt::ControlModifier) ? QTextCursor::PreviousWord : QTextCursor::PreviousCharacter);
        cursor.setPosition(qMax(m_inputStart, cursor.position()));
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        setCurrentCharFormat(m_inputFormat);
        return;
    }
    case Qt::Key_Left: {
        if (!selectionInInput())
            moveToInputEnd();
        QPlainTextEdit::keyPressEvent(event);
        QTextCursor cursor = textCursor();
        if (!(mods & Qt::ShiftModifier) && cursor.position() < m_inputStart) {
            cursor.setPosition(m_inputStart);
            setTextCursor(cursor);
        }
        return;
    }
    default:
        break;
    }

    // 고치는 키(글자, Delete, 붙여 넣기, 잘라내기)는 입력 자리에서만
    const bool editing = !event->text().isEmpty() || event->key() == Qt::Key_Delete || event->matches(QKeySequence::Paste)
                         || event->matches(QKeySequence::Cut);
    if (editing && !selectionInInput()) {
        if (event->matches(QKeySequence::Cut)) {
            copy(); // 지난 출력은 잘라 내지 않고 복사만
            return;
        }
        moveToInputEnd();
    }
    // 입력한 글자는 늘 입력 글자 모양으로 (프롬프트 바로 뒤에서 쓰면 프롬프트의 파란색을 물려받는다)
    if (editing && selectionInInput() && !textCursor().hasSelection())
        setCurrentCharFormat(m_inputFormat);
    QPlainTextEdit::keyPressEvent(event);
}

void ConsoleView::inputMethodEvent(QInputMethodEvent *event)
{
    // 한글처럼 조합해서 입력하는 글자도 입력 자리에만
    if (!m_promptShown) {
        event->accept();
        return;
    }
    if (!selectionInInput())
        moveToInputEnd();
    if (!textCursor().hasSelection())
        setCurrentCharFormat(m_inputFormat);
    QPlainTextEdit::inputMethodEvent(event);
}

bool ConsoleView::canInsertFromMimeData(const QMimeData *source) const
{
    return m_promptShown && source->hasText();
}

void ConsoleView::insertFromMimeData(const QMimeData *source)
{
    if (!m_promptShown || !source->hasText())
        return;
    if (!selectionInInput())
        moveToInputEnd();
    QString text = source->text();
    text.remove(u'\r');
    if (!text.contains(u'\n')) {
        textCursor().insertText(text, m_inputFormat);
        return;
    }
    // 여러 줄: 지금 쓰던 입력과 합쳐 줄마다 실행하고, 마지막 줄은 입력 자리에 남긴다
    const QTextCursor cursor = textCursor();
    const QString current = input();
    const int from = cursor.selectionStart() - m_inputStart;
    const int to = cursor.selectionEnd() - m_inputStart;
    QStringList lines = (current.left(from) + text + current.mid(to)).split(u'\n');
    const QString rest = lines.takeLast();
    setInput({});
    emit linesPasted(lines, rest);
}

void ConsoleView::dropEvent(QDropEvent *event)
{
    // 탐색기 이름이나 다른 글을 끌어다 놓으면 입력 자리에 (지난 출력은 그대로)
    if (!m_promptShown || !event->mimeData()->hasText()) {
        event->ignore();
        return;
    }
    moveToInputEnd();
    insertFromMimeData(event->mimeData());
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setFocus(Qt::MouseFocusReason);
}

void ConsoleView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        // Ctrl(또는 Alt)+드래그: 사각형 선택. 보통 선택과 커서는 그대로 둔다
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
    // 고른 글을 끌어서 옮기면 지난 출력이 지워지므로, 누르면 고른 것을 먼저 푼다 (복사는 Ctrl+C·오른쪽 클릭)
    if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::ShiftModifier) && textCursor().hasSelection()) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        setTextCursor(cursor);
    }
    QPlainTextEdit::mousePressEvent(event);
}

void ConsoleView::mouseMoveEvent(QMouseEvent *event)
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
    m_blockHeadLine = cursorForPosition(clamped).blockNumber();
    m_blockHeadX = clamped.x() + horizontalScrollBar()->value();
    updateBlockSelection();
    event->accept();
}

void ConsoleView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_blockDragging) {
        m_blockDragging = false;
        event->accept();
        return;
    }
    QPlainTextEdit::mouseReleaseEvent(event);
    // 그냥 누른 것이면(고른 글 없음) 커서는 입력 자리로 돌아간다 (입력 글 안을 누르면 거기에 둔다)
    if (event->button() == Qt::LeftButton && !textCursor().hasSelection()
        && (!m_promptShown || textCursor().position() < m_inputStart))
        moveToInputEnd(true);
}

void ConsoleView::wheelEvent(QWheelEvent *event)
{
    // Ctrl + 휠: 글자 크기 (모든 CLI 탭에 같이 적용되고 다음에도 유지된다)
    if (FontZoom::instance()->handleWheel(event, FontZoom::Target::Console, font().pointSizeF()))
        return;
    QPlainTextEdit::wheelEvent(event);
}

void ConsoleView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, &ConsoleView::copySelection)
        ->setEnabled(hasBlockSelection() || textCursor().hasSelection());
    QAction *paste = menu.addAction(tr("Paste"), QKeySequence::Paste, this, [this] {
        if (const QMimeData *data = QGuiApplication::clipboard()->mimeData())
            insertFromMimeData(data);
    });
    paste->setEnabled(m_promptShown && !QGuiApplication::clipboard()->text().isEmpty());
    menu.addAction(tr("Select All"), QKeySequence::SelectAll, this, &QPlainTextEdit::selectAll);
    menu.addSeparator();
    menu.addAction(tr("Clear"), QKeySequence(Qt::CTRL | Qt::Key_L), this, &ConsoleView::clearScreen);
    menu.exec(event->globalPos());
}

// ===========================================================================
// ConsoleTab

ConsoleTab::ConsoleTab(EngineClient *engine, const ConnectionProfile &profile, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_profile(profile)
{
    setObjectName(QStringLiteral("consoleTab"));

    // 위: 제목과 출력 형식, 멈추기, 지우기
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("consoleHeader"));
    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("consoleTitle"));
    m_format = new QComboBox(header);
    m_format->setObjectName(QStringLiteral("consoleFormat"));
    for (const CliApp::Format format : {CliApp::Format::Table, CliApp::Format::Csv, CliApp::Format::Tsv, CliApp::Format::Json})
        m_format->addItem(CliApp::formatName(format), int(format));
    m_format->setToolTip(tr("Result format (same as \\format)"));
    m_format->setFocusPolicy(Qt::NoFocus);
    m_stopButton = new QToolButton(header);
    m_stopButton->setText(tr("Stop"));
    m_stopButton->setToolTip(tr("Cancel the running statement (Ctrl+C)"));
    m_stopButton->setAutoRaise(true);
    m_clearButton = new QToolButton(header);
    m_clearButton->setText(tr("Clear"));
    m_clearButton->setToolTip(tr("Clear the screen (Ctrl+L)"));
    m_clearButton->setAutoRaise(true);
    m_reconnectButton = new QToolButton(header);
    m_reconnectButton->setText(tr("Reconnect"));
    m_reconnectButton->setAutoRaise(true);
    m_reconnectButton->hide();
    // 스크립트 폴더와 그 안의 파일 (고르면 @파일명으로 실행)
    m_scriptsButton = new QToolButton(header);
    m_scriptsButton->setObjectName(QStringLiteral("consoleScripts"));
    m_scriptsButton->setText(tr("Scripts"));
    m_scriptsButton->setToolTip(tr("%1 script folder: run a .sql file with @name").arg(Db::typeName(profile.type)));
    m_scriptsButton->setAutoRaise(true);
    m_scriptsButton->setPopupMode(QToolButton::InstantPopup);
    auto *scriptsMenu = new QMenu(m_scriptsButton);
    connect(scriptsMenu, &QMenu::aboutToShow, this, [this, scriptsMenu] { fillScriptsMenu(scriptsMenu); });
    m_scriptsButton->setMenu(scriptsMenu);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 4, 8, 4);
    headerLayout->setSpacing(6);
    headerLayout->addWidget(m_title, 1);
    headerLayout->addWidget(new QLabel(tr("Format"), header));
    headerLayout->addWidget(m_format);
    headerLayout->addWidget(m_stopButton);
    headerLayout->addWidget(m_clearButton);
    headerLayout->addWidget(m_scriptsButton);
    headerLayout->addWidget(m_reconnectButton);

    // 터미널 화면: 출력과 입력이 한곳 (맨 아래 프롬프트 뒤에 쓴다)
    m_view = new ConsoleView(this);
    m_view->setObjectName(QStringLiteral("consoleOutput"));
    // SQL 에디터와 같은 코딩용 글꼴 (Windows 기본 고정폭 글꼴은 Courier New라 쓰지 않는다)
    const QString coding = codingFontWithHangul();
    QFont font;
    font.setFamilies(coding.isEmpty() ? QStringList{QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                                                    QStringLiteral("Menlo"), QStringLiteral("DejaVu Sans Mono")}
                                      : QStringList{coding});
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSizeF(FontZoom::instance()->pointSize(FontZoom::Target::Console, 10.5));
    m_view->setFont(font);
    connect(FontZoom::instance(), &FontZoom::changed, this, [this](FontZoom::Target target, qreal pointSize) {
        if (target != FontZoom::Target::Console)
            return;
        QFont changed = m_view->font();
        changed.setPointSizeF(pointSize);
        m_view->setFont(changed);
        m_view->ensureCursorVisible();
    });
    m_view->setInputFormat(format(Kind::Normal, false));
    enableShiftWheelHorizontalScroll(m_view);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_view, 1);
    setFocusProxy(m_view);

    // CLI: 출력은 이 화면으로, 실행 기록·확인 창은 창이 정해 준다
    m_cli = new CliApp(this);
    m_cli->setOutput([this](const QString &text) { append(text, Kind::Normal); },
                     [this](const QString &text) { append(text, Kind::Error); });
    // 정하지 않은 치환 변수(&이름)는 SQL*Plus처럼 값을 묻는다
    // ACCEPT ... PROMPT, 정하지 않은 치환 변수(&이름): SQL*Plus처럼 그 글로 값을 묻고 화면에도 남긴다
    m_cli->setAskValue([this](const QString &prompt, bool hide, QString *value) {
        bool ok = false;
        *value = QInputDialog::getText(this, tr("Input"), prompt.trimmed(), hide ? QLineEdit::Password : QLineEdit::Normal,
                                       QString(), &ok);
        if (ok)
            append(prompt + (hide ? QString() : *value) + u'\n', Kind::Note);
        return ok;
    });
    connect(m_cli, &CliApp::stateChanged, this, [this] {
        updateHeader();
        emit stateChanged();
    });
    connect(m_cli, &CliApp::busyChanged, this, [this] {
        updateHeader();
        emit stateChanged();
    });
    connect(m_cli, &CliApp::quitRequested, this, &ConsoleTab::closeRequested, Qt::QueuedConnection);

    connect(m_view, &ConsoleView::submitted, this, [this] { runInput(m_view->input()); });
    connect(m_view, &ConsoleView::interruptRequested, this, &ConsoleTab::interrupt);
    connect(m_view, &ConsoleView::historyRequested, this, &ConsoleTab::showHistory);
    connect(m_view, &ConsoleView::linesPasted, this, [this](const QStringList &lines, const QString &rest) {
        m_queue << lines;
        m_tail = rest;
        m_hasTail = true;
        processQueue();
    });
    connect(m_format, &QComboBox::activated, this, [this] {
        m_cli->setFormat(CliApp::Format(m_format->currentData().toInt()));
        m_view->setFocus();
    });
    connect(m_stopButton, &QToolButton::clicked, this, &ConsoleTab::interrupt);
    connect(m_clearButton, &QToolButton::clicked, m_view, &ConsoleView::clearScreen);
    connect(m_reconnectButton, &QToolButton::clicked, this, [this] { start(m_spec); });
    updateHeader();
}

ConsoleTab::~ConsoleTab()
{
    // CliApp이 세션을 닫는다 (커밋하지 않은 변경은 엔진이 롤백한다)
    delete m_cli;
    m_cli = nullptr;
}

QStringList ConsoleTab::tableFontFamilies()
{
    // 코딩용 한글 글꼴이 없으면 표만 돋움체로: 한글이 정확히 두 칸이라 세로줄이 맞는다
    // (돋움체는 \ 를 ₩로 그리므로 표가 아닌 줄과 입력은 영문 고정폭 글꼴)
    if (!codingFontWithHangul().isEmpty())
        return {};
    const QStringList families = QFontDatabase::families();
    for (const QString &name : {QStringLiteral("DotumChe"), QStringLiteral("GulimChe")}) {
        if (families.contains(name))
            return {name};
    }
    return {};
}

QTextCharFormat ConsoleTab::format(Kind kind, bool table) const
{
    QTextCharFormat format;
    const bool dark = isDark(palette());
    switch (kind) {
    case Kind::Error:
        format.setForeground(dark ? QColor(0xe0, 0x6c, 0x6c) : QColor(0xc0, 0x30, 0x30));
        break;
    case Kind::Note:
        format.setForeground(palette().color(QPalette::PlaceholderText));
        break;
    case Kind::Normal:
        break;
    }
    if (table) {
        static const QStringList families = tableFontFamilies();
        if (!families.isEmpty())
            format.setFontFamilies(families);
    }
    return format;
}

void ConsoleTab::start(const ConnectSpec &spec)
{
    m_spec = spec;
    m_connecting = true;
    m_reconnectButton->hide();
    if (m_view->promptShown())
        m_view->commitInput();
    m_cli->disconnectSession();
    updateHeader();
    QPointer<ConsoleTab> self(this);
    m_cli->startEmbedded(m_engine, m_profile, spec, [self](bool ok) {
        if (!self)
            return;
        self->m_connecting = false;
        self->m_reconnectButton->setVisible(!ok);
        if (ok)
            self->showPrompt();
        self->updateHeader();
        emit self->stateChanged();
        self->processQueue();
    });
}

void ConsoleTab::submit(const QString &text)
{
    QString normalized = text;
    normalized.remove(u'\r');
    m_queue << normalized.split(u'\n');
    processQueue();
}

QString ConsoleTab::promptText() const
{
    return m_view->promptText();
}

QString ConsoleTab::inputText() const
{
    return m_view->input();
}

void ConsoleTab::setInputText(const QString &text)
{
    m_view->setInput(text);
}

bool ConsoleTab::isBusy() const
{
    return m_connecting || m_cli->isBusy();
}

bool ConsoleTab::hasPendingTransaction() const
{
    return m_cli->hasPendingTransaction();
}

bool ConsoleTab::isConnected() const
{
    return m_cli->isConnected();
}

void ConsoleTab::focusInput()
{
    m_view->setFocus();
}

void ConsoleTab::showPrompt()
{
    if (!m_cli->isConnected() || m_view->promptShown())
        return;
    QTextCharFormat promptFormat;
    promptFormat.setForeground(palette().color(QPalette::Link));
    m_view->showPrompt(m_cli->prompt(), promptFormat);
}

void ConsoleTab::runInput(const QString &line)
{
    if (isBusy() || !m_view->promptShown())
        return;
    if (!line.trimmed().isEmpty() && (m_history.isEmpty() || m_history.last() != line))
        m_history << line;
    m_historyIndex = -1;
    m_view->commitInput(); // 쓴 줄은 화면에 그대로 남는다
    QPointer<ConsoleTab> self(this);
    m_cli->submitLine(line, [self] {
        if (self)
            self->afterLine();
    });
}

void ConsoleTab::afterLine()
{
    // \format으로 바꿨으면 목록도 맞춘다
    m_format->setCurrentIndex(m_format->findData(int(m_cli->format())));
    showPrompt();
    updateHeader();
    processQueue();
}

void ConsoleTab::processQueue()
{
    // 한 줄씩: 앞 줄이 끝나야(실행 결과가 나와야) 다음 줄을 넘긴다
    if (isBusy() || !m_view->promptShown())
        return;
    if (m_queue.isEmpty()) {
        if (m_hasTail) {
            m_view->setInput(m_tail); // 붙여 넣은 마지막 줄 (Enter 전)
            m_tail.clear();
            m_hasTail = false;
        }
        return;
    }
    const QString line = m_queue.takeFirst();
    m_view->setInput(line);
    runInput(line);
}

void ConsoleTab::updateHeader()
{
    const bool busy = isBusy();
    m_stopButton->setEnabled(busy && !m_connecting);
    QString title = tr("CLI — %1").arg(m_profile.name);
    if (m_connecting)
        title += tr("  ·  connecting...");
    else if (!m_cli->isConnected())
        title += tr("  ·  not connected");
    else {
        title += tr("  ·  auto-commit %1").arg(m_cli->autoCommit() ? tr("on") : tr("off"));
        if (busy)
            title += tr("  ·  running...");
        if (m_cli->hasPendingTransaction())
            title += tr("  ·  uncommitted changes");
    }
    m_title->setText(title);
}

void ConsoleTab::append(const QString &text, Kind kind)
{
    // 표의 줄과 나머지 줄을 나눠 글꼴을 정한다 (같은 종류가 이어지는 줄은 한 번에)
    QString chunk;
    bool chunkIsTable = false;
    qsizetype position = 0;
    while (position < text.size()) {
        const qsizetype newline = text.indexOf(u'\n', position);
        const qsizetype end = newline < 0 ? text.size() : newline + 1;
        const QString line = text.mid(position, end - position);
        const bool table = isTableLine(line);
        if (!chunk.isEmpty() && table != chunkIsTable) {
            m_view->appendOutput(chunk, format(kind, chunkIsTable));
            chunk.clear();
        }
        chunk += line;
        chunkIsTable = table;
        position = end;
    }
    if (!chunk.isEmpty())
        m_view->appendOutput(chunk, format(kind, chunkIsTable));
}

void ConsoleTab::interrupt()
{
    if (m_cli->isBusy()) {
        m_queue.clear();
        m_hasTail = false;
        m_cli->cancelRunning();
        return;
    }
    if (!m_view->promptShown())
        return;
    // 쓰던 문장 버리기: 그 줄 끝에 ^C를 남기고 새 프롬프트
    m_queue.clear();
    m_hasTail = false;
    m_view->commitInput(QStringLiteral("^C"), format(Kind::Note, false));
    m_cli->discardInput();
    showPrompt();
}

void ConsoleTab::fillScriptsMenu(QMenu *menu)
{
    menu->clear();
    const QString folder = ScriptFolders::folder(m_profile.type);
    const QString typeName = Db::typeName(m_profile.type);
    QAction *where = menu->addAction(folder.isEmpty() ? tr("No %1 script folder yet").arg(typeName)
                                                      : tr("%1: %2").arg(typeName, QDir::toNativeSeparators(folder)));
    where->setEnabled(false);
    menu->addAction(tr("Choose Folder..."), this, [this] {
        const QString current = ScriptFolders::folder(m_profile.type);
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("%1 Script Folder").arg(Db::typeName(m_profile.type)),
                                                                 current.isEmpty() ? QDir::homePath() : current);
        if (chosen.isEmpty())
            return;
        ScriptFolders::setFolder(m_profile.type, chosen);
        append(tr("%1 script folder: %2\n").arg(Db::typeName(m_profile.type), QDir::toNativeSeparators(chosen)), Kind::Note);
    });
    menu->addAction(tr("Open Folder"), this, [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); })
        ->setEnabled(!folder.isEmpty());
    menu->addAction(tr("Script Folders for All DBMS..."), this, [this] {
        ScriptFoldersDialog dialog(this, m_profile.type);
        dialog.exec();
    });
    const QStringList files = ScriptFolders::list(folder, 200);
    if (files.isEmpty())
        return;
    menu->addSeparator();
    QAction *title = menu->addAction(tr("Run (@name)"));
    title->setEnabled(false);
    for (const QString &file : files) {
        QString name = file;
        if (name.endsWith(QStringLiteral(".sql"), Qt::CaseInsensitive))
            name.chop(4);
        if (name.contains(u' '))
            name = u'"' + name + u'"';
        menu->addAction(QStringLiteral("@%1").arg(name), this, [this, name] {
            submit(QStringLiteral("@%1").arg(name));
            focusInput();
        })->setEnabled(m_cli->isConnected());
    }
}

void ConsoleTab::showHistory(int step)
{
    if (m_history.isEmpty())
        return;
    if (m_historyIndex < 0) {
        if (step > 0)
            return;
        m_draft = m_view->input();
        m_historyIndex = int(m_history.size());
    }
    m_historyIndex += step;
    if (m_historyIndex < 0)
        m_historyIndex = 0;
    if (m_historyIndex >= m_history.size()) {
        m_historyIndex = -1;
        m_view->setInput(m_draft);
        return;
    }
    m_view->setInput(m_history[m_historyIndex]);
}
