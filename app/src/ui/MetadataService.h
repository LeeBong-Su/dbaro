#pragma once

#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>

class EngineClient;

// 연결별 메타데이터(스키마, 객체, 컬럼) 조회와 캐시.
// Database Explorer와 SQL 자동완성이 함께 쓴다. 연결마다 메타데이터 전용 세션(자동 커밋) 하나만 연다.
// 같은 요청은 캐시에서 바로 답하고, 진행 중인 같은 요청은 하나로 묶는다.
class MetadataService : public QObject
{
    Q_OBJECT

public:
    // 접속 정보 만들기. interactive가 false면 비밀번호를 묻지 않는다 (자동완성처럼 사용자가 요청하지 않은 조회).
    using SpecProvider = std::function<bool(const ConnectionProfile &profile, ConnectSpec *spec, bool interactive)>;
    using Callback = std::function<void(const QCborMap &result, const QString &error)>;

    explicit MetadataService(EngineClient *engine, QObject *parent = nullptr);
    ~MetadataService() override;

    void setSpecProvider(SpecProvider provider);
    void setProfiles(const QList<ConnectionProfile> &profiles); // 모든 세션을 닫고 캐시를 비운다
    const ConnectionProfile *profile(const QString &profileId) const;

    // method: metaSchemas, metaObjects(schema), metaColumns(schema, table)
    // refresh면 캐시를 무시하고 다시 가져온다.
    void request(const QString &profileId, const QString &method, const QCborMap &params, Callback callback,
                 bool interactive = true, bool refresh = false);
    std::optional<QCborMap> cached(const QString &profileId, const QString &method, const QCborMap &params) const;
    bool isLoading(const QString &profileId, const QString &method, const QCborMap &params) const;

    void invalidate(const QString &profileId); // DDL 뒤처럼 객체 목록이 바뀌었을 때
    bool isConnected(const QString &profileId) const { return m_sessions.contains(profileId); }
    void disconnectProfile(const QString &profileId);

signals:
    void connectionChanged(const QString &profileId, bool connected);
    void loaded(const QString &profileId); // 새 메타데이터가 캐시에 들어옴

private:
    using SessionCallback = std::function<void(const QString &sessionId, const QString &error)>;

    static QString cacheKey(const QString &profileId, const QString &method, const QCborMap &params);
    void withSession(const QString &profileId, bool interactive, SessionCallback callback);
    void disconnectAll();

    QPointer<EngineClient> m_engine;
    SpecProvider m_specProvider;
    QList<ConnectionProfile> m_profiles;
    QHash<QString, QString> m_sessions;                 // 연결 id → 엔진 세션 id
    QHash<QString, QList<SessionCallback>> m_waiting;   // 접속 중인 연결 id → 기다리는 요청
    QHash<QString, QCborMap> m_cache;                   // cacheKey → 결과
    QHash<QString, QList<Callback>> m_inflight;         // cacheKey → 기다리는 요청
    int m_generation = 0; // 연결 목록을 새로 받을 때마다 증가 (늦게 온 응답 무시)
};
