#pragma once

#include "ConnectionProfile.h"

#include <QString>
#include <QStringList>

// DBMS별 SQL 스크립트 폴더 (CLI의 @파일명이 먼저 찾는 곳). GUI와 dbaro-cli가 같은 설정을 쓴다.
namespace ScriptFolders {

QString folder(DbType type); // 없으면 빈 값
void setFolder(DbType type, const QString &path);

// 실행할 파일 찾기: 절대 경로면 그대로, 아니면 folders를 차례로 본다. 확장자가 없으면 .sql도 붙여 본다 (SQL*Plus처럼).
QString resolve(const QString &name, const QStringList &folders);

// 폴더 아래의 .sql 파일 (하위 폴더 포함, 폴더 기준 상대 경로, 이름순)
QStringList list(const QString &folder, int limit = 500);

} // namespace ScriptFolders
