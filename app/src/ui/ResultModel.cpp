#include "ResultModel.h"

#include "Theme.h"

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
    m_editing = false;
    m_editable.clear();
    m_original.clear();
    m_inserted.clear();
    m_deleted.clear();
    endResetModel();
    emit changesChanged();
}

QCborValue ResultModel::value(int row, int column) const
{
    if (row < 0 || row >= m_rows.size() || column < 0 || column >= m_columns.size())
        return {};
    return m_rows[row].at(column);
}

void ResultModel::setEditing(bool editing, const QList<bool> &editableColumns)
{
    m_editing = editing;
    m_editable = editableColumns;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, int(m_columns.size()) - 1));
}

bool ResultModel::isColumnEditable(int column) const
{
    return m_editing && column >= 0 && column < m_columns.size() && (column >= m_editable.size() || m_editable[column]);
}

Qt::ItemFlags ResultModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.isValid() && isColumnEditable(index.column()) && !m_deleted.contains(index.row()))
        flags |= Qt::ItemIsEditable;
    return flags;
}

bool ResultModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || !isColumnEditable(index.column()))
        return false;
    setCellValue(index.row(), index.column(), QCborValue(value.toString()));
    return true;
}

void ResultModel::setCellValue(int row, int column, const QCborValue &value)
{
    if (row < 0 || row >= m_rows.size() || column < 0 || column >= m_columns.size() || m_deleted.contains(row))
        return;
    if (m_rows[row].at(column) == value)
        return;
    if (!m_inserted.contains(row) && !m_original.contains(row))
        m_original.insert(row, m_rows[row]);
    QCborArray &cells = m_rows[row];
    while (cells.size() < m_columns.size())
        cells.append(QCborValue(QCborValue::Null));
    cells[column] = value;
    // 원래 값으로 되돌렸으면 고친 행에서 뺀다
    if (m_original.contains(row) && m_original.value(row) == m_rows[row])
        m_original.remove(row);
    emit dataChanged(index(row, 0), index(row, int(m_columns.size()) - 1));
    emit changesChanged();
}

int ResultModel::appendEmptyRow()
{
    const int row = int(m_rows.size());
    beginInsertRows({}, row, row);
    QCborArray cells;
    for (int c = 0; c < m_columns.size(); ++c)
        cells.append(QCborValue(QCborValue::Null));
    m_rows.append(cells);
    m_inserted.insert(row);
    endInsertRows();
    emit changesChanged();
    return row;
}

void ResultModel::removeRowAt(int row)
{
    beginRemoveRows({}, row, row);
    m_rows.removeAt(row);
    auto shiftSet = [row](QSet<int> &set) {
        QSet<int> shifted;
        for (int r : std::as_const(set)) {
            if (r != row)
                shifted.insert(r > row ? r - 1 : r);
        }
        set = shifted;
    };
    shiftSet(m_inserted);
    shiftSet(m_deleted);
    QHash<int, QCborArray> original;
    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it) {
        if (it.key() != row)
            original.insert(it.key() > row ? it.key() - 1 : it.key(), it.value());
    }
    m_original = original;
    endRemoveRows();
}

void ResultModel::markDeleted(const QList<int> &rows)
{
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int row : std::as_const(sorted)) {
        if (row < 0 || row >= m_rows.size())
            continue;
        if (m_inserted.contains(row)) {
            removeRowAt(row);
        } else if (m_deleted.contains(row)) {
            m_deleted.remove(row); // 다시 누르면 지우기 취소
            emit dataChanged(index(row, 0), index(row, int(m_columns.size()) - 1));
        } else {
            m_deleted.insert(row);
            emit dataChanged(index(row, 0), index(row, int(m_columns.size()) - 1));
        }
    }
    emit changesChanged();
}

void ResultModel::revertChanges()
{
    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it)
        m_rows[it.key()] = it.value();
    m_original.clear();
    m_deleted.clear();
    QList<int> inserted(m_inserted.begin(), m_inserted.end());
    std::sort(inserted.begin(), inserted.end(), std::greater<int>());
    for (int row : std::as_const(inserted))
        removeRowAt(row);
    m_inserted.clear();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, int(m_columns.size()) - 1));
    emit changesChanged();
}

void ResultModel::acceptChanges()
{
    QList<int> deleted(m_deleted.begin(), m_deleted.end());
    std::sort(deleted.begin(), deleted.end(), std::greater<int>());
    for (int row : std::as_const(deleted))
        removeRowAt(row);
    m_deleted.clear();
    m_original.clear();
    m_inserted.clear();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, int(m_columns.size()) - 1));
    emit changesChanged();
}

int ResultModel::pendingChangeCount() const
{
    QSet<int> rows = m_inserted;
    rows.unite(m_deleted);
    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it)
        rows.insert(it.key());
    return int(rows.size());
}

ResultModel::RowState ResultModel::rowState(int row) const
{
    if (m_deleted.contains(row))
        return RowState::Deleted;
    if (m_inserted.contains(row))
        return RowState::Inserted;
    if (m_original.contains(row))
        return RowState::Modified;
    return RowState::Clean;
}

bool ResultModel::isCellChanged(int row, int column) const
{
    const auto it = m_original.constFind(row);
    return it != m_original.constEnd() && it->at(column) != m_rows[row].at(column);
}

QList<RowChange> ResultModel::changes() const
{
    QList<RowChange> list;
    for (int row = 0; row < m_rows.size(); ++row) {
        RowChange change;
        if (m_deleted.contains(row)) {
            change.kind = RowChange::Kind::Delete;
            change.original = m_original.value(row, m_rows[row]);
        } else if (m_inserted.contains(row)) {
            change.kind = RowChange::Kind::Insert;
            change.current = m_rows[row];
        } else if (m_original.contains(row)) {
            change.kind = RowChange::Kind::Update;
            change.original = m_original.value(row);
            change.current = m_rows[row];
            for (int c = 0; c < m_columns.size(); ++c) {
                if (change.original.at(c) != change.current.at(c))
                    change.changed.insert(c);
            }
        } else {
            continue;
        }
        list.append(change);
    }
    return list;
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
    case Qt::FontRole: {
        const bool deleted = m_deleted.contains(index.row());
        if (isNull || deleted) {
            QFont font;
            font.setItalic(isNull);
            font.setStrikeOut(deleted); // 지울 행
            return font;
        }
        return {};
    }
    case Qt::BackgroundRole:
        // 반영하지 않은 편집: 지울 행, 추가한 행, 고친 셀
        if (m_deleted.contains(index.row()))
            return Theme::deletedRowColor();
        if (m_inserted.contains(index.row()))
            return Theme::insertedRowColor();
        if (isCellChanged(index.row(), index.column()))
            return Theme::editedCellColor();
        return {};
    case Qt::EditRole:
        return isNull ? QString() : fullText(value);
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
