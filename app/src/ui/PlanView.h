#pragma once

#include <QCborMap>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QStandardItemModel;
class QToolButton;
class QTreeView;

// 예상 실행 계획. 엔진이 준 계획을 트리(단계별 비용·행 수·조건)로 보여 주고,
// DB 고유 형식의 글(Oracle DBMS_XPLAN, PostgreSQL EXPLAIN 등)이 있으면 Text로 바꿔 볼 수 있다.
// 테이블·인덱스를 읽는 단계는 객체 이름이 링크처럼 보이고, 누르면(Enter, 오른쪽 클릭 메뉴) objectRequested를 보낸다.
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
    QPlainTextEdit *m_text = nullptr;
};
