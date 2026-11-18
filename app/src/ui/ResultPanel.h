#pragma once

#include "QuerySession.h"
#include "core/ResultExport.h"

#include <QCborArray>
#include <QElapsedTimer>
#include <QTabWidget>

class PlanView;
class QLabel;
class QPlainTextEdit;
class QTableView;
class QTimer;
class ResultModel;

// 에디터 아래의 결과 영역. 결과 표, 실행 로그, 서버 출력(DBMS_OUTPUT, 경고), 실행 계획을 탭으로 보여 준다.
// 오른쪽 위에는 마지막 문장의 행 수와 걸린 시간을 보여 준다 (실행 중에는 흐르는 시간).
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

    void setRunning(bool running);
    void showExecutionInfo(const ExecutionInfo &info);
    void showPlan(const QCborMap &plan);

    // 내보내기 (Excel, CSV, SQL INSERT ...). 대화상자에서 고르거나 바로 부른다.
    // filePath가 비어 있으면 클립보드로. 전체 행이고 남은 행이 있으면 먼저 모두 가져온다.
    void setDialect(SqlDialect dialect) { m_dialect = dialect; }
    void setResultSql(const QString &sql) { m_resultSql = sql; }
    void showExportDialog();
    void exportResults(const ExportOptions &options, bool selectionOnly, const QString &filePath);
    bool isExporting() const { return m_exportPending; }

    ResultModel *model() const { return m_model; }
    PlanView *planView() const { return m_plan; }
    QString infoText() const;
    static QString formatDuration(qint64 ms);

signals:
    void moreRowsRequested();
    void fetchAllRequested(); // 내보내기 전에 남은 행을 모두 가져와 달라
    void exportFinished(const QString &message, bool ok);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void copySelection(bool withHeader);
    void finishExport();
    void cancelExport(const QString &reason);
    QString suggestedTableName() const;
    void fitColumns();
    void updateRunningText();
    void setInfo(const QString &text, const QString &toolTip = {});
    void placeInfo();

    ResultModel *m_model = nullptr;
    QTableView *m_grid = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_output = nullptr;
    PlanView *m_plan = nullptr;
    QLabel *m_info = nullptr;
    QTimer *m_runningTimer = nullptr;
    QElapsedTimer m_runClock;
    bool m_running = false;
    bool m_infoDuringRun = false;
    ExecutionInfo m_lastInfo;
    bool m_columnsFitted = false;

    SqlDialect m_dialect = SqlDialect::Generic;
    QString m_resultSql;
    bool m_exportPending = false;
    ExportOptions m_exportOptions;
    bool m_exportSelectionOnly = false;
    QString m_exportPath;
};
