#pragma once

#include <QCborArray>
#include <QTabWidget>

class QPlainTextEdit;
class QTableView;
class ResultModel;

// 에디터 아래의 결과 영역. 결과 표, 실행 로그, 서버 출력(DBMS_OUTPUT, 경고)을 탭으로 보여 준다.
class ResultPanel : public QTabWidget
{
    Q_OBJECT

public:
    explicit ResultPanel(QWidget *parent = nullptr);

    void startResult(const QCborArray &columns);
    void appendRows(const QCborArray &rows, bool hasMore);
    void appendLog(const QString &message, bool isError = false);
    void appendOutput(const QStringList &lines);
    void showLog();
    void showResult();

    ResultModel *model() const { return m_model; }

signals:
    void moreRowsRequested();

private:
    void copySelection(bool withHeader);
    void fitColumns();

    ResultModel *m_model = nullptr;
    QTableView *m_grid = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_output = nullptr;
    bool m_columnsFitted = false;
};
