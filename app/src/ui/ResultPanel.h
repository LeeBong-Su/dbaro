#pragma once

#include "QuerySession.h"
#include "core/ResultExport.h"

#include <QCborArray>
#include <QElapsedTimer>
#include <QTabWidget>

class PlanView;
class QAction;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QSplitter;
class QTableView;
class QTableWidget;
class QTimer;
class QToolBar;
class ResultModel;
class ResultProxyModel;

// 에디터 아래의 결과 영역. 결과 표, 실행 로그, 서버 출력(DBMS_OUTPUT, 경고), 실행 계획을 탭으로 보여 준다.
// 오른쪽 위에는 마지막 문장의 행 수와 걸린 시간을 보여 준다 (실행 중에는 흐르는 시간).
//
// 결과 표 위의 도구 막대:
// - 필터: 입력한 단어가 모두 들어 있는 행만 (셀 오른쪽 클릭 → 이 값으로 거르기). 머리글을 누르면 정렬 (오름 → 내림 → 원래 순서)
//   거르기·정렬은 가져온 행 안에서 한다. Fetch All로 남은 행을 모두 가져온다
// - Record: 오른쪽에 지금 행을 세로로(컬럼 | 값), 지금 셀의 긴 값을 통째로(JSON은 보기 좋게) 보여 준다
// - Edit: 테이블 하나에서 나온 결과면 셀을 고치고 행을 더하거나 지운 뒤 UPDATE·INSERT·DELETE로 반영한다
// - Export: Excel, CSV, SQL INSERT, JSON, Markdown
class ResultPanel : public QTabWidget
{
    Q_OBJECT

public:
    explicit ResultPanel(QWidget *parent = nullptr);

    void startResult(const QCborArray &columns);
    void appendRows(const QCborArray &rows, bool hasMore);
    void appendLog(const QString &message, bool isError = false);
    void appendOutput(const QStringList &lines);
    void clearLog();
    void clearOutput();
    QPlainTextEdit *logView() const { return m_log; }
    QPlainTextEdit *outputView() const { return m_output; }
    void showLog();
    void showResult();

    void setRunning(bool running);
    void showExecutionInfo(const ExecutionInfo &info);
    void showPlan(const QCborMap &plan);

    // 내보내기 (Excel, CSV, SQL INSERT ...). 대화상자에서 고르거나 바로 부른다.
    // filePath가 비어 있으면 클립보드로. 전체 행이고 남은 행이 있으면 먼저 모두 가져온다.
    void setDialect(SqlDialect dialect) { m_dialect = dialect; }
    void setResultSql(const QString &sql) { m_resultSql = sql; }
    QString resultSql() const { return m_resultSql; }
    void showExportDialog();
    void exportResults(const ExportOptions &options, bool selectionOnly, const QString &filePath);
    bool isExporting() const { return m_exportPending; }

    // 거르기·정렬·세로 보기
    void setQuickFilter(const QString &text);
    void filterByValue(int row, int column); // 원본 행·컬럼 위치
    void clearFilters();
    void sortByColumn(int column, Qt::SortOrder order); // column -1: 원래 순서
    void fetchAll();
    void setRecordViewVisible(bool visible);
    QSortFilterProxyModel *viewModel() const;
    QTableWidget *recordView() const { return m_record; }
    QPlainTextEdit *valueView() const { return m_value; }

    // 편집. Edit을 누르면 editRequested가 나가고, QueryTab이 테이블·기본 키를 알아낸 뒤 beginEditing을 부른다.
    void beginEditing(const QString &table, const QList<bool> &editableColumns, const QList<int> &keyColumns);
    void editUnavailable(const QString &reason);
    bool hasPendingEdits() const;
    QStringList editStatements(QString *error = nullptr) const;
    void acceptEdits();  // DB에 반영했다
    void discardEdits(); // 편집을 끝내고 바꾼 것을 버린다
    bool isEditing() const;

    ResultModel *model() const { return m_model; }
    PlanView *planView() const { return m_plan; }
    QString infoText() const;
    static QString formatDuration(qint64 ms);

signals:
    void moreRowsRequested();
    void fetchAllRequested(); // 남은 행을 모두 가져와 달라 (내보내기, Fetch All)
    void exportFinished(const QString &message, bool ok);
    void editRequested();
    void applyEditsRequested(const QStringList &statements);
    void planObjectRequested(const QCborMap &object); // 실행 계획에서 오퍼레이션을 눌렀다

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void createResultPage();
    void copySelection(bool withHeader);
    QList<int> selectedSourceRows() const;
    void finishExport();
    void cancelExport(const QString &reason);
    QString suggestedTableName() const;
    void fitColumns();
    void updateRunningText();
    void setInfo(const QString &text, const QString &toolTip = {});
    void placeInfo();
    void updateToolbar();
    void updateSidePanel();
    void onHeaderClicked(int column);
    void setEditMode(bool on);
    void previewAndApply();
    void addRow();
    void deleteRows();
    void setSelectedNull();

    ResultModel *m_model = nullptr;
    ResultProxyModel *m_proxy = nullptr;
    QWidget *m_resultPage = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_rowsLabel = nullptr;
    QSplitter *m_splitter = nullptr;
    QTabWidget *m_side = nullptr;
    QTableWidget *m_record = nullptr;
    QPlainTextEdit *m_value = nullptr;
    QAction *m_fetchAllAction = nullptr;
    QAction *m_recordAction = nullptr;
    QAction *m_editAction = nullptr;
    QAction *m_addRowAction = nullptr;
    QAction *m_deleteRowsAction = nullptr;
    QAction *m_setNullAction = nullptr;
    QAction *m_revertAction = nullptr;
    QAction *m_applyAction = nullptr;
    QAction *m_filterByValueAction = nullptr;
    QAction *m_clearFiltersAction = nullptr;
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
    bool m_fetchAllPending = false;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    QString m_editTable;
    QList<int> m_editKeys;
    bool m_editWaiting = false; // Edit을 눌러 테이블 정보를 기다리는 중

    SqlDialect m_dialect = SqlDialect::Generic;
    QString m_resultSql;
    bool m_exportPending = false;
    ExportOptions m_exportOptions;
    bool m_exportSelectionOnly = false;
    QString m_exportPath;
};
