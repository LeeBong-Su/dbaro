#pragma once

#include "core/ConnectionProfile.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;

// 쿼리 탭 맨 위의 연결 정보 표시줄. 지금 어느 DB에 붙어 있는지 크게 보여 준다.
// 연결 이름, 접속 대상(DB 종류·호스트·DB·사용자), 상태, 환경·읽기 전용·커밋 안 된 변경 배지.
// 배경에 환경 색을 은은하게 깔고(운영은 더 진하게), 왼쪽에 환경 색 막대를 둔다.
// 접속하면 이름 옆에 이 탭의 지금 스키마(MySQL·MariaDB는 데이터베이스)가 목록으로 나와서 바로 바꿀 수 있다.
class ConnectionBar : public QWidget
{
    Q_OBJECT

public:
    enum class State { NotConnected, Connecting, Connected, Running };

    explicit ConnectionBar(QWidget *parent = nullptr);

    void setProfile(const ConnectionProfile *profile);
    void setState(State state, bool pendingTransaction, const QString &serverInfo);
    void setSchema(const QString &schema);         // 이 탭의 지금 스키마 (고르지 않고 보여 주기만)
    void setSchemas(const QStringList &schemas);   // 고를 수 있는 스키마 목록
    QString schemaText() const;                    // 보이는 지금 스키마 (안 보이면 빈 값)
    QComboBox *schemaCombo() const { return m_schemaCombo; }

    QString nameText() const;
    QString stateText() const;

signals:
    void schemaChosen(const QString &schema); // 목록에서 다른 스키마를 골랐다

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    void refreshSchemaItems();
    static QString target(const ConnectionProfile &profile);

    std::optional<ConnectionProfile> m_profile;
    State m_state = State::NotConnected;
    bool m_pending = false;
    QString m_serverInfo;
    QString m_schema;
    QStringList m_schemas;

    QLabel *m_dot = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_schemaCaption = nullptr;
    QComboBox *m_schemaCombo = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_environment = nullptr;
    QLabel *m_readOnly = nullptr;
    QLabel *m_pendingBadge = nullptr;
    QLabel *m_stateLabel = nullptr;
};
