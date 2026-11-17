#include "ResultModel.h"

#include <QApplication>
#include <QCborMap>
#include <QFont>
#include <QLocale>
#include <QPalette>

namespace {

constexpr int kDisplayLimit = 500;

QString fullText(const QCborValue &value)
{
    switch (value.type()) {
    case QCborValue::Null:
    case QCborValue::Undefined:
        return {};
    case QCborValue::True:
        return QStringLiteral("true");
    case QCborValue::False:
        return QStringLiteral("false");
    case QCborValue::Integer:
        return QString::number(value.toInteger());
    case QCborValue::Double:
        return QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
    case QCborValue::ByteArray:
        return QStringLiteral("0x") + QString::fromLatin1(value.toByteArray().toHex());
    default:
        return value.toString();
    }
}

} // namespace

ResultModel::ResultModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void ResultModel::reset(const QCborArray &columns)
{
    beginResetModel();
    m_columns.clear();
    for (const QCborValue &value : columns) {
        const QCborMap column = value.toMap();
        m_columns.append({column.value(QStringLiteral("name")).toString(),
                          column.value(QStringLiteral("typeName")).toString(),
                          column.value(QStringLiteral("numeric")).toBool()});
    }
    m_rows.clear();
    m_hasMore = false;
    m_fetchRequested = false;
    endResetModel();
}

void ResultModel::appendRows(const QCborArray &rows, bool hasMore)
{
    if (!rows.isEmpty()) {
        const int first = int(m_rows.size());
        beginInsertRows({}, first, first + int(rows.size()) - 1);
        for (const QCborValue &row : rows)
            m_rows.append(row.toArray());
        endInsertRows();
    }
    m_hasMore = hasMore;
    m_fetchRequested = false;
}

void ResultModel::clear()
{
    reset({});
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_columns.size());
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size() || index.column() >= m_columns.size())
        return {};

    const QCborValue value = m_rows[index.row()].at(index.column());
    const bool isNull = value.isNull() || value.isUndefined();

    switch (role) {
    case Qt::DisplayRole: {
        if (isNull)
            return QStringLiteral("<null>");
        if (value.isByteArray()) {
            const QByteArray bytes = value.toByteArray();
            const QString hex = QString::fromLatin1(bytes.left(32).toHex());
            return QStringLiteral("0x") + hex + (bytes.size() > 32 ? QStringLiteral("...") : QString());
        }
        QString text = fullText(value);
        text.replace(u'\n', u' ').replace(u'\r', u' ').replace(u'\t', u' ');
        if (text.size() > kDisplayLimit)
            text = text.left(kDisplayLimit) + QStringLiteral("...");
        return text;
    }
    case Qt::ToolTipRole: {
        if (isNull)
            return {};
        const QString text = fullText(value);
        return text.size() > 80 ? text.left(4000) : QVariant();
    }
    case Qt::ForegroundRole:
        if (isNull)
            return QApplication::palette().color(QPalette::PlaceholderText);
        return {};
    case Qt::FontRole:
        if (isNull) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        return {};
    case Qt::TextAlignmentRole:
        if (m_columns[index.column()].numeric)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    default:
        return {};
    }
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Vertical) {
        if (role == Qt::DisplayRole)
            return section + 1;
        return {};
    }
    if (section < 0 || section >= m_columns.size())
        return {};
    if (role == Qt::DisplayRole)
        return m_columns[section].name;
    if (role == Qt::ToolTipRole)
        return m_columns[section].typeName;
    return {};
}

bool ResultModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && m_hasMore && !m_fetchRequested;
}

void ResultModel::fetchMore(const QModelIndex &parent)
{
    if (!canFetchMore(parent))
        return;
    m_fetchRequested = true;
    emit moreRowsRequested();
}

QString ResultModel::copyText(int row, int column) const
{
    if (row < 0 || row >= m_rows.size() || column < 0 || column >= m_columns.size())
        return {};
    return fullText(m_rows[row].at(column));
}

QString ResultModel::columnName(int column) const
{
    return column >= 0 && column < m_columns.size() ? m_columns[column].name : QString();
}
