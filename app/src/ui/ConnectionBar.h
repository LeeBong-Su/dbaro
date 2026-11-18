#pragma once

#include "core/ConnectionProfile.h"

#include <QWidget>

#include <optional>

class QLabel;

// 쿼리 탭 맨 위의 연결 정보 표시줄. 지금 어느 DB에 붙어 있는지 크게 보여 준다.
// 연결 이름, 접속 대상(DB 종류·호스트·DB·사용자), 상태, 환경·읽기 전용·커밋 안 된 변경 배지.
// 배경에 환경 색을 은은하게 깔고(운영은 더 진하게), 왼쪽에 환경 색 막대를 둔다.
class ConnectionBar : public QWidget
{
    Q_OBJECT

public:
    enum class State { NotConnected, Connecting, Connected, Running };

    explicit ConnectionBar(QWidget *parent = nullptr);

    void setProfile(const ConnectionProfile *profile);
    void setState(State state, bool pendingTransaction, const QString &serverInfo);
    void setSchema(const QString &schema); // USE로 바꾼 기본 데이터베이스 (MySQL·MariaDB)
    QString schemaText() const;

    QString nameText() const;
    QString stateText() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    static QString target(const ConnectionProfile &profile);

    std::optional<ConnectionProfile> m_profile;
    State m_state = State::NotConnected;
    bool m_pending = false;
    QString m_serverInfo;
    QString m_schema;

    QLabel *m_dot = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_schemaLabel = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_environment = nullptr;
    QLabel *m_readOnly = nullptr;
    QLabel *m_pendingBadge = nullptr;
    QLabel *m_stateLabel = nullptr;
};
