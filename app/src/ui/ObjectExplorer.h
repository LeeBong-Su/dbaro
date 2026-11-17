#pragma once

#include "core/ConnectionProfile.h"

#include <QList>
#include <QWidget>

class QLineEdit;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

// 왼쪽 패널. 연결 → 스키마 → 객체(테이블, 뷰, 프로시저 등) 트리와 이름 필터.
// 지금은 연결 목록만 보여 준다. 스키마와 객체는 엔진의 메타데이터 조회가 생기면 붙인다.
class ObjectExplorer : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectExplorer(QWidget *parent = nullptr);

    void setConnections(const QList<ConnectionProfile> &profiles);

signals:
    // 연결을 두 번 클릭했을 때. 현재 쿼리 탭의 연결로 지정하는 데 쓴다.
    void connectionActivated(const QString &connectionId);

private:
    QLineEdit *m_filter = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
};
