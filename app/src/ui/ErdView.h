#pragma once

#include <QCborArray>
#include <QGraphicsView>
#include <QHash>
#include <QList>
#include <QStringList>

class ErdTableItem;
class ErdRelationItem;

// ERD에 그릴 모델 (엔진 reverseModel의 테이블 하나)
struct ErdColumn
{
    QString name;
    QString type;
    QString remarks;
    bool nullable = true;
    bool primaryKey = false;
    bool foreignKey = false;
};

struct ErdForeignKey
{
    QString name;
    QStringList columns;
    QString refSchema;
    QString refTable;
    QStringList refColumns;
};

struct ErdTable
{
    QString name;
    QString type; // TABLE, VIEW, PARTITIONED TABLE ...
    QString remarks;
    qint64 rows = -1;
    QList<ErdColumn> columns;
    QList<ErdForeignKey> foreignKeys;

    bool isView() const { return type.endsWith(QLatin1String("VIEW")); }
    static QList<ErdTable> fromModel(const QCborArray &tables);
};

// ERD 화면. 테이블 상자(컬럼, PK·FK 표시)와 외래 키 선(자식 쪽 까마귀 발, 부모 쪽 막대).
// - 자동 배치: 외래 키로 이어진 테이블끼리 묶어, 가리켜지는 테이블을 왼쪽에 두는 층 배치. 홀로 있는 테이블은 아래 격자로
// - 상자는 끌어서 옮기고(선이 따라온다), Ctrl+휠로 확대·축소, 빈 곳을 끌면 화면 이동
// - 고른 테이블과 그 선을 강조한다. 두 번 클릭하면 tableActivated
// - 내보내기는 테마와 상관없이 흰 바탕으로 그린다 (문서에 붙이기 좋게)
class ErdView : public QGraphicsView
{
    Q_OBJECT

public:
    enum class Detail { AllColumns, KeysOnly, NamesOnly };

    explicit ErdView(QWidget *parent = nullptr);

    void setModel(const QString &schema, const QList<ErdTable> &tables);
    void setDetail(Detail detail);
    Detail detail() const { return m_detail; }
    void arrange();             // 자동 배치
    void fitAll();              // 전체가 보이게
    void zoomBy(qreal factor);
    qreal zoom() const;
    bool focusTable(const QString &name); // 그 테이블을 고르고 가운데로
    QStringList tableNames() const;
    int relationCount() const { return int(m_relations.size()); }
    QPointF tablePosition(const QString &name) const;

    // 흰 바탕으로 그린다. 실패하면 false
    bool exportPng(const QString &path, qreal scale = 2.0);
    bool exportSvg(const QString &path);

signals:
    void tableActivated(const QString &name);
    void zoomChanged(qreal zoom);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    friend class ErdTableItem;
    friend class ErdRelationItem;
    void tableMoved(ErdTableItem *item);
    void selectionChanged();
    void applyColors();
    void renderTo(QPainter *painter, const QRectF &target, const QRectF &source);
    QRectF contentRect() const;

    Detail m_detail = Detail::AllColumns;
    QString m_schema;
    QHash<QString, ErdTableItem *> m_tables;
    QList<ErdRelationItem *> m_relations;
};
