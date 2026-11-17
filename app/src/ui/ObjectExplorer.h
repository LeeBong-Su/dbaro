#pragma once

#include <QWidget>

class QLineEdit;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

// 왼쪽 패널. 연결 → 스키마 → 객체(테이블, 뷰, 프로시저 등) 트리와 이름 필터.
class ObjectExplorer : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectExplorer(QWidget *parent = nullptr);

private:
    QLineEdit *m_filter = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
};
