#include "ErdView.h"

#include "Theme.h"

#include <QCborMap>
#include <QFontMetricsF>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QImage>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QSvgGenerator>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>

namespace {

struct ErdColors
{
    QColor background, box, border, header, viewHeader, headerText, text, dim, pk, fk, line, highlight;
};

ErdColors darkColors()
{
    return {QColor(0x1e, 0x1f, 0x22), QColor(0x2b, 0x2d, 0x30), QColor(0x4e, 0x52, 0x58), QColor(0x2f, 0x4b, 0x73),
            QColor(0x3a, 0x55, 0x45), QColor(0xee, 0xf1, 0xf5), QColor(0xdf, 0xe1, 0xe5), QColor(0x8c, 0x91, 0x99),
            QColor(0xe6, 0xb4, 0x22), QColor(0x6c, 0xa8, 0xf0), QColor(0x80, 0x86, 0x8f), QColor(0x4d, 0x9b, 0xff)};
}

ErdColors lightColors()
{
    return {QColor(Qt::white), QColor(Qt::white), QColor(0xb4, 0xbb, 0xc6), QColor(0xdc, 0xe8, 0xfa),
            QColor(0xdc, 0xf0, 0xe2), QColor(0x1f, 0x2a, 0x37), QColor(0x22, 0x26, 0x2d), QColor(0x7a, 0x82, 0x8e),
            QColor(0xb7, 0x83, 0x00), QColor(0x1f, 0x6f, 0xd1), QColor(0x8a, 0x93, 0xa0), QColor(0x1a, 0x73, 0xe8)};
}

// 지금 그릴 색 (내보낼 때만 잠깐 밝은 색으로 바꾼다)
ErdColors g_colors = darkColors();

constexpr qreal kPadding = 10;
constexpr qreal kMarkerWidth = 24;
constexpr qreal kMaxWidth = 440;
constexpr qreal kMinZoom = 0.08;
constexpr qreal kMaxZoom = 3.0;

} // namespace

// ---------------------------------------------------------------------------

class ErdTableItem : public QGraphicsObject
{
public:
    ErdTableItem(const ErdTable &table, ErdView *view)
        : m_view(view)
        , m_table(table)
    {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setCursor(Qt::OpenHandCursor);
        setZValue(1);
        relayout(ErdView::Detail::AllColumns);
    }

    const ErdTable &table() const { return m_table; }
    QRectF box() const { return QRectF(0, 0, m_width, m_height); }
    QRectF sceneBox() const { return mapRectToScene(box()); }
    QRectF boundingRect() const override { return box().adjusted(-3, -3, 3, 3); }

    void relayout(ErdView::Detail detail)
    {
        prepareGeometryChange();
        m_detail = detail;
        m_visible.clear();
        for (int i = 0; i < m_table.columns.size(); ++i) {
            const ErdColumn &column = m_table.columns.at(i);
            if (detail == ErdView::Detail::AllColumns || (detail == ErdView::Detail::KeysOnly && (column.primaryKey || column.foreignKey)))
                m_visible << i;
        }
        const QFontMetricsF metrics(font());
        const QFontMetricsF bold(boldFont());
        m_rowHeight = std::ceil(metrics.height() + 6);
        m_headerHeight = std::ceil(bold.height() + 12);
        qreal nameWidth = 0;
        qreal typeWidth = 0;
        for (int index : std::as_const(m_visible)) {
            nameWidth = std::max(nameWidth, metrics.horizontalAdvance(m_table.columns.at(index).name));
            typeWidth = std::max(typeWidth, metrics.horizontalAdvance(m_table.columns.at(index).type));
        }
        qreal width = kPadding + kMarkerWidth + nameWidth + 18 + typeWidth + kPadding;
        width = std::max(width, bold.horizontalAdvance(title()) + 2 * kPadding);
        m_width = std::ceil(std::clamp(width, 140.0, kMaxWidth));
        m_typeWidth = std::min(typeWidth, m_width * 0.45);
        const int rows = int(m_visible.size()) + (detail == ErdView::Detail::KeysOnly && m_visible.size() < m_table.columns.size() ? 1 : 0);
        m_height = m_headerHeight + rows * m_rowHeight + (rows > 0 ? 6 : 0);
        update();
    }

    // 그 컬럼 줄의 세로 가운데 (항목 좌표). 보이지 않는 컬럼이면 머리글 가운데
    qreal columnY(const QString &column) const
    {
        for (int row = 0; row < m_visible.size(); ++row) {
            if (m_table.columns.at(m_visible.at(row)).name == column)
                return m_headerHeight + 3 + row * m_rowHeight + m_rowHeight / 2;
        }
        return m_headerHeight / 2;
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const ErdColors &c = g_colors;
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF rect = box();
        QPainterPath outline;
        outline.addRoundedRect(rect, 5, 5);
        painter->fillPath(outline, c.box);

        // 머리글
        QPainterPath header;
        header.addRoundedRect(QRectF(0, 0, m_width, m_headerHeight + 6), 5, 5);
        QPainterPath clip;
        clip.addRect(QRectF(0, 0, m_width, m_headerHeight));
        painter->fillPath(header.intersected(clip), m_table.isView() ? c.viewHeader : c.header);
        painter->setPen(c.headerText);
        painter->setFont(boldFont());
        painter->drawText(QRectF(kPadding, 0, m_width - 2 * kPadding, m_headerHeight), Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetricsF(boldFont()).elidedText(title(), Qt::ElideRight, m_width - 2 * kPadding));

        // 컬럼
        painter->setFont(font());
        const QFontMetricsF metrics(font());
        QFont marker = font();
        marker.setBold(true);
        marker.setPointSizeF(std::max(6.0, font().pointSizeF() - 2));
        qreal y = m_headerHeight + 3;
        for (int index : std::as_const(m_visible)) {
            const ErdColumn &column = m_table.columns.at(index);
            const QRectF row(0, y, m_width, m_rowHeight);
            if (column.primaryKey || column.foreignKey) {
                painter->setFont(marker);
                painter->setPen(column.primaryKey ? c.pk : c.fk);
                painter->drawText(QRectF(kPadding, y, kMarkerWidth, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                                  column.primaryKey && column.foreignKey ? QStringLiteral("PF")
                                  : column.primaryKey                    ? QStringLiteral("PK")
                                                                         : QStringLiteral("FK"));
                painter->setFont(font());
            }
            const qreal nameLeft = kPadding + kMarkerWidth;
            const qreal typeLeft = m_width - kPadding - m_typeWidth;
            QFont nameFont = font();
            nameFont.setBold(column.primaryKey);
            painter->setFont(nameFont);
            painter->setPen(c.text);
            painter->drawText(QRectF(nameLeft, row.top(), typeLeft - nameLeft - 8, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetricsF(nameFont).elidedText(column.name, Qt::ElideRight, typeLeft - nameLeft - 8));
            painter->setFont(font());
            painter->setPen(c.dim);
            painter->drawText(QRectF(typeLeft, row.top(), m_typeWidth, m_rowHeight), Qt::AlignVCenter | Qt::AlignRight,
                              metrics.elidedText(column.type, Qt::ElideRight, m_typeWidth));
            y += m_rowHeight;
        }
        if (m_detail == ErdView::Detail::KeysOnly && m_visible.size() < m_table.columns.size()) {
            painter->setPen(c.dim);
            painter->drawText(QRectF(kPadding + kMarkerWidth, y, m_width, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                              ErdView::tr("+ %n more", nullptr, int(m_table.columns.size() - m_visible.size())));
        }

        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(isSelected() ? c.highlight : c.border, isSelected() ? 2.0 : 1.0));
        painter->drawPath(outline);
    }

    QList<ErdRelationItem *> relations;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
    {
        if (change == ItemPositionHasChanged)
            m_view->tableMoved(this);
        return QGraphicsObject::itemChange(change, value);
    }

    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override
    {
        const qreal y = event->pos().y();
        if (y < m_headerHeight) {
            QStringList lines = {QStringLiteral("<b>%1</b> (%2)").arg(m_table.name.toHtmlEscaped(), m_table.type)};
            if (!m_table.remarks.isEmpty())
                lines << m_table.remarks.toHtmlEscaped();
            if (m_table.rows >= 0)
                lines << ErdView::tr("About %1 rows").arg(QLocale(QLocale::English).toString(m_table.rows));
            setToolTip(lines.join(QStringLiteral("<br>")));
            return;
        }
        const int row = int((y - m_headerHeight - 3) / m_rowHeight);
        if (row >= 0 && row < m_visible.size()) {
            const ErdColumn &column = m_table.columns.at(m_visible.at(row));
            QString tip = QStringLiteral("<b>%1</b> %2%3").arg(column.name.toHtmlEscaped(), column.type.toHtmlEscaped(),
                                                            column.nullable ? QString() : QStringLiteral(" NOT NULL"));
            if (!column.remarks.isEmpty())
                tip += QStringLiteral("<br>") + column.remarks.toHtmlEscaped();
            for (const ErdForeignKey &fk : m_table.foreignKeys) {
                const int at = int(fk.columns.indexOf(column.name));
                if (at >= 0)
                    tip += QStringLiteral("<br>→ %1.%2").arg(fk.refTable.toHtmlEscaped(), fk.refColumns.value(at).toHtmlEscaped());
            }
            setToolTip(tip);
        } else {
            setToolTip({});
        }
    }

    void mousePressEvent(QGraphicsSceneMouseEvent *event) override
    {
        setCursor(Qt::ClosedHandCursor);
        QGraphicsObject::mousePressEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override
    {
        setCursor(Qt::OpenHandCursor);
        QGraphicsObject::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override
    {
        emit m_view->tableActivated(m_table.name);
        QGraphicsObject::mouseDoubleClickEvent(event);
    }

private:
    QFont font() const { return m_view->font(); }
    QFont boldFont() const
    {
        QFont f = m_view->font();
        f.setBold(true);
        return f;
    }
    QString title() const { return m_table.name; }

    ErdView *m_view = nullptr;
    ErdTable m_table;
    ErdView::Detail m_detail = ErdView::Detail::AllColumns;
    QList<int> m_visible;
    qreal m_width = 160;
    qreal m_height = 40;
    qreal m_rowHeight = 18;
    qreal m_headerHeight = 24;
    qreal m_typeWidth = 60;
};

// ---------------------------------------------------------------------------

class ErdRelationItem : public QGraphicsItem
{
public:
    ErdRelationItem(ErdTableItem *child, ErdTableItem *parent, const ErdForeignKey &fk)
        : m_child(child)
        , m_parent(parent)
        , m_fk(fk)
    {
        setZValue(0);
        setAcceptHoverEvents(true);
        child->relations << this;
        if (parent != child)
            parent->relations << this;
        setToolTip(QStringLiteral("%1\n%2 (%3) → %4 (%5)")
                       .arg(fk.name, child->table().name, fk.columns.join(QStringLiteral(", ")), fk.refTable,
                            fk.refColumns.join(QStringLiteral(", "))));
        updatePath();
    }

    ErdTableItem *child() const { return m_child; }
    ErdTableItem *parentTable() const { return m_parent; }

    void setHighlighted(bool on)
    {
        if (m_highlighted == on)
            return;
        m_highlighted = on;
        setZValue(on ? 0.5 : 0);
        update();
    }

    void updatePath()
    {
        prepareGeometryChange();
        const QRectF c = m_child->sceneBox();
        const QRectF p = m_parent->sceneBox();
        const qreal cy = m_child->mapToScene(0, m_child->columnY(m_fk.columns.value(0))).y();
        const qreal py = m_parent->mapToScene(0, m_parent->columnY(m_fk.refColumns.value(0))).y();
        QPointF start, end;
        if (m_child == m_parent) {
            start = QPointF(c.right(), cy);
            end = QPointF(c.right(), py == cy ? cy + 8 : py);
            m_startDir = 1;
            m_endDir = 1;
        } else if (c.left() > p.right()) {
            start = QPointF(c.left(), cy);
            end = QPointF(p.right(), py);
            m_startDir = -1;
            m_endDir = 1;
        } else if (c.right() < p.left()) {
            start = QPointF(c.right(), cy);
            end = QPointF(p.left(), py);
            m_startDir = 1;
            m_endDir = -1;
        } else {
            // 가로로 겹치면 둘 다 오른쪽 변에서 돌아 나간다
            start = QPointF(c.right(), cy);
            end = QPointF(p.right(), py);
            m_startDir = 1;
            m_endDir = 1;
        }
        m_start = start;
        m_end = end;
        const qreal reach = m_startDir == m_endDir ? 40 + std::abs(start.x() - end.x()) : std::max(40.0, std::abs(end.x() - start.x()) / 2);
        QPainterPath path(start);
        path.cubicTo(start + QPointF(m_startDir * reach, 0), end + QPointF(m_endDir * reach, 0), end);
        m_path = path;
        update();
    }

    QRectF boundingRect() const override { return m_path.boundingRect().adjusted(-14, -14, 14, 14); }

    QPainterPath shape() const override
    {
        QPainterPathStroker stroker;
        stroker.setWidth(8);
        return stroker.createStroke(m_path);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const ErdColors &c = g_colors;
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor color = m_highlighted ? c.highlight : c.line;
        painter->setPen(QPen(color, m_highlighted ? 2.0 : 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(m_path);
        // 자식 쪽: 까마귀 발 (여럿)
        const QPointF foot = m_start + QPointF(m_startDir * 12, 0);
        painter->drawLine(foot, m_start + QPointF(0, -6));
        painter->drawLine(foot, m_start);
        painter->drawLine(foot, m_start + QPointF(0, 6));
        // 부모 쪽: 막대 둘 (하나, 반드시)
        for (qreal offset : {7.0, 11.0}) {
            const QPointF bar = m_end + QPointF(m_endDir * offset, 0);
            painter->drawLine(bar + QPointF(0, -6), bar + QPointF(0, 6));
        }
    }

private:
    ErdTableItem *m_child = nullptr;
    ErdTableItem *m_parent = nullptr;
    ErdForeignKey m_fk;
    QPainterPath m_path;
    QPointF m_start, m_end;
    int m_startDir = 1;
    int m_endDir = -1;
    bool m_highlighted = false;
};

// ---------------------------------------------------------------------------

QList<ErdTable> ErdTable::fromModel(const QCborArray &tables)
{
    QList<ErdTable> list;
    for (const QCborValue &value : tables) {
        const QCborMap t = value.toMap();
        ErdTable table;
        table.name = t.value(QStringLiteral("name")).toString();
        table.type = t.value(QStringLiteral("type")).toString();
        table.remarks = t.value(QStringLiteral("remarks")).toString();
        table.rows = t.value(QStringLiteral("rows")).isInteger() ? t.value(QStringLiteral("rows")).toInteger() : -1;
        for (const QCborValue &item : t.value(QStringLiteral("columns")).toArray()) {
            const QCborMap c = item.toMap();
            ErdColumn column;
            column.name = c.value(QStringLiteral("name")).toString();
            column.type = c.value(QStringLiteral("type")).toString();
            column.remarks = c.value(QStringLiteral("remarks")).toString();
            column.nullable = c.value(QStringLiteral("nullable")).toBool();
            column.primaryKey = c.value(QStringLiteral("primaryKey")).toBool();
            column.foreignKey = c.value(QStringLiteral("foreignKey")).toBool();
            table.columns << column;
        }
        auto strings = [](const QCborValue &v) {
            QStringList out;
            for (const QCborValue &s : v.toArray())
                out << s.toString();
            return out;
        };
        for (const QCborValue &item : t.value(QStringLiteral("foreignKeys")).toArray()) {
            const QCborMap f = item.toMap();
            ErdForeignKey fk;
            fk.name = f.value(QStringLiteral("name")).toString();
            fk.columns = strings(f.value(QStringLiteral("columns")));
            fk.refSchema = f.value(QStringLiteral("refSchema")).toString();
            fk.refTable = f.value(QStringLiteral("refTable")).toString();
            fk.refColumns = strings(f.value(QStringLiteral("refColumns")));
            table.foreignKeys << fk;
        }
        list << table;
    }
    return list;
}

ErdView::ErdView(QWidget *parent)
    : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("erdView"));
    setScene(new QGraphicsScene(this));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag); // 빈 곳을 끌면 화면 이동, 상자는 상자를 옮긴다
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    applyColors();
    connect(scene(), &QGraphicsScene::selectionChanged, this, &ErdView::selectionChanged);
}

void ErdView::applyColors()
{
    g_colors = Theme::current() == Theme::Kind::Light ? lightColors() : darkColors();
    setBackgroundBrush(g_colors.background);
    if (scene())
        scene()->update();
}

void ErdView::changeEvent(QEvent *event)
{
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyColors();
}

void ErdView::setModel(const QString &schema, const QList<ErdTable> &tables)
{
    m_schema = schema;
    scene()->clear();
    m_tables.clear();
    m_relations.clear();
    for (const ErdTable &table : tables) {
        auto *item = new ErdTableItem(table, this);
        item->relayout(m_detail);
        scene()->addItem(item);
        m_tables.insert(table.name, item);
    }
    for (ErdTableItem *child : std::as_const(m_tables)) {
        for (const ErdForeignKey &fk : child->table().foreignKeys) {
            if (!fk.refSchema.isEmpty() && fk.refSchema != schema)
                continue; // 다른 스키마 테이블은 그리지 않는다 (툴팁에는 나온다)
            ErdTableItem *parent = m_tables.value(fk.refTable);
            if (!parent)
                continue;
            auto *relation = new ErdRelationItem(child, parent, fk);
            scene()->addItem(relation);
            m_relations << relation;
        }
    }
    arrange();
}

void ErdView::setDetail(Detail detail)
{
    if (m_detail == detail)
        return;
    m_detail = detail;
    for (ErdTableItem *item : std::as_const(m_tables))
        item->relayout(detail);
    arrange(); // 높이가 바뀌어 겹치지 않게 다시 놓는다
}

void ErdView::arrange()
{
    // 외래 키 그래프: 자식 → 부모
    QHash<QString, QSet<QString>> parents;
    QHash<QString, QSet<QString>> neighbours;
    for (ErdRelationItem *relation : std::as_const(m_relations)) {
        const QString child = relation->child()->table().name;
        const QString parent = relation->parentTable()->table().name;
        if (child == parent)
            continue;
        parents[child].insert(parent);
        neighbours[child].insert(parent);
        neighbours[parent].insert(child);
    }

    // 층: 아무것도 가리키지 않는 테이블이 0층, 자식은 부모보다 오른쪽 (순환은 끊는다)
    QHash<QString, int> rank;
    QSet<QString> visiting;
    std::function<int(const QString &)> rankOf = [&](const QString &name) -> int {
        if (rank.contains(name))
            return rank.value(name);
        if (visiting.contains(name))
            return 0;
        visiting.insert(name);
        int r = 0;
        for (const QString &parent : parents.value(name))
            r = std::max(r, rankOf(parent) + 1);
        visiting.remove(name);
        rank.insert(name, r);
        return r;
    };

    // 아무것도 가리키지 않는 테이블(부모만 되는 테이블)은 0층이 아니라 자식 바로 왼쪽에 둔다.
    // 그래야 멀리 떨어진 자식과 이은 선이 다른 상자 뒤로 지나가지 않는다.
    QHash<QString, QSet<QString>> children;
    for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
        for (const QString &parent : it.value())
            children[parent].insert(it.key());
    }
    for (const QString &name : m_tables.keys())
        rankOf(name);
    for (auto it = children.cbegin(); it != children.cend(); ++it) {
        if (!parents.value(it.key()).isEmpty())
            continue;
        int nearest = INT_MAX;
        for (const QString &child : it.value())
            nearest = std::min(nearest, rank.value(child));
        if (nearest != INT_MAX)
            rank.insert(it.key(), std::max(0, nearest - 1));
    }

    // 이어진 묶음
    QStringList names = m_tables.keys();
    std::sort(names.begin(), names.end());
    QSet<QString> seen;
    QList<QStringList> components;
    QStringList isolated;
    for (const QString &name : std::as_const(names)) {
        if (seen.contains(name))
            continue;
        if (!neighbours.contains(name)) {
            isolated << name;
            seen.insert(name);
            continue;
        }
        QStringList component;
        QList<QString> queue = {name};
        seen.insert(name);
        while (!queue.isEmpty()) {
            const QString current = queue.takeFirst();
            component << current;
            for (const QString &next : neighbours.value(current)) {
                if (!seen.contains(next)) {
                    seen.insert(next);
                    queue << next;
                }
            }
        }
        components << component;
    }
    std::sort(components.begin(), components.end(), [](const QStringList &a, const QStringList &b) { return a.size() > b.size(); });

    constexpr qreal columnGap = 90;
    constexpr qreal rowGap = 28;
    constexpr qreal maxColumnHeight = 1500;
    qreal top = 0;
    qreal widest = 0;
    for (const QStringList &component : std::as_const(components)) {
        QMap<int, QStringList> layers;
        for (const QString &name : component)
            layers[rankOf(name)] << name;
        // 부모들의 위치 평균으로 줄을 맞춘다 (교차를 줄인다)
        QHash<QString, qreal> order;
        for (auto it = layers.begin(); it != layers.end(); ++it) {
            QStringList &layer = it.value();
            std::sort(layer.begin(), layer.end());
            if (it.key() > 0) {
                auto key = [&](const QString &name) {
                    qreal sum = 0;
                    int count = 0;
                    for (const QString &parent : parents.value(name)) {
                        if (order.contains(parent)) {
                            sum += order.value(parent);
                            ++count;
                        }
                    }
                    return count ? sum / count : 1e9;
                };
                std::stable_sort(layer.begin(), layer.end(), [&](const QString &a, const QString &b) { return key(a) < key(b); });
            }
            for (int i = 0; i < layer.size(); ++i)
                order.insert(layer.at(i), i);
        }

        qreal x = 0;
        qreal componentHeight = 0;
        for (const QStringList &layer : std::as_const(layers)) {
            qreal y = 0;
            qreal columnWidth = 0;
            for (const QString &name : layer) {
                ErdTableItem *item = m_tables.value(name);
                const QRectF box = item->box();
                if (y > 0 && y + box.height() > maxColumnHeight) {
                    // 너무 길면 옆 줄로
                    x += columnWidth + columnGap / 2;
                    componentHeight = std::max(componentHeight, y);
                    y = 0;
                    columnWidth = 0;
                }
                item->setPos(x, top + y);
                y += box.height() + rowGap;
                columnWidth = std::max(columnWidth, box.width());
            }
            componentHeight = std::max(componentHeight, y);
            x += columnWidth + columnGap;
        }
        widest = std::max(widest, x);
        top += componentHeight + 70;
    }

    // 홀로 있는 테이블: 격자
    if (!isolated.isEmpty()) {
        const qreal limit = std::max(widest, 1100.0);
        qreal x = 0;
        qreal y = top;
        qreal rowHeight = 0;
        for (const QString &name : std::as_const(isolated)) {
            ErdTableItem *item = m_tables.value(name);
            const QRectF box = item->box();
            if (x > 0 && x + box.width() > limit) {
                x = 0;
                y += rowHeight + rowGap;
                rowHeight = 0;
            }
            item->setPos(x, y);
            x += box.width() + 40;
            rowHeight = std::max(rowHeight, box.height());
        }
    }
    for (ErdRelationItem *relation : std::as_const(m_relations))
        relation->updatePath();
    scene()->setSceneRect(contentRect().adjusted(-400, -400, 400, 400));
}

QRectF ErdView::contentRect() const
{
    QRectF rect;
    for (ErdTableItem *item : m_tables)
        rect = rect.united(item->sceneBox());
    for (ErdRelationItem *relation : m_relations)
        rect = rect.united(relation->sceneBoundingRect());
    return rect.adjusted(-30, -30, 30, 30);
}

void ErdView::fitAll()
{
    const QRectF rect = contentRect();
    if (rect.isEmpty())
        return;
    fitInView(rect, Qt::KeepAspectRatio);
    // 테이블이 적으면 너무 크게 키우지 않는다
    if (zoom() > 1.0)
        setTransform(QTransform());
    centerOn(rect.center());
    emit zoomChanged(zoom());
}

qreal ErdView::zoom() const
{
    return transform().m11();
}

void ErdView::zoomBy(qreal factor)
{
    const qreal target = std::clamp(zoom() * factor, kMinZoom, kMaxZoom);
    const qreal applied = target / zoom();
    scale(applied, applied);
    emit zoomChanged(zoom());
}

void ErdView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        zoomBy(std::pow(1.15, event->angleDelta().y() / 120.0));
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

bool ErdView::focusTable(const QString &name)
{
    ErdTableItem *item = m_tables.value(name);
    if (!item) {
        for (auto it = m_tables.cbegin(); it != m_tables.cend(); ++it) {
            if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
                item = it.value();
                break;
            }
        }
    }
    if (!item)
        return false;
    scene()->clearSelection();
    item->setSelected(true);
    centerOn(item->sceneBox().center());
    return true;
}

QStringList ErdView::tableNames() const
{
    QStringList names = m_tables.keys();
    std::sort(names.begin(), names.end());
    return names;
}

QPointF ErdView::tablePosition(const QString &name) const
{
    ErdTableItem *item = m_tables.value(name);
    return item ? item->pos() : QPointF();
}

void ErdView::tableMoved(ErdTableItem *item)
{
    for (ErdRelationItem *relation : std::as_const(item->relations))
        relation->updatePath();
}

void ErdView::selectionChanged()
{
    QSet<ErdTableItem *> selected;
    for (QGraphicsItem *item : scene()->selectedItems()) {
        if (auto *table = dynamic_cast<ErdTableItem *>(item))
            selected.insert(table);
    }
    for (ErdRelationItem *relation : std::as_const(m_relations))
        relation->setHighlighted(selected.contains(relation->child()) || selected.contains(relation->parentTable()));
}

void ErdView::renderTo(QPainter *painter, const QRectF &target, const QRectF &source)
{
    // 내보낼 때는 흰 바탕·밝은 색, 고른 표시 없이
    const QList<QGraphicsItem *> selected = scene()->selectedItems();
    scene()->clearSelection();
    const ErdColors saved = g_colors;
    g_colors = lightColors();
    painter->fillRect(target, g_colors.background);
    scene()->render(painter, target, source);
    g_colors = saved;
    for (QGraphicsItem *item : selected)
        item->setSelected(true);
}

bool ErdView::exportPng(const QString &path, qreal scale)
{
    const QRectF source = contentRect();
    if (source.isEmpty())
        return false;
    // 아주 큰 ERD는 이미지 크기를 줄인다 (가로·세로 16000px 이하)
    scale = std::min(scale, 16000.0 / std::max(source.width(), source.height()));
    QImage image(QSize(int(std::ceil(source.width() * scale)), int(std::ceil(source.height() * scale))), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    renderTo(&painter, QRectF(QPointF(0, 0), QSizeF(image.size())), source);
    painter.end();
    return image.save(path, "PNG");
}

bool ErdView::exportSvg(const QString &path)
{
    const QRectF source = contentRect();
    if (source.isEmpty())
        return false;
    QSvgGenerator generator;
    generator.setFileName(path);
    generator.setSize(source.size().toSize());
    generator.setViewBox(QRectF(QPointF(0, 0), source.size()));
    generator.setTitle(m_schema);
    generator.setDescription(tr("ERD of %1, made with DBaro").arg(m_schema));
    QPainter painter;
    if (!painter.begin(&generator))
        return false;
    renderTo(&painter, QRectF(QPointF(0, 0), source.size()), source);
    painter.end();
    return true;
}
