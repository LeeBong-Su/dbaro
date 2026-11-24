#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

// 오래 걸리는 일을 알리는 표시들.
// 금방 끝나는 일에 깜빡이지 않도록 조금 기다렸다가(기본 0.4초) 나타난다.

// 돌아가는 원호 (테마의 강조색)
class BusySpinner : public QWidget
{
    Q_OBJECT

public:
    explicit BusySpinner(int size, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    // 탭 아이콘처럼 그림 한 장이 필요할 때. angle은 도(0~359)
    static QPixmap frame(int size, int angle, const QColor &color, qreal devicePixelRatio);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    int m_size;
    int m_angle = 0;
    QTimer *m_timer = nullptr;
};

// 대상 위젯(결과 영역 등)을 흐리게 덮고 가운데에 카드를 띄운다: 돌아가는 원호, 하는 일, 문장, 걸린 시간, 취소.
// 흐린 막은 마우스를 막지 않아 이전 결과를 계속 스크롤·복사할 수 있고, 카드만 마우스를 받는다.
// 작게(compact) 하면 흐리게 덮지 않고 카드만 오른쪽 위에 둔다 (실행 중에도 줄이 늘어나는 Log를 볼 때).
class BusyOverlay : public QObject
{
    Q_OBJECT

public:
    explicit BusyOverlay(QWidget *target, QObject *parent = nullptr);

    void start(const QString &title, const QString &detail = {}, bool cancellable = false);
    void setText(const QString &title, const QString &detail);
    void stop();
    // 흐리게 덮지 않고 카드만 오른쪽 위에. start·stop과 따로 기억한다
    void setCompact(bool compact);
    void setDelay(int ms) { m_delayMs = ms; }

    bool isActive() const { return m_active; }   // start 뒤 stop 전
    bool isShown() const;                         // 지금 화면에 보임
    bool isCompact() const { return m_compact; }
    bool isDimmed() const;                        // 대상을 흐리게 덮고 있음
    QString title() const;
    QString detail() const;
    QString elapsedText() const;
    QPushButton *cancelButton() const { return m_cancel; }

signals:
    void cancelRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateVisibility();
    void place();
    void tick();

    QPointer<QWidget> m_target;
    QWidget *m_dim = nullptr;
    QFrame *m_card = nullptr;
    BusySpinner *m_spinner = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_elapsed = nullptr;
    QPushButton *m_cancel = nullptr;
    QTimer *m_delay = nullptr;
    QTimer *m_clockTimer = nullptr;
    QElapsedTimer m_clock;
    QString m_fullDetail;
    int m_delayMs = 400;
    bool m_active = false;
    bool m_due = false; // 기다린 시간이 지남
    bool m_compact = false;
};

// 대상 위젯의 오른쪽 위에 붙는 작은 알림: 돌아가는 원호와 "DDL 만드는 중" 같은 글.
// 여러 일을 함께 기다리면 마지막 일을 보이고 남은 수를 붙인다. 마우스는 막지 않는다.
class BusyBadge : public QFrame
{
    Q_OBJECT

public:
    explicit BusyBadge(QWidget *target);

    int begin(const QString &text); // 표시할 일 하나를 시작하고 그 번호를 돌려준다
    void end(int ticket);
    void setDelay(int ms) { m_delayMs = ms; }
    bool isBusy() const { return !m_tasks.isEmpty(); }
    QString text() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void refresh();
    void place();

    QPointer<QWidget> m_target;
    BusySpinner *m_spinner = nullptr;
    QLabel *m_label = nullptr;
    QTimer *m_delay = nullptr;
    QList<QPair<int, QString>> m_tasks;
    int m_nextTicket = 1;
    int m_delayMs = 300;
};
