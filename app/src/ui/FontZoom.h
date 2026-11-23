#pragma once

#include <QObject>

class QWheelEvent;

// Ctrl + 마우스 휠로 바꾸는 글자 크기. SQL 에디터와 CLI 탭이 따로 하나씩 갖고,
// 바꾸면 모든 창의 같은 종류 화면에 바로 적용하고 설정에 남긴다 (새 탭, 다음 실행도 같은 크기).
class FontZoom : public QObject
{
    Q_OBJECT

public:
    enum class Target { Editor, Console };

    static FontZoom *instance();

    static constexpr qreal kMinimum = 6;
    static constexpr qreal kMaximum = 48;

    // 저장한 크기 (없으면 fallback)
    qreal pointSize(Target target, qreal fallback) const;
    void setPointSize(Target target, qreal pointSize);
    // Ctrl이 눌린 휠이면 크기를 바꾸고 true (휠 한 칸에 1pt, 터치패드의 작은 값은 모아서)
    bool handleWheel(QWheelEvent *event, Target target, qreal current);

signals:
    void changed(FontZoom::Target target, qreal pointSize);

private:
    explicit FontZoom(QObject *parent = nullptr);
    int m_remainder[2] = {0, 0};
};
