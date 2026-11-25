#pragma once

#include <QWidget>

class AppController;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

class QTimer;

// 연결 선택 창 (시작 화면). 저장된 연결을 최근에 연 순서로 보여 주고, 고른 연결을 그 연결의 창으로 연다.
// 새 연결 만들기·편집도 여기서 한다.
// 컬럼 머리글을 누르면 그 컬럼으로 정렬(오름 → 내림 → 최근 순), 끌어서 순서·너비를 바꾼다. 컬럼 상태는 기억한다.
class LauncherWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherWindow(AppController *app, QWidget *parent = nullptr);

    void refresh();
    void openSelected();
    void setFilterText(const QString &text);
    QTreeWidget *list() const { return m_list; }

signals:
    void closedByUser();

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyFilter();
    void updateButtons();
    QString selectedId() const;
    void setUpColumns();
    void resetColumns();     // 처음 너비·순서, 최근 순 정렬
    void onSortChanged(int column);
    void saveColumns();

    AppController *m_app = nullptr;
    QTimer *m_saveColumnsTimer = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_edit = nullptr;
    QCheckBox *m_reopen = nullptr;
};
