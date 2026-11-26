#pragma once

#include <QCborMap>
#include <QWidget>

class QLabel;
class BlockSelectTextView;
class QStackedWidget;
class QStandardItemModel;
class QToolButton;
class QTreeView;

// 실행 계획 (예상 계획, 또는 실행해서 얻은 실제 계획). 엔진이 준 계획을 트리(단계별 비용·행 수·조건)로 보여 주고,
// DB 고유 형식의 글(Oracle DBMS_XPLAN, PostgreSQL EXPLAIN 등)이 있으면 Text로 바꿔 볼 수 있다.
// 실제 계획은 예상과 실제 행 수가 크게 다른 단계를 눈에 띄게 칠하고, 실행되지 않은 단계는 흐리게 보인다.
// 테이블·인덱스를 읽는 단계는 객체 이름이 링크처럼 보이고, 그 링크를 두 번 클릭하면(또는 오른쪽 클릭 메뉴) objectRequested를 보낸다.
class PlanView : public QWidget
{
    Q_OBJECT

public:
    explicit PlanView(QWidget *parent = nullptr);

    void setPlan(const QCborMap &plan);
    void clear();

    QTreeView *tree() const { return m_tree; }
    QStandardItemModel *model() const { return m_model; }
    QString planText() const;
    bool showsText() const;
    QString title() const;
    bool isActual() const { return m_actual; }
    // 예상과 실제 행 수가 크게 다르다고 볼 기준: 10배 이상이고 차이가 100행 이상
    static bool isMisestimate(qint64 estimated, qint64 actual);
    // 그 줄의 단계가 읽는 객체 {schema, name, type, index}. 없으면 빈 맵
    QCborMap objectAt(const QModelIndex &index) const;

signals:
    void objectRequested(const QCborMap &object);

private:
    void showTree(bool tree);
    void requestObject(const QModelIndex &index);

    QLabel *m_title = nullptr;
    QToolButton *m_treeButton = nullptr;
    QToolButton *m_textButton = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    BlockSelectTextView *m_text = nullptr;
    bool m_actual = false;
    int m_linkColumn = 0; // 객체 이름 링크가 있는 컬럼 (Object, 없으면 첫 칸)
};
