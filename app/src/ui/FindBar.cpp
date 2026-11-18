#include "FindBar.h"

#include "Icons.h"
#include "SqlEditor.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

namespace {
constexpr int kMaxMatches = 10000;

QToolButton *toggleButton(const QString &text, const QString &toolTip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QToolButton *actionButton(const QString &text, const QString &toolTip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}
} // namespace

FindBar::FindBar(SqlEditor *editor, QWidget *parent)
    : QWidget(parent)
    , m_editor(editor)
{
    setObjectName(QStringLiteral("findBar"));
    setAttribute(Qt::WA_StyledBackground);

    m_find = new QLineEdit(this);
    m_find->setObjectName(QStringLiteral("findField"));
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_replace = new QLineEdit(this);
    m_replace->setObjectName(QStringLiteral("replaceField"));
    m_replace->setPlaceholderText(tr("Replace"));
    m_replace->setClearButtonEnabled(true);

    m_matchCase = toggleButton(QStringLiteral("Aa"), tr("Match case"), this);
    m_wholeWord = toggleButton(QStringLiteral("W"), tr("Words"), this);
    m_regex = toggleButton(QStringLiteral(".*"), tr("Regular expression ($1, $2 ... in the replacement)"), this);
    m_inSelection = toggleButton(tr("In selection"), tr("Search only in the text that was selected"), this);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("findStatus"));
    m_status->setMinimumWidth(m_status->fontMetrics().horizontalAdvance(QStringLiteral("10,000+ results  ")));

    QToolButton *previous = actionButton(QStringLiteral("↑"), tr("Previous (Shift+Enter, Shift+F3)"), this);
    QToolButton *next = actionButton(QStringLiteral("↓"), tr("Next (Enter, F3)"), this);
    auto *close = new QToolButton(this);
    close->setIcon(Icons::get(QStringLiteral("close")));
    close->setIconSize(QSize(12, 12));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close (Esc)"));

    QToolButton *replaceOne = actionButton(tr("Replace"), tr("Replace this one and find the next (Enter)"), this);
    QToolButton *replaceEvery = actionButton(tr("Replace All"), tr("Replace every match (undo with Ctrl+Z)"), this);

    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(2);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_matchCase);
    findRow->addWidget(m_wholeWord);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_inSelection);
    findRow->addSpacing(6);
    findRow->addWidget(m_status);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addStretch();
    findRow->addWidget(close);

    m_replaceRow = new QWidget(this);
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(2);
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(replaceOne);
    replaceRow->addWidget(replaceEvery);
    replaceRow->addStretch();

    // 두 줄의 입력란 폭을 맞춘다 (찾기 줄의 버튼 영역만큼 바꾸기 줄도 비운다).
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(8, 4, 6, 4);
    layout->setHorizontalSpacing(4);
    layout->setVerticalSpacing(3);
    layout->addLayout(findRow, 0, 0);
    layout->addWidget(m_replaceRow, 1, 0);
    m_find->setMinimumWidth(260);
    m_replace->setMinimumWidth(260);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(150);
    connect(m_refreshTimer, &QTimer::timeout, this, &FindBar::refresh);

    connect(m_find, &QLineEdit::textChanged, this, [this] {
        refresh();
        // 입력하는 동안 커서 뒤의 첫 결과로 옮겨 간다 (선택 영역 안 찾기는 커서를 옮기지 않는다).
        if (!m_matches.isEmpty() && !m_inSelection->isChecked()) {
            QTextCursor cursor = m_editor->textCursor();
            cursor.setPosition(cursor.selectionStart());
            m_editor->setTextCursor(cursor);
            findNext();
        }
    });
    for (QToolButton *button : {m_matchCase, m_wholeWord, m_regex})
        connect(button, &QToolButton::toggled, this, &FindBar::refresh);
    connect(m_inSelection, &QToolButton::toggled, this, [this](bool on) {
        if (on)
            captureSelectionRange();
        refresh();
    });
    connect(previous, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(close, &QToolButton::clicked, this, &FindBar::closeBar);
    connect(replaceOne, &QToolButton::clicked, this, &FindBar::replaceCurrent);
    connect(replaceEvery, &QToolButton::clicked, this, &FindBar::replaceAll);
    connect(m_editor->document(), &QTextDocument::contentsChanged, this, [this] {
        if (isVisible())
            m_refreshTimer->start();
    });

    m_find->installEventFilter(this);
    m_replace->installEventFilter(this);
    m_editor->installEventFilter(this);
    hide();
}

void FindBar::openFind()
{
    open(false);
}

void FindBar::openReplace()
{
    open(true);
}

bool FindBar::isReplaceMode() const
{
    return !m_replaceRow->isHidden();
}

void FindBar::open(bool replace)
{
    const QTextCursor cursor = m_editor->textCursor();
    const QString selected = cursor.selectedText();
    const bool multiLine = selected.contains(QChar::ParagraphSeparator);

    m_replaceRow->setVisible(replace);
    show();

    QSignalBlocker blocker(m_find);
    if (multiLine) {
        // 여러 줄을 고른 채 열면 그 안에서 찾는다.
        m_inSelection->setChecked(true);
        captureSelectionRange();
    } else {
        QSignalBlocker selectionBlocker(m_inSelection);
        m_inSelection->setChecked(false);
        if (!selected.isEmpty())
            m_find->setText(selected);
    }
    m_find->setFocus();
    m_find->selectAll();
    refresh();
}

void FindBar::closeBar()
{
    hide();
    m_matches.clear();
    m_current = -1;
    m_editor->setSearchMatches({});
    m_editor->setFocus();
}

void FindBar::captureSelectionRange()
{
    const QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection()) {
        QSignalBlocker blocker(m_inSelection);
        m_inSelection->setChecked(false);
        m_rangeStart = {};
        m_rangeEnd = {};
        return;
    }
    m_rangeStart = QTextCursor(m_editor->document());
    m_rangeStart.setPosition(cursor.selectionStart());
    m_rangeStart.setKeepPositionOnInsert(true); // 범위 맨 앞에 넣은 글자도 범위 안
    m_rangeEnd = QTextCursor(m_editor->document());
    m_rangeEnd.setPosition(cursor.selectionEnd());
}

void FindBar::setMatchCase(bool on)
{
    m_matchCase->setChecked(on);
}

void FindBar::setWholeWord(bool on)
{
    m_wholeWord->setChecked(on);
}

void FindBar::setRegex(bool on)
{
    m_regex->setChecked(on);
}

void FindBar::setInSelection(bool on)
{
    m_inSelection->setChecked(on);
}

QString FindBar::statusText() const
{
    return m_statusText;
}

void FindBar::showStatus(const QString &text, bool error)
{
    m_statusText = text;
    m_status->setText(text);
    m_status->setProperty("error", error);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

std::optional<QRegularExpression> FindBar::pattern(QString *error) const
{
    QString source = m_regex->isChecked() ? m_find->text() : QRegularExpression::escape(m_find->text());
    if (m_wholeWord->isChecked())
        source = QStringLiteral("(?<![\\w$#])(?:%1)(?![\\w$#])").arg(source);
    QRegularExpression::PatternOptions options = QRegularExpression::MultilineOption
                                                 | QRegularExpression::UseUnicodePropertiesOption;
    if (!m_matchCase->isChecked())
        options |= QRegularExpression::CaseInsensitiveOption;
    QRegularExpression re(source, options);
    if (!re.isValid()) {
        if (error)
            *error = re.errorString();
        return std::nullopt;
    }
    return re;
}

void FindBar::refresh()
{
    m_refreshTimer->stop();
    m_matches.clear();
    m_current = -1;
    m_truncated = false;

    const QString needle = m_find->text();
    if (needle.isEmpty() || isHidden()) {
        m_editor->setSearchMatches({});
        showStatus({});
        return;
    }
    QString error;
    const std::optional<QRegularExpression> re = pattern(&error);
    m_find->setProperty("invalid", !re.has_value());
    m_find->style()->unpolish(m_find);
    m_find->style()->polish(m_find);
    if (!re) {
        m_editor->setSearchMatches({});
        showStatus(tr("Bad pattern"), true);
        m_find->setToolTip(error);
        return;
    }
    m_find->setToolTip({});

    const QString text = m_editor->toPlainText();
    int from = 0;
    int to = int(text.size());
    if (m_inSelection->isChecked() && !m_rangeStart.isNull()) {
        from = m_rangeStart.position();
        to = m_rangeEnd.position();
    }
    QRegularExpressionMatchIterator it = re->globalMatch(text, from);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedEnd() > to)
            break;
        if (match.capturedLength() == 0)
            continue;
        if (m_matches.size() >= kMaxMatches) {
            m_truncated = true;
            break;
        }
        QTextCursor cursor(m_editor->document());
        cursor.setPosition(int(match.capturedStart()));
        cursor.setPosition(int(match.capturedEnd()), QTextCursor::KeepAnchor);
        m_matches.append(cursor);
    }

    const QTextCursor selection = m_editor->textCursor();
    m_current = matchIndexOf(selection.selectionStart(), selection.selectionEnd());
    m_editor->setSearchMatches(m_matches, m_current);
    if (m_matches.isEmpty())
        showStatus(tr("No results"), true);
    else if (m_current >= 0)
        showStatus(QStringLiteral("%1/%2%3").arg(m_current + 1).arg(QLocale().toString(m_matches.size()), m_truncated ? QStringLiteral("+") : QString()));
    else
        showStatus(tr("%1%2 results").arg(QLocale().toString(m_matches.size()), m_truncated ? QStringLiteral("+") : QString()));
}

int FindBar::matchIndexOf(int start, int end) const
{
    for (int i = 0; i < m_matches.size(); ++i) {
        if (m_matches[i].selectionStart() == start && m_matches[i].selectionEnd() == end)
            return i;
    }
    return -1;
}

void FindBar::findNext()
{
    step(true);
}

void FindBar::findPrevious()
{
    step(false);
}

void FindBar::step(bool forward)
{
    if (isHidden() || m_find->text().isEmpty()) {
        openFind();
        return;
    }
    if (m_refreshTimer->isActive() || m_matches.isEmpty())
        refresh();
    if (m_matches.isEmpty())
        return;

    const QTextCursor cursor = m_editor->textCursor();
    int index = -1;
    bool wrapped = false;
    if (forward) {
        const int position = cursor.hasSelection() ? cursor.selectionEnd() : cursor.position();
        for (int i = 0; i < m_matches.size(); ++i) {
            if (m_matches[i].selectionStart() >= position) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            index = 0;
            wrapped = true;
        }
    } else {
        const int position = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
        for (int i = int(m_matches.size()) - 1; i >= 0; --i) {
            if (m_matches[i].selectionStart() < position) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            index = int(m_matches.size()) - 1;
            wrapped = true;
        }
    }
    select(index, wrapped);
}

void FindBar::select(int index, bool wrapped)
{
    m_current = index;
    m_editor->setTextCursor(m_matches[index]);
    m_editor->ensureCursorVisible();
    m_editor->setSearchMatches(m_matches, m_current);
    QString text = QStringLiteral("%1/%2%3").arg(index + 1).arg(QLocale().toString(m_matches.size()),
                                                                    m_truncated ? QStringLiteral("+") : QString());
    if (wrapped)
        text += tr(" (wrapped)");
    showStatus(text);
}

QString FindBar::expand(const QRegularExpressionMatch &match) const
{
    // $0..$99 묶음, \n \t \\ $$ 를 바꾼다.
    const QString source = m_replace->text();
    QString out;
    for (int i = 0; i < source.size(); ++i) {
        const QChar c = source[i];
        if (c == u'$' && i + 1 < source.size()) {
            if (source[i + 1] == u'$') {
                out += u'$';
                ++i;
                continue;
            }
            if (source[i + 1].isDigit()) {
                int j = i + 1;
                int group = 0;
                while (j < source.size() && source[j].isDigit() && j < i + 3) {
                    const int next = group * 10 + source[j].digitValue();
                    if (next > match.lastCapturedIndex() && j > i + 1)
                        break;
                    group = next;
                    ++j;
                }
                out += match.captured(group);
                i = j - 1;
                continue;
            }
        }
        if (c == u'\\' && i + 1 < source.size()) {
            const QChar n = source[i + 1];
            if (n == u'n' || n == u't' || n == u'\\') {
                out += n == u'n' ? QStringLiteral("\n") : n == u't' ? QStringLiteral("\t") : QStringLiteral("\\");
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

QString FindBar::replacementFor(const QString &text, int start, int end) const
{
    if (!m_regex->isChecked())
        return m_replace->text();
    const std::optional<QRegularExpression> re = pattern();
    if (!re)
        return m_replace->text();
    const QRegularExpressionMatch match =
        re->match(text, start, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch() || match.capturedEnd() != end)
        return m_replace->text();
    return expand(match);
}

bool FindBar::replaceCurrent()
{
    refresh();
    QTextCursor cursor = m_editor->textCursor();
    const int index = matchIndexOf(cursor.selectionStart(), cursor.selectionEnd());
    if (index < 0) {
        findNext(); // 먼저 다음 결과를 고른다
        return false;
    }
    const QString replacement = replacementFor(m_editor->toPlainText(), cursor.selectionStart(), cursor.selectionEnd());
    cursor.insertText(replacement);
    m_editor->setTextCursor(cursor);
    refresh();
    if (!m_matches.isEmpty())
        findNext();
    return true;
}

int FindBar::replaceAll()
{
    refresh();
    if (m_matches.isEmpty())
        return 0;

    // 바꿀 글자는 바꾸기 전의 내용으로 미리 정하고, 뒤에서부터 바꿔서 앞쪽 위치가 흔들리지 않게 한다.
    const QString text = m_editor->toPlainText();
    QList<QPair<QPair<int, int>, QString>> edits;
    for (const QTextCursor &match : std::as_const(m_matches)) {
        const int start = match.selectionStart();
        const int end = match.selectionEnd();
        edits.append({{start, end}, replacementFor(text, start, end)});
    }
    QTextCursor cursor(m_editor->document());
    cursor.beginEditBlock();
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        cursor.setPosition(it->first.first);
        cursor.setPosition(it->first.second, QTextCursor::KeepAnchor);
        cursor.insertText(it->second);
    }
    cursor.endEditBlock();

    const int count = int(edits.size());
    refresh();
    showStatus(tr("Replaced %n", nullptr, count));
    return count;
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);

    if (watched == m_editor) {
        // 에디터에서 Esc: 세로 커서가 없으면 찾기 막대도 닫는다 (에디터도 Esc를 그대로 받는다).
        if (key->key() == Qt::Key_Escape && isVisible() && !m_editor->hasExtraCursors())
            closeBar();
        return false;
    }

    const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    if (key->key() == Qt::Key_Escape) {
        closeBar();
        return true;
    }
    if (enter && watched == m_find) {
        if (key->modifiers() & Qt::ShiftModifier)
            findPrevious();
        else
            findNext();
        return true;
    }
    if (enter && watched == m_replace) {
        if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
            replaceAll();
        else
            replaceCurrent();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
