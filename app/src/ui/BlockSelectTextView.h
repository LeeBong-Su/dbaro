#pragma once

#include <QList>
#include <QPlainTextEdit>
#include <QTextCursor>

// 읽기 전용 글 상자 + Alt+드래그 사각형(세로) 선택.
// 실행 계획 글처럼 열을 맞춘 글에서 컬럼만 골라 복사할 때 쓴다 (화면에 보이는 사각형, 글자 가운데가 들어오면 포함).
// - Alt+드래그: 사각형 선택 (보통 선택은 푼다). 화면 밖으로 끌면 그쪽으로 스크롤한다
// - Ctrl+C·오른쪽 클릭 Copy: 사각형이면 줄마다 고른 글을 줄바꿈으로 이어 복사, 아니면 보통 선택
// - 그냥 누르기·Esc·다른 키·글이 바뀌면 사각형 선택은 풀린다
// (CLI 탭의 ConsoleView도 같은 방식으로 사각형을 고른다)
class BlockSelectTextView : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit BlockSelectTextView(QWidget *parent = nullptr);

    bool hasBlockSelection() const { return !m_blockCursors.isEmpty(); }
    QString blockSelectionText() const;  // 줄마다 고른 글을 줄바꿈으로 이은 것
    void selectBlock(const QPoint &from, const QPoint &to); // 화면(viewport) 좌표
    void clearBlockSelection();
    void copySelection(); // 사각형이 있으면 그것, 아니면 보통 선택

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void updateBlockSelection();

    bool m_blockDragging = false;
    int m_anchorLine = 0;
    int m_headLine = 0;
    qreal m_anchorX = 0; // 문서 기준 x (가로 스크롤 포함)
    qreal m_headX = 0;
    QList<QTextCursor> m_blockCursors;
};
