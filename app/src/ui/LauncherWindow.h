#pragma once

#include <QWidget>

class AppController;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// 연결 선택 창 (시작 화면). 저장된 연결을 최근에 연 순서로 보여 주고, 고른 연결을 그 연결의 창으로 연다.
// 새 연결 만들기·편집도 여기서 한다.
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
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyFilter();
    void updateButtons();
    QString selectedId() const;

    AppController *m_app = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_edit = nullptr;
    QCheckBox *m_reopen = nullptr;
};
