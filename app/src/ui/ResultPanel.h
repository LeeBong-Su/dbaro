#pragma once

#include <QTabWidget>

class QPlainTextEdit;
class QTableView;

// 에디터 아래의 결과 영역. 결과 그리드, 실행 로그, 서버 출력(DBMS_OUTPUT 등)을 탭으로 보여 준다.
class ResultPanel : public QTabWidget
{
    Q_OBJECT

public:
    explicit ResultPanel(QWidget *parent = nullptr);

    void appendLog(const QString &message);
    void showLog();

private:
    QTableView *m_grid = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_output = nullptr;
};
