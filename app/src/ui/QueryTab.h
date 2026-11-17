#pragma once

#include <QWidget>

class QColor;
class QFrame;
class QPlainTextEdit;
class QSplitter;
class ResultPanel;

// 쿼리 탭 하나. 위는 SQL 에디터, 아래는 결과 패널이다.
class QueryTab : public QWidget
{
    Q_OBJECT

public:
    explicit QueryTab(QWidget *parent = nullptr);

    QPlainTextEdit *editor() const { return m_editor; }
    ResultPanel *resultPanel() const { return m_results; }

    // 연결 색상(예: 운영 DB는 빨강)을 탭 위쪽 띠로 표시한다. 유효하지 않은 색이면 띠를 숨긴다.
    void setConnectionColor(const QColor &color);

signals:
    void cursorPositionChanged(int line, int column);

private:
    QFrame *m_colorStrip = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    ResultPanel *m_results = nullptr;
    QSplitter *m_splitter = nullptr;
};
