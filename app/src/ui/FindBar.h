#pragma once

#include <QList>
#include <QRegularExpression>
#include <QTextCursor>
#include <QWidget>

#include <optional>

class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class SqlEditor;

// 에디터 위의 찾기/바꾸기 막대 (Ctrl+F, Ctrl+R).
// - 대소문자 구분(Aa), 단어 단위(W), 정규식(.*), 선택 영역 안에서만
// - 찾은 곳을 모두 칠하고 "3/12"처럼 위치를 보여 준다. 편집하면 다시 찾는다
// - Enter 다음, Shift+Enter 이전, Esc 닫기. 정규식 바꾸기는 $1, $2 ... 로 묶음을 쓴다 (\n, \t도 됨)
// - 모두 바꾸기는 한 번의 실행 취소(Ctrl+Z)로 되돌린다
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(SqlEditor *editor, QWidget *parent = nullptr);

    void openFind();
    void openReplace();
    void closeBar();
    void findNext();
    void findPrevious();
    bool replaceCurrent();
    int replaceAll();

    QLineEdit *findField() const { return m_find; }
    QLineEdit *replaceField() const { return m_replace; }
    void setMatchCase(bool on);
    void setWholeWord(bool on);
    void setRegex(bool on);
    void setInSelection(bool on);
    bool isReplaceMode() const;

    int matchCount() const { return int(m_matches.size()); }
    int currentMatch() const { return m_current; }
    QString statusText() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void open(bool replace);
    void refresh();
    void step(bool forward);
    void select(int index, bool wrapped);
    void showStatus(const QString &text, bool error = false);
    int matchIndexOf(int start, int end) const;
    std::optional<QRegularExpression> pattern(QString *error = nullptr) const;
    QString replacementFor(const QString &text, int start, int end) const;
    QString expand(const QRegularExpressionMatch &match) const;
    void captureSelectionRange();

    SqlEditor *m_editor = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_replace = nullptr;
    QWidget *m_replaceRow = nullptr;
    QToolButton *m_matchCase = nullptr;
    QToolButton *m_wholeWord = nullptr;
    QToolButton *m_regex = nullptr;
    QToolButton *m_inSelection = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QList<QTextCursor> m_matches;
    int m_current = -1;
    bool m_truncated = false;
    QString m_statusText;
    QTextCursor m_rangeStart; // 선택 영역 안에서만 찾을 때의 범위 (편집하면 함께 움직인다)
    QTextCursor m_rangeEnd;
};
