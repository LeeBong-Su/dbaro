#pragma once

#include <QCborMap>
#include <QWidget>

class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;

// 실행 계획의 오퍼레이션을 누르면 뜨는 객체 정보 창 (연결 창마다 하나, 누를 때마다 내용만 바뀐다).
// 그 단계가 읽는 테이블의 구조와 통계, 인덱스(통계 포함)를 보여 주고, 계획이 쓴 인덱스를 눈에 띄게 칠한다.
// 인덱스를 읽는 단계(INDEX RANGE SCAN 등)면 그 인덱스가 걸린 테이블을 찾아 보여 준다.
class PlanObjectWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PlanObjectWindow(QWidget *parent = nullptr);

    // 불러오기 시작. 돌려준 번호로 결과를 넘겨야 한다 (그 사이 다른 오퍼레이션을 누르면 늦게 온 결과는 버린다).
    int beginLoading(const QString &display);
    void showResult(int ticket, const QCborMap &result);
    void showError(int ticket, const QString &message);

    QString tableSchema() const { return m_schema; }
    QString tableName() const { return m_table; }
    QString usedIndex() const { return m_index; }
    QLabel *titleLabel() const { return m_title; }
    QLabel *summaryLabel() const { return m_summary; }
    QTableWidget *indexTable() const { return m_indexes; }
    QTableWidget *columnTable() const { return m_columns; }
    QTableWidget *statsTable() const { return m_stats; }
    QTabWidget *pages() const { return m_pages; }
    bool isLoading() const { return m_loading; }

signals:
    void tableDetailsRequested(const QString &schema, const QString &name);

private:
    int m_ticket = 0;
    bool m_loading = false;
    QString m_schema;
    QString m_table;
    QString m_index;

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QTabWidget *m_pages = nullptr;
    QTableWidget *m_indexes = nullptr;
    QTableWidget *m_columns = nullptr;
    QTableWidget *m_stats = nullptr;
    QPushButton *m_detailsButton = nullptr;
};
