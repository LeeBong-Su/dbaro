#pragma once

#include <QAbstractTableModel>
#include <QCborArray>
#include <QList>

// 쿼리 결과 표. 행은 엔진에서 페이지 단위로 받아 뒤에 붙인다.
// 표를 끝까지 스크롤하면 Qt가 fetchMore를 부르고, 모델은 moreRowsRequested로 다음 페이지를 요청한다.
class ResultModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit ResultModel(QObject *parent = nullptr);

    void reset(const QCborArray &columns);
    void appendRows(const QCborArray &rows, bool hasMore);
    void clear();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    // 복사용 원래 값 (NULL은 빈 문자열, 잘림 없음)
    QString copyText(int row, int column) const;
    QString columnName(int column) const;
    bool hasMore() const { return m_hasMore; }

signals:
    void moreRowsRequested();

private:
    struct Column
    {
        QString name;
        QString typeName;
        bool numeric = false;
    };

    QList<Column> m_columns;
    QList<QCborArray> m_rows;
    bool m_hasMore = false;
    bool m_fetchRequested = false;
};
