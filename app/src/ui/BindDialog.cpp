#include "BindDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

BindDialog::BindDialog(const QList<BindVariable> &variables, const QHash<QString, SqlBinds::Value> &remembered,
                       QWidget *parent)
    : QDialog(parent)
    , m_variables(variables)
{
    setWindowTitle(tr("Bind Variables"));
    setObjectName(QStringLiteral("bindDialog"));

    auto *intro = new QLabel(tr("Enter the values to put in place of the variables."), this);
    intro->setWordWrap(true);

    auto *grid = new QWidget(this);
    auto *layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(8);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int i = 0; i < variables.size(); ++i) {
        const BindVariable &variable = variables[i];
        auto *label = new QLabel(variable.label(), grid);
        label->setFont(mono);
        auto *field = new QLineEdit(grid);
        field->setObjectName(QStringLiteral("bindValue%1").arg(i));
        field->setMinimumWidth(280);
        auto *type = new QComboBox(grid);
        type->setObjectName(QStringLiteral("bindType%1").arg(i));
        type->addItem(tr("Auto"), int(SqlBinds::Value::Type::Auto));
        type->addItem(tr("Text"), int(SqlBinds::Value::Type::Text));
        type->addItem(tr("Raw SQL"), int(SqlBinds::Value::Type::Raw));
        type->addItem(tr("NULL"), int(SqlBinds::Value::Type::Null));
        type->setToolTip(tr("Auto: numbers as they are, anything else as a string.\n"
                            "Text: always a quoted string.\nRaw SQL: as typed (e.g. SYSDATE).\nNULL: NULL."));
        if (variable.kind == BindVariable::Kind::Substitution) {
            type->setVisible(false); // SQL*Plus처럼 입력한 그대로 넣는다
            field->setToolTip(tr("Substitution variable: the text is inserted as typed (add quotes for strings)."));
        }
        connect(type, &QComboBox::currentIndexChanged, field, [field, type] {
            field->setEnabled(type->currentData().toInt() != int(SqlBinds::Value::Type::Null));
        });

        const auto it = remembered.constFind(variable.key());
        if (it != remembered.constEnd()) {
            field->setText(it->text);
            type->setCurrentIndex(type->findData(int(it->type)));
        }
        layout->addWidget(label, i, 0);
        layout->addWidget(field, i, 1);
        layout->addWidget(type, i, 2);
        m_fields.append(field);
        m_types.append(type);
    }

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(grid);
    scroll->setMinimumHeight(qMin(int(variables.size()), 8) * (m_fields.isEmpty() ? 28 : m_fields[0]->sizeHint().height() + 8) + 8);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Execute"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *main = new QVBoxLayout(this);
    main->addWidget(intro);
    main->addWidget(scroll, 1);
    main->addWidget(buttons);

    if (!m_fields.isEmpty()) {
        m_fields.first()->setFocus();
        m_fields.first()->selectAll();
    }
}

QList<SqlBinds::Value> BindDialog::values() const
{
    QList<SqlBinds::Value> values;
    for (int i = 0; i < m_fields.size(); ++i) {
        SqlBinds::Value value;
        value.text = m_fields[i]->text();
        value.type = SqlBinds::Value::Type(m_types[i]->currentData().toInt());
        values.append(value);
    }
    return values;
}

void BindDialog::setValue(int index, const QString &text, SqlBinds::Value::Type type)
{
    if (index < 0 || index >= m_fields.size())
        return;
    m_fields[index]->setText(text);
    m_types[index]->setCurrentIndex(m_types[index]->findData(int(type)));
}
