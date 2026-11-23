#pragma once

#include "core/SqlDialect.h"
#include "core/SqlSplitter.h"

#include <QList>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTextEdit>

#include <functional>

class QTimer;
class SqlCompleter;
class SqlHighlighter;

// SQL 에디터. QPlainTextEdit 위에 다음을 더한다.
// - 줄 번호와 현재 문장 표시(줄 번호 옆 막대), 현재 줄·괄호 짝 강조
// - 방언별 문법 하이라이트와 문장 분리 (실행 범위 계산)
// - 세로 커서(블록 선택): Alt+드래그, Alt+Shift+방향키, Ctrl+Alt+위/아래
//   여러 줄에 동시에 입력·삭제·붙여넣기. 한글처럼 조합해서 입력하는 글자도 모든 커서에 들어간다.
// - Tab/Shift+Tab 들여쓰기, Ctrl+/ 줄 주석, Enter 시 들여쓰기 유지
// - 자동완성 (SqlCompleter): 입력 중 또는 Ctrl+Space
class SqlEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit SqlEditor(QWidget *parent = nullptr);

    void setDialect(SqlDialect dialect);
    SqlDialect dialect() const { return m_dialect; }

    // 실행할 문장. wholeScript면 전체, 아니면 선택 영역 안의 문장들, 선택이 없으면 커서가 있는 문장.
    QList<SqlStatement> statementsToExecute(bool wholeScript);
    QList<SqlStatement> statements();

    SqlCompleter *completer() const { return m_completer; }

    // 선택한 글자 (한 줄). 선택이 없으면 커서가 있는 이름 (schema.name, 따옴표 포함)
    QString objectNameAtCursor() const;

    // 세로 커서 수 (일반 상태면 1)
    int cursorCount() const { return m_cursors.size() > 1 ? m_cursors.size() : 1; }
    bool hasExtraCursors() const { return m_cursors.size() > 1; }

    // 찾기 막대가 찾은 곳 (모두 옅게, current번째는 진하게 칠한다). 빈 목록이면 지운다.
    void setSearchMatches(const QList<QTextCursor> &matches, int current = -1);

    // 두 번 클릭한 단어와 같은 단어(대소문자 무시, 단어 단위)가 있는 곳. 선택이 바뀌거나 글을 고치면 지운다.
    const QList<QTextCursor> &wordMarks() const { return m_wordMarks; }
    QString markedWord() const { return m_markedWord; }
    void markWordAt(int position);
    void clearWordMarks();

    int lineNumberAreaWidth() const;
    void paintLineNumberArea(QPaintEvent *event);

signals:
    // 두 번 클릭한 단어가 count곳에 있다 (word가 비어 있으면 표시를 지웠다)
    void wordMarksChanged(const QString &word, int count);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override; // Ctrl + 휠: 글자 크기

private:
    void processKey(QKeyEvent *event);
    // 줄 번호 영역과 강조
    void updateLineNumberAreaWidth();
    void applyFontSize(qreal pointSize);
    void updateLineNumberArea(const QRect &rect, int dy);
    void updateExtraSelections();
    void appendBracketMatch(QList<QTextEdit::ExtraSelection> &selections) const;
    void onCursorPositionChanged();

    // 문장 분석
    void reparse();
    void updateCurrentStatement();

    // 세로 커서
    bool handleColumnSelectionKey(QKeyEvent *event);
    bool handleMultiCursorKey(QKeyEvent *event);
    void startColumnSelection(int line, qreal x);
    void updateColumnSelection(int headLine, qreal headX);
    void clearExtraCursors();
    void syncPrimaryCursor();
    void editAllCursors(const std::function<void(QTextCursor &)> &edit);
    void moveAllCursors(const std::function<void(QTextCursor &)> &move);
    void insertAtAllCursors(const QString &text);
    void copyFromCursors() const;
    void pasteToCursors();
    int positionAt(int line, qreal x) const;
    qreal xOf(const QTextCursor &cursor) const;

    // 편집 도우미
    bool selectionSpansLines() const;
    void indentLines(bool outdent);
    void toggleLineComment();

    QWidget *m_lineNumberArea = nullptr;
    SqlHighlighter *m_highlighter = nullptr;
    SqlDialect m_dialect = SqlDialect::Generic;

    QTimer *m_parseTimer = nullptr;
    QList<SqlStatement> m_statements;
    SqlCompleter *m_completer = nullptr;
    bool m_statementsDirty = true;
    int m_currentStatement = -1;

    QList<QTextCursor> m_searchMatches;
    int m_currentSearchMatch = -1;
    QList<QTextCursor> m_wordMarks;
    QString m_markedWord;

    QList<QTextCursor> m_cursors; // 세로 커서 (2개 이상일 때만 사용)
    int m_primaryIndex = 0;       // m_cursors 중 주 커서 (깜빡이는 커서, 한글 조합 위치)
    bool m_updatingCursors = false;
    bool m_columnDragging = false;
    bool m_columnRect = false; // 아직 편집하지 않은 사각형 선택 상태
    int m_columnAnchorLine = 0;
    qreal m_columnAnchorX = 0;
    int m_columnHeadLine = 0;
    qreal m_columnHeadX = 0;
};
