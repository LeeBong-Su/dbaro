#include "ReverseDialog.h"

#include "Icons.h"
#include "MetadataService.h"

#include <QCborArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kTypeRole = Qt::UserRole + 1;

bool isView(const QString &type)
{
    return type.contains(QLatin1String("VIEW"), Qt::CaseInsensitive);
}

bool isTable(const QString &type)
{
    return type.contains(QLatin1String("TABLE"), Qt::CaseInsensitive) && !type.contains(QLatin1String("TEMPORARY"), Qt::CaseInsensitive)
           && type.compare(QLatin1String("SYSTEM TABLE"), Qt::CaseInsensitive) != 0;
}
} // namespace

ReverseDialog::ReverseDialog(MetadataService *metadata, const QString &metadataId, const QString &schema, QWidget *parent)
    : QDialog(parent)
    , m_metadata(metadata)
    , m_metadataId(metadataId)
{
    setWindowTitle(tr("Reverse Engineer"));
    resize(520, 600);

    m_schema = new QComboBox(this);
    m_schema->setObjectName(QStringLiteral("reverseSchema"));
    m_schema->setMinimumContentsLength(24);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter tables"));
    m_filter->setClearButtonEnabled(true);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("reverseTables"));
    m_list->setUniformItemSizes(true);
    m_views = new QCheckBox(tr("Include views"), this);
    m_views->setChecked(true);
    m_count = new QLabel(this);

    auto *all = new QPushButton(tr("Select All"), this);
    auto *none = new QPushButton(tr("Select None"), this);
    connect(all, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden())
                m_list->item(i)->setCheckState(Qt::Checked);
        }
    });
    connect(none, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden())
                m_list->item(i)->setCheckState(Qt::Unchecked);
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(tr("Reverse Engineer"));
    m_ok->setEnabled(false); // 목록을 읽고 하나라도 고르면
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("Schema"), m_schema);
    auto *selection = new QHBoxLayout;
    selection->addWidget(all);
    selection->addWidget(none);
    selection->addStretch();
    selection->addWidget(m_count);

    auto *note = new QLabel(tr("Reads the tables, columns, keys, foreign keys and indexes, then draws the ERD. "
                               "The DDL script is made when you open its page."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    layout->addLayout(selection);
    layout->addWidget(m_views);
    layout->addWidget(note);
    layout->addWidget(buttons);

    connect(m_schema, &QComboBox::currentTextChanged, this, &ReverseDialog::loadObjects);
    connect(m_filter, &QLineEdit::textChanged, this, &ReverseDialog::applyFilter);
    connect(m_views, &QCheckBox::toggled, this, &ReverseDialog::applyFilter);
    connect(m_list, &QListWidget::itemChanged, this, &ReverseDialog::updateCount);
    loadSchemas(schema);
}

QString ReverseDialog::schema() const
{
    return m_schema->currentText();
}

bool ReverseDialog::includeViews() const
{
    return m_views->isChecked();
}

QStringList ReverseDialog::tables() const
{
    QStringList checked;
    int candidates = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem *item = m_list->item(i);
        if (!m_views->isChecked() && isView(item->data(kTypeRole).toString()))
            continue;
        ++candidates;
        if (item->checkState() == Qt::Checked)
            checked << item->text();
    }
    return checked.size() == candidates ? QStringList() : checked;
}

void ReverseDialog::loadSchemas(const QString &preferred)
{
    if (!m_metadata)
        return;
    QPointer<ReverseDialog> self(this);
    m_metadata->request(m_metadataId, QStringLiteral("metaSchemas"), {}, [self, preferred](const QCborMap &result, const QString &error) {
        if (!self)
            return;
        QStringList names;
        for (const QCborValue &name : result.value(QStringLiteral("names")).toArray())
            names << name.toString();
        const QString current = preferred.isEmpty() ? result.value(QStringLiteral("current")).toString() : preferred;
        if (!current.isEmpty() && !names.contains(current))
            names.prepend(current);
        self->m_schema->blockSignals(true);
        self->m_schema->clear();
        self->m_schema->addItems(names);
        self->m_schema->setCurrentText(current);
        self->m_schema->blockSignals(false);
        if (!error.isEmpty())
            self->m_count->setText(error.section(u'\n', 0, 0));
        self->loadObjects();
    });
}

void ReverseDialog::loadObjects()
{
    const QString schema = m_schema->currentText();
    m_list->clear();
    if (!m_metadata || schema.isEmpty())
        return;
    m_count->setText(tr("Loading..."));
    const int generation = ++m_generation;
    QCborMap params;
    params.insert(QStringLiteral("schema"), schema);
    QPointer<ReverseDialog> self(this);
    m_metadata->request(m_metadataId, QStringLiteral("metaObjects"), params,
                        [self, generation](const QCborMap &result, const QString &error) {
                            if (!self || generation != self->m_generation)
                                return;
                            if (!error.isEmpty()) {
                                self->m_count->setText(error.section(u'\n', 0, 0));
                                return;
                            }
                            self->m_list->blockSignals(true);
                            for (const QCborValue &value : result.value(QStringLiteral("objects")).toArray()) {
                                const QCborMap object = value.toMap();
                                const QString type = object.value(QStringLiteral("type")).toString();
                                if (!isTable(type) && !isView(type))
                                    continue;
                                auto *item = new QListWidgetItem(Icons::get(isView(type) ? QStringLiteral("view") : QStringLiteral("table")),
                                                                 object.value(QStringLiteral("name")).toString(), self->m_list);
                                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                                item->setCheckState(Qt::Checked);
                                item->setData(kTypeRole, type);
                                const QString remarks = object.value(QStringLiteral("remarks")).toString();
                                item->setToolTip(remarks.isEmpty() ? type : type + u'\n' + remarks);
                            }
                            self->m_list->sortItems();
                            self->m_list->blockSignals(false);
                            self->applyFilter();
                        });
}

void ReverseDialog::applyFilter()
{
    const QString filter = m_filter->text().trimmed();
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const bool view = isView(item->data(kTypeRole).toString());
        item->setHidden((view && !m_views->isChecked()) || (!filter.isEmpty() && !item->text().contains(filter, Qt::CaseInsensitive)));
    }
    updateCount();
}

void ReverseDialog::updateCount()
{
    int checked = 0;
    int total = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem *item = m_list->item(i);
        if (!m_views->isChecked() && isView(item->data(kTypeRole).toString()))
            continue;
        ++total;
        checked += item->checkState() == Qt::Checked ? 1 : 0;
    }
    m_count->setText(tr("%1 of %2 selected").arg(checked).arg(total));
    m_ok->setEnabled(checked > 0);
}
