#pragma once

#include "core/SqlBinds.h"

#include <QDialog>
#include <QHash>

class QComboBox;
class QLineEdit;

// 실행 전에 바인드 변수 값을 묻는다. 지난번에 넣은 값이 미리 채워진다.
// 형식: Auto(숫자는 그대로, 나머지는 문자열), Text('...'), Raw SQL(그대로, 예: SYSDATE), NULL.
// SQL*Plus 치환 변수(&name)는 늘 입력한 그대로 넣으므로 형식을 고르지 않는다.
class BindDialog : public QDialog
{
    Q_OBJECT

public:
    BindDialog(const QList<BindVariable> &variables, const QHash<QString, SqlBinds::Value> &remembered,
               QWidget *parent = nullptr);

    QList<SqlBinds::Value> values() const;
    void setValue(int index, const QString &text, SqlBinds::Value::Type type = SqlBinds::Value::Type::Auto);

private:
    QList<BindVariable> m_variables;
    QList<QLineEdit *> m_fields;
    QList<QComboBox *> m_types;
};
