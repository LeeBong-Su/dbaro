#pragma once

#include "TextFile.h"

#include <QList>
#include <QString>

// 앱을 닫았다 열어도 쿼리 탭이 그대로 돌아오도록 남기는 탭 상태.
struct WorkspaceTab
{
    QString title;
    bool fixedTitle = false; // 연결 이름을 따르지 않는 제목 (객체 조회, DDL, 파일)
    QString connectionId;
    QString sql;             // 파일 탭이고 저장한 그대로면 비워 두고 파일에서 다시 읽는다
    int cursor = 0;
    QString filePath;
    bool modified = false;   // 파일 탭에서 저장하지 않은 변경이 있다
    TextFileFormat format;
    QString ddlKey;
};

struct Workspace
{
    QList<WorkspaceTab> tabs;
    int current = 0;
};

// 작업 공간 파일 (앱 데이터 폴더의 workspace.json). 쓰는 중에 앱이 꺼져도 깨지지 않게 임시 파일로 바꿔치기한다.
class WorkspaceStore
{
public:
    WorkspaceStore();
    explicit WorkspaceStore(const QString &filePath);

    QString filePath() const { return m_filePath; }
    bool load(Workspace *workspace, QString *error = nullptr) const; // 파일이 없으면 빈 작업 공간
    bool save(const Workspace &workspace, QString *error = nullptr) const;

private:
    QString m_filePath;
};
