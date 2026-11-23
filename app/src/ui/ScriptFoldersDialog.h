#pragma once

#include "core/ConnectionProfile.h"

#include <QDialog>
#include <QHash>

class QLineEdit;

// DBMS별 SQL 스크립트 폴더 정하기 (CLI 탭과 dbaro-cli의 @파일명이 먼저 찾는 곳)
class ScriptFoldersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ScriptFoldersDialog(QWidget *parent = nullptr, DbType focus = DbType::Oracle);

    QLineEdit *pathEdit(DbType type) const { return m_paths.value(int(type)); }
    void accept() override;

private:
    QHash<int, QLineEdit *> m_paths;
};
