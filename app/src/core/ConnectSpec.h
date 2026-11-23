#pragma once

#include "ConnectionProfile.h"

#include <QCborMap>
#include <QString>
#include <QStringList>

// 엔진에 보내는 연결 요청 (connect/test).
struct ConnectSpec
{
    QString type; // 엔진 드라이버 종류: oracle, tibero, mariadb, mysql, postgres
    QString url;
    QString user;
    QString password;
    QString driverPath;  // 비우면 엔진이 드라이버를 내려받는다 (Tibero는 필수)
    QString driverClass; // 비우면 종류별 기본 드라이버 클래스
    bool autoCommit = false;
    bool readOnly = false;
    int connectTimeoutSec = 10;
    QStringList initSql; // 접속 직후 실행할 문장들
    QString schema;      // 접속 직후 바꿀 스키마 (쿼리 탭에서 고른 것). 비우면 접속 설정의 기본값

    QCborMap toCbor() const;
    static ConnectSpec fromProfile(const ConnectionProfile &profile, const QString &password, bool autoCommit);
};
