#pragma once

#include "core/ConnectSpec.h"
#include "core/ConnectionStore.h"
#include "core/QueryHistory.h"
#include "core/SqlBinds.h"
#include "core/WorkspaceStore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

class EngineClient;
class LauncherWindow;
class MainWindow;
class MetadataService;
class QWidget;

// 앱 전체. 연결마다 창 하나(MainWindow)를 열고, 모든 창이 엔진 프로세스·메타데이터·실행 기록·연결 목록을 함께 쓴다.
// - 시작하면 연결 선택 창(LauncherWindow). 연결을 고르면 그 연결의 창이 열리고 선택 창은 숨는다
// - 같은 연결을 다시 고르면 이미 열린 창을 앞으로 가져온다
// - 마지막 연결 창을 닫으면 선택 창으로 돌아가고, 선택 창까지 닫으면 끝난다
// - 작업 표시줄 점프 목록에 저장된 연결을 최근 쓴 순서로 둔다
class AppController : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    // 처음 실행: 인자(--connection <id>, --launcher, --new-connection)를 처리하고,
    // 연 창이 없으면 (설정에 따라 지난번 창을 다시 열거나) 연결 선택 창을 띄운다.
    void start(const QStringList &arguments);
    // 두 번째 실행에서 넘어온 인자. 인자가 없으면 연결 선택 창을 앞으로.
    void handleArguments(const QStringList &arguments);

    MainWindow *openConnection(const QString &connectionId);
    MainWindow *windowFor(const QString &connectionId) const;
    QList<MainWindow *> windows() const;
    LauncherWindow *launcher();
    void showLauncher();
    // 새 연결 만들기 → 만들었으면 그 창을 연다
    void newConnection(QWidget *parent);
    // 연결 관리 대화상자. 저장했으면 true (selectedId: 대화상자에서 마지막에 고른 연결)
    bool editConnections(QWidget *parent, bool startNew, const QString &selectId = {}, QString *selectedId = nullptr);
    // 모든 창을 닫고 끝낸다 (커밋하지 않은 변경은 창마다 묻는다)
    void quitAll();

    const ConnectionStore &store() const { return m_store; }
    const ConnectionProfile *profile(const QString &id) const { return m_store.find(id); }
    QStringList recentConnections() const; // 최근에 연 순서
    EngineClient *engine() const { return m_engine; }
    MetadataService *metadata() const { return m_metadata; }
    QueryHistory &history() { return m_history; }
    WorkspaceStore &workspaceStore() { return m_workspace; }
    QHash<QString, SqlBinds::Value> &bindValues() { return m_bindValues; }

    // 비밀번호: 저장한 것 → 앱이 켜져 있는 동안 입력한 것 → (interactive면) 묻기
    bool passwordFor(const ConnectionProfile &profile, QString *password, bool interactive, QWidget *parent = nullptr);
    // 쿼리 탭이 접속할 때 쓰는 요청. 테스트 도구는 setSpecOverride로 바꿀 수 있다 (내장 DB 등).
    ConnectSpec connectSpec(const ConnectionProfile &profile, const QString &password, bool autoCommit) const;
    void setSpecOverride(std::function<void(const ConnectionProfile &, ConnectSpec *)> override)
    {
        m_specOverride = std::move(override);
    }
    void forgetPassword(const QString &connectionId);

    // 연결 창을 열면(탭을 고르면) 바로 접속한다. 테스트 도구는 끌 수 있다.
    bool autoConnect() const { return m_autoConnect; }
    void setAutoConnect(bool on) { m_autoConnect = on; }

    bool reopenAtStartup() const;
    void setReopenAtStartup(bool on);

    // 창을 보이기 직전에 부른다 (테스트 도구가 화면에 띄우지 않게 할 때)
    void setBeforeShow(std::function<void(QWidget *)> hook) { m_beforeShow = std::move(hook); }

signals:
    void connectionsChanged();
    void windowsChanged();

private:
    void loadConnections();
    void applyConnections(const QStringList &removedIds);
    void onWindowClosed(const QString &connectionId);
    void touchRecent(const QString &connectionId);
    void rememberOpenWindows();
    void updateJumpList();
    void show(QWidget *window);

    ConnectionStore m_store;
    EngineClient *m_engine = nullptr;
    MetadataService *m_metadata = nullptr;
    QueryHistory m_history;
    WorkspaceStore m_workspace;
    QHash<QString, SqlBinds::Value> m_bindValues; // 바인드 변수마다 마지막에 넣은 값 (앱이 켜져 있는 동안)
    QHash<QString, QString> m_sessionPasswords;   // 저장하지 않은 비밀번호 (앱이 켜져 있는 동안)
    QHash<QString, QPointer<MainWindow>> m_windows;
    QPointer<LauncherWindow> m_launcher;
    std::function<void(QWidget *)> m_beforeShow;
    std::function<void(const ConnectionProfile &, ConnectSpec *)> m_specOverride;
    bool m_quitting = false;
    bool m_autoConnect = true;
};
