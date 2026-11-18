#include "MetadataService.h"

#include "ipc/EngineClient.h"

MetadataService::MetadataService(EngineClient *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    if (engine) {
        connect(engine, &EngineClient::engineStopped, this, [this] {
            // 엔진과 함께 세션도 사라졌다. 캐시는 그대로 두고 다음 요청 때 다시 접속한다.
            ++m_generation;
            const QStringList ids = m_sessions.keys();
            m_sessions.clear();
            m_waiting.clear();
            m_inflight.clear();
            for (const QString &id : ids)
                emit connectionChanged(id, false);
        });
    }
}

MetadataService::~MetadataService()
{
    disconnectAll();
}

void MetadataService::setSpecProvider(SpecProvider provider)
{
    m_specProvider = std::move(provider);
}

void MetadataService::setProfiles(const QList<ConnectionProfile> &profiles)
{
    disconnectAll();
    m_profiles = profiles;
    m_cache.clear();
}

const ConnectionProfile *MetadataService::profile(const QString &profileId) const
{
    for (const ConnectionProfile &profile : m_profiles) {
        if (profile.id == profileId)
            return &profile;
    }
    return nullptr;
}

QString MetadataService::cacheKey(const QString &profileId, const QString &method, const QCborMap &params)
{
    QStringList parts{profileId, method};
    QStringList keys;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        keys << it.key().toString();
    keys.sort();
    for (const QString &key : std::as_const(keys))
        parts << key + u'=' + params.value(key).toVariant().toString();
    return parts.join(QChar(0x1f));
}

std::optional<QCborMap> MetadataService::cached(const QString &profileId, const QString &method,
                                                const QCborMap &params) const
{
    const auto it = m_cache.constFind(cacheKey(profileId, method, params));
    if (it == m_cache.constEnd())
        return std::nullopt;
    return it.value();
}

bool MetadataService::isLoading(const QString &profileId, const QString &method, const QCborMap &params) const
{
    return m_inflight.contains(cacheKey(profileId, method, params));
}

void MetadataService::invalidate(const QString &profileId)
{
    const QString prefix = profileId + QChar(0x1f);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key().startsWith(prefix))
            it = m_cache.erase(it);
        else
            ++it;
    }
}

void MetadataService::request(const QString &profileId, const QString &method, const QCborMap &params,
                              Callback callback, bool interactive, bool refresh)
{
    const QString key = cacheKey(profileId, method, params);
    if (!refresh) {
        const auto hit = m_cache.constFind(key);
        if (hit != m_cache.constEnd()) {
            callback(hit.value(), {});
            return;
        }
    }
    if (m_inflight.contains(key)) {
        m_inflight[key].append(callback);
        return;
    }
    m_inflight.insert(key, {callback});

    const int generation = m_generation;
    QPointer<MetadataService> self(this);
    auto finish = [self, key, generation, profileId](const QCborMap &result, const QString &error) {
        if (!self || generation != self->m_generation)
            return;
        const QList<Callback> callbacks = self->m_inflight.take(key);
        if (error.isEmpty()) {
            self->m_cache.insert(key, result);
            emit self->loaded(profileId);
        }
        for (const Callback &waiting : callbacks)
            waiting(result, error);
    };

    withSession(profileId, interactive, [self, method, params, finish](const QString &sessionId, const QString &error) {
        if (!self)
            return;
        if (!error.isEmpty() || !self->m_engine) {
            finish({}, error.isEmpty() ? tr("The engine is not available.") : error);
            return;
        }
        QCborMap withSessionId = params;
        withSessionId.insert(QStringLiteral("sessionId"), sessionId);
        self->m_engine->request(method, withSessionId, [finish](const QCborMap &result, const EngineError &engineError) {
            finish(result, engineError.message);
        });
    });
}

void MetadataService::withSession(const QString &profileId, bool interactive, SessionCallback callback)
{
    if (m_sessions.contains(profileId)) {
        callback(m_sessions.value(profileId), {});
        return;
    }
    if (m_waiting.contains(profileId)) {
        m_waiting[profileId].append(callback);
        return;
    }

    const ConnectionProfile *connection = profile(profileId);
    if (!connection || !m_engine) {
        callback({}, tr("The connection is not available."));
        return;
    }
    ConnectSpec spec;
    if (!m_specProvider || !m_specProvider(*connection, &spec, interactive)) {
        callback({}, interactive ? tr("Connection cancelled.") : tr("Not connected yet."));
        return;
    }
    spec.autoCommit = true; // 메타데이터는 조회만 한다

    m_waiting.insert(profileId, {callback});
    const int generation = m_generation;
    QPointer<MetadataService> self(this);
    m_engine->request(QStringLiteral("connect"), spec.toCbor(),
                      [self, profileId, generation](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          const QString sessionId = result.value(QStringLiteral("sessionId")).toString();
                          if (generation != self->m_generation) {
                              // 그 사이에 연결 목록이 바뀌었다: 늦게 열린 연결은 바로 닫는다.
                              if (!sessionId.isEmpty() && self->m_engine) {
                                  QCborMap params;
                                  params.insert(QStringLiteral("sessionId"), sessionId);
                                  self->m_engine->request(QStringLiteral("disconnect"), params);
                              }
                              return;
                          }
                          const QList<SessionCallback> callbacks = self->m_waiting.take(profileId);
                          if (error.isError()) {
                              for (const SessionCallback &waiting : callbacks)
                                  waiting({}, tr("Connection failed: %1").arg(error.message));
                              return;
                          }
                          self->m_sessions.insert(profileId, sessionId);
                          emit self->connectionChanged(profileId, true);
                          for (const SessionCallback &waiting : callbacks)
                              waiting(sessionId, {});
                      });
}

void MetadataService::disconnectProfile(const QString &profileId)
{
    const QString sessionId = m_sessions.take(profileId);
    if (!sessionId.isEmpty() && m_engine) {
        QCborMap params;
        params.insert(QStringLiteral("sessionId"), sessionId);
        m_engine->request(QStringLiteral("disconnect"), params);
    }
    invalidate(profileId);
    if (!sessionId.isEmpty())
        emit connectionChanged(profileId, false);
}

void MetadataService::disconnectAll()
{
    ++m_generation;
    if (m_engine) {
        for (const QString &sessionId : std::as_const(m_sessions)) {
            QCborMap params;
            params.insert(QStringLiteral("sessionId"), sessionId);
            m_engine->request(QStringLiteral("disconnect"), params);
        }
    }
    const QStringList ids = m_sessions.keys();
    m_sessions.clear();
    m_waiting.clear();
    m_inflight.clear();
    for (const QString &id : ids)
        emit connectionChanged(id, false);
}
