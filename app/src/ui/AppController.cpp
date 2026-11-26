#include "AppController.h"

#include "ConnectionDialog.h"
#include "LauncherWindow.h"
#include "MainWindow.h"
#include "MetadataService.h"
#include "WindowsIntegration.h"
#include "core/ConnectSpec.h"
#include "core/SecretStore.h"
#include "ipc/EngineClient.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>

namespace {
const auto kRecentKey = QStringLiteral("connections/recent");
const auto kReopenKey = QStringLiteral("startup/reopenWindows");
const auto kOpenWindowsKey = QStringLiteral("startup/openWindows");
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
{
    m_engine = new EngineClient(this);
    m_metadata = new MetadataService(m_engine, this);
    m_metadata->setSpecProvider([this](const ConnectionProfile &profile, ConnectSpec *spec, bool interactive) {
        QString password;
        if (!passwordFor(profile, &password, interactive, QApplication::activeWindow()))
            return false;
        *spec = connectSpec(profile, password, true);
        return true;
    });
    loadConnections();
}

AppController::~AppController()
{
    // 창(탭의 DB 세션)을 엔진보다 먼저 정리한다.
    m_quitting = true;
    const auto windows = m_windows.values();
    m_windows.clear();
    for (const QPointer<MainWindow> &window : windows)
        delete window.data();
    delete m_launcher.data();
}

void AppController::loadConnections()
{
    QString error;
    m_store.load(&error);
    m_metadata->setProfiles(m_store.profiles());
}

void AppController::start(const QStringList &arguments)
{
    updateJumpList();
    handleArguments(arguments);
    if (!m_windows.isEmpty() || (m_launcher && m_launcher->isVisible()))
        return;
    if (reopenAtStartup()) {
        for (const QString &id : QSettings().value(kOpenWindowsKey).toStringList()) {
            if (profile(id))
                openConnection(id);
        }
    }
    if (m_windows.isEmpty())
        showLauncher();
}

void AppController::handleArguments(const QStringList &arguments)
{
    bool handled = false;
    // --open-sql <파일> [--title <제목>]: --connection으로 연 창의 새 쿼리 탭에 SQL을 넣는다 (dbaro-cli mcp의 open_in_dbaro)
    QString sqlFile, sqlTitle;
    for (int i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == u"--open-sql")
            sqlFile = arguments[i + 1];
        else if (arguments[i] == u"--title")
            sqlTitle = arguments[i + 1];
    }
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments[i];
        if ((argument == u"--open-sql" || argument == u"--title") && i + 1 < arguments.size()) {
            ++i;
        } else if (argument == u"--connection" && i + 1 < arguments.size()) {
            const QString id = arguments[++i];
            if (profile(id)) {
                MainWindow *window = openConnection(id);
                if (window && !sqlFile.isEmpty())
                    openSqlFile(window, sqlFile, sqlTitle);
            } else {
                showLauncher();
                QMessageBox::information(m_launcher, tr("DBaro"), tr("That connection no longer exists."));
            }
            handled = true;
        } else if (argument == u"--launcher") {
            showLauncher();
            handled = true;
        } else if (argument == u"--new-connection") {
            showLauncher();
            newConnection(m_launcher);
            handled = true;
        }
    }
    // 인자 없이 다시 실행했으면(시작 메뉴 등) 지금 앞에 둘 창을 보여 준다.
    if (!handled && (!m_windows.isEmpty() || (m_launcher && m_launcher->isVisible()))) {
        if (m_launcher && m_launcher->isVisible())
            WindowsIntegration::bringToFront(m_launcher);
        else
            showLauncher();
    }
}

void AppController::openSqlFile(MainWindow *window, const QString &path, const QString &title)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(window, tr("DBaro"), tr("Could not open %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
        return;
    }
    const QString sql = QString::fromUtf8(file.readAll());
    file.close();
    // dbaro-cli mcp가 넘긴 임시 파일만 지운다
    const QFileInfo info(path);
    if (info.fileName().startsWith(QLatin1String("dbaro-sql-")) && info.suffix() == u"sql"
        && QDir(info.absolutePath()) == QDir(QDir::tempPath()))
        QFile::remove(path);
    window->openSqlTab(sql.trimmed() + u'\n', title.isEmpty() ? tr("Claude") : title);
}

void AppController::show(QWidget *window)
{
    if (m_beforeShow)
        m_beforeShow(window);
    WindowsIntegration::bringToFront(window);
}

MainWindow *AppController::windowFor(const QString &connectionId) const
{
    return m_windows.value(connectionId).data();
}

QList<MainWindow *> AppController::windows() const
{
    QList<MainWindow *> list;
    for (const QPointer<MainWindow> &window : m_windows) {
        if (window)
            list.append(window.data());
    }
    return list;
}

MainWindow *AppController::openConnection(const QString &connectionId)
{
    if (!profile(connectionId))
        return nullptr;
    if (MainWindow *existing = windowFor(connectionId)) {
        touchRecent(connectionId);
        show(existing);
        return existing;
    }
    auto *window = new MainWindow(this, connectionId);
    window->setAttribute(Qt::WA_DeleteOnClose);
    // 새 창은 열려 있는 창과 겹치지 않게 조금씩 비켜 놓는다 (이 연결의 크기·위치를 기억하지 않았을 때).
    if (!window->hasSavedGeometry() && !m_windows.isEmpty())
        window->move(window->pos() + QPoint(32, 32) * int(m_windows.size()));
    m_windows.insert(connectionId, window);
    connect(window, &MainWindow::windowClosed, this, [this, connectionId] { onWindowClosed(connectionId); });
    touchRecent(connectionId);
    show(window);
    if (m_launcher)
        m_launcher->hide();
    rememberOpenWindows();
    emit windowsChanged();
    return window;
}

void AppController::onWindowClosed(const QString &connectionId)
{
    m_windows.remove(connectionId);
    if (m_quitting)
        return;
    rememberOpenWindows();
    emit windowsChanged();
    if (m_windows.isEmpty())
        showLauncher(); // 마지막 연결 창을 닫으면 선택 창으로 돌아간다
}

LauncherWindow *AppController::launcher()
{
    if (!m_launcher) {
        m_launcher = new LauncherWindow(this);
        connect(m_launcher, &LauncherWindow::closedByUser, this, [this] {
            if (m_windows.isEmpty())
                quitAll(); // 연결 창이 없을 때 선택 창을 닫으면 끝난다
        });
    }
    return m_launcher;
}

void AppController::showLauncher()
{
    LauncherWindow *window = launcher();
    window->refresh();
    show(window);
}

void AppController::newConnection(QWidget *parent)
{
    QString selected;
    if (editConnections(parent, true, {}, &selected) && profile(selected))
        openConnection(selected);
}

bool AppController::editConnections(QWidget *parent, bool startNew, const QString &selectId, QString *selectedId)
{
    ConnectionDialog dialog(m_store.profiles(), parent);
    dialog.setEngine(m_engine);
    if (startNew)
        dialog.startNewConnection();
    else if (!selectId.isEmpty())
        dialog.selectConnection(selectId);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    m_store.setProfiles(dialog.profiles());
    QString error;
    if (!m_store.save(&error))
        QMessageBox::warning(parent, tr("Connections"), tr("Could not save the connections.\n%1").arg(error));

    // 비밀번호는 연결 파일이 아니라 OS 보안 저장소에 반영한다.
    for (const QString &id : dialog.removedIds())
        SecretStore::removePassword(id);
    const QHash<QString, QString> pending = dialog.pendingPasswords();
    QStringList failed;
    for (const ConnectionProfile &profile : m_store.profiles()) {
        m_sessionPasswords.remove(profile.id);
        if (!profile.savePassword) {
            SecretStore::removePassword(profile.id);
            continue;
        }
        const auto it = pending.constFind(profile.id);
        if (it != pending.constEnd() && !SecretStore::savePassword(profile.id, profile.user, it.value(), &error))
            failed << profile.name;
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(parent, tr("Connections"),
                             tr("Could not save the password for: %1\n%2").arg(failed.join(QStringLiteral(", ")), error));
    }
    if (selectedId)
        *selectedId = dialog.selectedId();
    applyConnections(dialog.removedIds());
    return true;
}

void AppController::applyConnections(const QStringList &removedIds)
{
    m_metadata->setProfiles(m_store.profiles());
    // 지운 연결의 창은 닫고 그 탭 기록도 지운다.
    for (const QString &id : removedIds) {
        if (MainWindow *window = windowFor(id))
            window->close();
        m_workspace.remove(id);
    }
    emit connectionsChanged();
    updateJumpList();
}

void AppController::quitAll()
{
    rememberOpenWindows(); // 다음에 다시 열 창
    m_quitting = true;
    for (MainWindow *window : windows()) {
        if (!window->close()) {
            m_quitting = false; // 커밋하지 않은 변경 때문에 취소했다
            return;
        }
    }
    if (m_launcher)
        m_launcher->hide();
    QApplication::quit();
}

QStringList AppController::recentConnections() const
{
    return QSettings().value(kRecentKey).toStringList();
}

void AppController::touchRecent(const QString &connectionId)
{
    QStringList recent = recentConnections();
    recent.removeAll(connectionId);
    recent.prepend(connectionId);
    while (recent.size() > 50)
        recent.removeLast();
    QSettings().setValue(kRecentKey, recent);
    updateJumpList();
}

void AppController::rememberOpenWindows()
{
    if (m_quitting)
        return;
    QStringList open;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (it.value())
            open << it.key();
    }
    QSettings().setValue(kOpenWindowsKey, open);
}

bool AppController::reopenAtStartup() const
{
    return QSettings().value(kReopenKey, false).toBool();
}

void AppController::setReopenAtStartup(bool on)
{
    QSettings().setValue(kReopenKey, on);
}

void AppController::updateJumpList()
{
    // 최근에 연 연결부터, 나머지는 이름 순
    QList<const ConnectionProfile *> ordered;
    for (const QString &id : recentConnections()) {
        if (const ConnectionProfile *p = profile(id))
            ordered.append(p);
    }
    QList<ConnectionProfile> rest = m_store.profiles();
    std::sort(rest.begin(), rest.end(), [](const ConnectionProfile &a, const ConnectionProfile &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    for (const ConnectionProfile &p : rest) {
        if (!ordered.contains(profile(p.id)))
            ordered.append(profile(p.id));
    }

    QList<WindowsIntegration::JumpItem> items;
    for (const ConnectionProfile *p : std::as_const(ordered)) {
        QString title = p->name;
        if (p->environment == Environment::Production)
            title += tr(" (PRODUCTION)");
        QString address = p->host.isEmpty() ? QString() : QStringLiteral(" · %1:%2").arg(p->host).arg(p->port);
        items.append({title, QStringLiteral("--connection %1").arg(p->id),
                      QStringLiteral("%1%2 · %3").arg(Db::typeName(p->type), address, p->user)});
    }
    const QList<WindowsIntegration::JumpItem> tasks{
        {tr("Connections..."), QStringLiteral("--launcher"), tr("Choose a connection to open")},
        {tr("New Connection..."), QStringLiteral("--new-connection"), tr("Add a database connection")},
    };
    WindowsIntegration::updateJumpList(tr("Connections"), items, tasks);
}

bool AppController::passwordFor(const ConnectionProfile &profile, QString *password, bool interactive, QWidget *parent)
{
    if (profile.savePassword) {
        if (const std::optional<QString> saved = SecretStore::loadPassword(profile.id)) {
            *password = *saved;
            return true;
        }
    }
    if (m_sessionPasswords.contains(profile.id)) {
        *password = m_sessionPasswords.value(profile.id);
        return true;
    }
    if (!interactive)
        return false; // 자동완성처럼 사용자가 요청하지 않은 조회는 비밀번호를 묻지 않는다

    bool ok = false;
    const QString entered = QInputDialog::getText(parent, tr("Password"),
                                                  tr("Password for %1 on \"%2\":").arg(profile.user, profile.name),
                                                  QLineEdit::Password, QString(), &ok);
    if (!ok)
        return false;
    m_sessionPasswords.insert(profile.id, entered);
    *password = entered;
    return true;
}

ConnectSpec AppController::connectSpec(const ConnectionProfile &profile, const QString &password, bool autoCommit) const
{
    ConnectSpec spec = ConnectSpec::fromProfile(profile, password, autoCommit);
    if (m_specOverride)
        m_specOverride(profile, &spec);
    return spec;
}

void AppController::forgetPassword(const QString &connectionId)
{
    m_sessionPasswords.remove(connectionId);
}
