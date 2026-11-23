#pragma once

#include "core/ResultEdits.h"

#include <QAbstractTableModel>
#include <QCborArray>
#include <QHash>
#include <QList>
#include <QSet>

// 쿼리 결과 표. 행은 엔진에서 페이지 단위로 받아 뒤에 붙인다.
// 표를 끝까지 스크롤하면 Qt가 fetchMore를 부르고, 모델은 moreRowsRequested로 다음 페이지를 요청한다.
// 편집 모드에서는 셀을 고치고 행을 더하거나 지울 수 있다. 바꾼 내용은 DB에 반영하기 전까지 모델에만 있고
// (고친 셀·추가한 행·지울 행을 색으로 구분), changes()로 꺼내 문장으로 만든다.
class ResultModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    struct Column
    {
        QString name;
        QString typeName;
        bool numeric = false;
    };

    explicit ResultModel(QObject *parent = nullptr);

    void reset(const QCborArray &columns);
    void appendRows(const QCborArray &rows, bool hasMore);
    void clear();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    // 복사용 원래 값 (NULL은 빈 문자열, 잘림 없음)
    QString copyText(int row, int column) const;
    QString columnName(int column) const;
    bool hasMore() const { return m_hasMore; }
    const QList<Column> &columns() const { return m_columns; }
    const QList<QCborArray> &rows() const { return m_rows; }
    QCborValue value(int row, int column) const;

    // 편집
    enum class RowState { Clean, Modified, Inserted, Deleted };
    void setEditing(bool editing, const QList<bool> &editableColumns = {});
    bool isEditing() const { return m_editing; }
    bool isColumnEditable(int column) const;
    void setCellValue(int row, int column, const QCborValue &value);
    int appendEmptyRow();
    void markDeleted(const QList<int> &rows); // 추가한 행은 바로 없앤다
    void revertChanges();
    void acceptChanges(); // 반영했으니 지금 값이 원래 값이 된다
    int pendingChangeCount() const;
    QList<RowChange> changes() const;
    RowState rowState(int row) const;
    bool isCellChanged(int row, int column) const;

signals:
    void moreRowsRequested();
    void changesChanged();

private:
    void removeRowAt(int row);

    QList<Column> m_columns;
    QList<QCborArray> m_rows;
    bool m_editing = false;
    QList<bool> m_editable;
    QHash<int, QCborArray> m_original; // 고친 행의 원래 값
    QSet<int> m_inserted;
    QSet<int> m_deleted;
    bool m_hasMore = false;
    bool m_fetchRequested = false;
};
