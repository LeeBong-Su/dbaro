#include "ObjectExplorer.h"

#include "Icons.h"
#include "ScrollHelpers.h"
#include "UiHelpers.h"
#include "core/SqlDialect.h"
#include "core/SqlIdentifiers.h"
#include "MetadataService.h"

#include <QAction>
#include <QApplication>
#include <QCborArray>
#include <QClipboard>
#include <QHeaderView>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

enum Role {
    KindRole = Qt::UserRole + 1,
    ConnectionIdRole,
    SchemaRole,
    NameRole,
    TypeRole,
    LoadedRole,
    LoadingRole,
    DetailRole, // 이름 옆에 흐리게 보이는 보조 정보 (타입, 코멘트, 개수)
};

enum Kind { ConnectionKind = 1, SchemaKind, FolderKind, ObjectKind, ColumnKind, PlaceholderKind };

int kindOf(const QStandardItem *item)
{
    return item ? item->data(KindRole).toInt() : 0;
}

QString normalizedType(const QString &type)
{
    const QString upper = type.toUpper();
    return upper == u"BASE TABLE" ? QStringLiteral("TABLE") : upper;
}

bool hasColumns(const QString &type)
{
    return type.contains(u"TABLE") || type.contains(u"VIEW");
}

int typeOrder(const QString &type)
{
    static const QStringList order{QStringLiteral("TABLE"), QStringLiteral("VIEW"), QStringLiteral("MATERIALIZED VIEW"),
                                   QStringLiteral("SYNONYM"), QStringLiteral("SEQUENCE"), QStringLiteral("INDEX"),
                                   QStringLiteral("FUNCTION"), QStringLiteral("PROCEDURE"), QStringLiteral("TYPE")};
    const int index = int(order.indexOf(type));
    if (index >= 0)
        return index;
    return type.startsWith(u"SYSTEM") ? 100 : 50;
}

QString folderTitle(const QString &type)
{
    static const QHash<QString, QString> titles{
        {QStringLiteral("TABLE"), QStringLiteral("Tables")},
        {QStringLiteral("VIEW"), QStringLiteral("Views")},
        {QStringLiteral("MATERIALIZED VIEW"), QStringLiteral("Materialized Views")},
        {QStringLiteral("SYNONYM"), QStringLiteral("Synonyms")},
        {QStringLiteral("SEQUENCE"), QStringLiteral("Sequences")},
        {QStringLiteral("INDEX"), QStringLiteral("Indexes")},
        {QStringLiteral("FUNCTION"), QStringLiteral("Functions")},
        {QStringLiteral("PROCEDURE"), QStringLiteral("Procedures")},
        {QStringLiteral("TYPE"), QStringLiteral("Types")},
        {QStringLiteral("SYSTEM TABLE"), QStringLiteral("System Tables")},
        {QStringLiteral("SYSTEM VIEW"), QStringLiteral("System Views")},
    };
    if (titles.contains(type))
        return titles.value(type);
    QString title = type.toLower();
    if (!title.isEmpty())
        title[0] = title[0].toUpper();
    return title + u's';
}

QIcon objectIcon(const QString &type)
{
    if (type.contains(u"VIEW"))
        return Icons::get(QStringLiteral("view"));
    if (type.contains(u"TABLE"))
        return Icons::get(QStringLiteral("table"));
    if (type == u"FUNCTION" || type == u"PROCEDURE")
        return Icons::get(QStringLiteral("routine"));
    return Icons::get(QStringLiteral("object"));
}

// 이름 뒤에 보조 정보(타입·코멘트·개수)를 흐리게 그리는 델리게이트
class ExplorerDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QString detail = index.data(DetailRole).toString();
        if (detail.isEmpty())
            return;

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const QRect detailRect = textRect.adjusted(opt.fontMetrics.horizontalAdvance(opt.text) + 10, 0, 0, 0);
        if (detailRect.width() <= 4)
            return;

        painter->save();
        const bool selected = opt.state & QStyle::State_Selected;
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->setFont(opt.font);
        painter->drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                          opt.fontMetrics.elidedText(detail, Qt::ElideRight, detailRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const QString detail = index.data(DetailRole).toString();
        if (!detail.isEmpty())
            size.rwidth() += option.fontMetrics.horizontalAdvance(detail) + 10;
        return size;
    }
};

} // namespace

ObjectExplorer::ObjectExplorer(MetadataService *metadata, QWidget *parent)
    : QWidget(parent)
    , m_metadata(metadata)
{
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("explorerFilter"));
    m_filter->setPlaceholderText(tr("Filter loaded objects"));
    m_filter->setClearButtonEnabled(true);

    m_model = new QStandardItemModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setRecursiveFilteringEnabled(true);

    m_tree = new QTreeView(this);
    m_tree->setObjectName(QStringLiteral("explorerTree"));
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setModel(m_proxy);
    m_tree->setItemDelegate(new ExplorerDelegate(m_tree));
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setExpandsOnDoubleClick(false); // 두 번 클릭은 직접 처리한다 (테이블은 데이터 조회)
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents); // 긴 이름은 가로 스크롤
    enableShiftWheelHorizontalScroll(m_tree);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);

    connect(m_filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_tree, &QTreeView::expanded, this, &ObjectExplorer::onExpanded);
    connect(m_tree, &QTreeView::doubleClicked, this, &ObjectExplorer::onDoubleClicked);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &ObjectExplorer::showContextMenu);
    if (metadata)
        connect(metadata, &MetadataService::connectionChanged, this, &ObjectExplorer::setConnectedLook);

    setConnections({});
}

ObjectExplorer::~ObjectExplorer() = default;

void ObjectExplorer::setConnections(const QList<ConnectionProfile> &profiles)
{
    ++m_generation; // 이전 트리에 대한 응답은 버린다
    m_pendingRequests = 0;
    m_profiles = profiles;
    m_model->clear();

    if (profiles.isEmpty()) {
        auto *empty = new QStandardItem(tr("No connections yet. Add one with Database > New Connection."));
        empty->setData(PlaceholderKind, KindRole);
        empty->setFlags(Qt::NoItemFlags);
        m_model->appendRow(empty);
        return;
    }

    for (const ConnectionProfile &profile : profiles) {
        auto *item = new QStandardItem(colorSwatchIcon(Env::color(profile.environment)), profile.name);
        item->setEditable(false);
        item->setData(ConnectionKind, KindRole);
        item->setData(profile.id, ConnectionIdRole);
        item->setData(Db::typeName(profile.type), DetailRole);
        item->setToolTip(QStringLiteral("%1 — %2").arg(Db::typeName(profile.type), Db::jdbcUrl(profile)));
        setPlaceholder(item, tr("Expand to connect"));
        m_model->appendRow(item);
        if (m_metadata && m_metadata->isConnected(profile.id))
            setConnectedLook(profile.id, true);
    }
}

void ObjectExplorer::setConnectedLook(const QString &profileId, bool connected)
{
    if (QStandardItem *item = findConnection(profileId)) {
        QFont font = item->font();
        font.setBold(connected); // 연결된 항목은 굵게
        item->setFont(font);
    }
}

// ---------------------------------------------------------------------------
// 펼치기와 불러오기

void ObjectExplorer::onExpanded(const QModelIndex &proxyIndex)
{
    QStandardItem *item = sourceItem(proxyIndex);
    if (item && !item->data(LoadedRole).toBool() && !item->data(LoadingRole).toBool())
        load(item);
}

void ObjectExplorer::load(QStandardItem *item)
{
    switch (kindOf(item)) {
    case ConnectionKind:
        loadSchemas(item);
        break;
    case SchemaKind:
        loadObjects(item);
        break;
    case ObjectKind:
        if (hasColumns(item->data(TypeRole).toString()))
            loadColumns(item);
        break;
    default:
        break;
    }
}

void ObjectExplorer::reload(QStandardItem *item)
{
    if (kindOf(item) == FolderKind)
        item = item->parent(); // 폴더는 스키마 단위로 다시 불러온다
    if (!item || kindOf(item) == ColumnKind || kindOf(item) == PlaceholderKind)
        return;
    item->setData(false, LoadedRole);
    m_refreshNext = true;
    load(item);
    m_refreshNext = false;
    expandItem(item);
}

void ObjectExplorer::loadSchemas(QStandardItem *connection)
{
    request(connection, QStringLiteral("metaSchemas"), {}, [this](QStandardItem *item, const QCborMap &result) {
        const QString connectionId = item->data(ConnectionIdRole).toString();
        const QString current = result.value(QStringLiteral("current")).toString();
        QStandardItem *currentItem = nullptr;

        for (const QCborValue &value : result.value(QStringLiteral("names")).toArray()) {
            const QString name = value.toString();
            auto *schema = new QStandardItem(Icons::get(QStringLiteral("schema")), name);
            schema->setEditable(false);
            schema->setData(SchemaKind, KindRole);
            schema->setData(connectionId, ConnectionIdRole);
            schema->setData(name, SchemaRole);
            setPlaceholder(schema, tr("Expand to load"));
            if (!current.isEmpty() && name.compare(current, Qt::CaseInsensitive) == 0) {
                QFont font = schema->font();
                font.setBold(true);
                schema->setFont(font);
                schema->setData(tr("current"), DetailRole);
                currentItem = schema;
            }
            item->appendRow(schema);
        }

        if (item->rowCount() == 0) {
            setPlaceholder(item, tr("No schemas"));
            return;
        }
        // 지금 접속한 스키마를 맨 위에 두고 바로 펼친다.
        if (currentItem) {
            item->insertRow(0, item->takeRow(currentItem->row()));
            expandItem(currentItem);
        }
    });
}

void ObjectExplorer::loadObjects(QStandardItem *schema)
{
    QCborMap params;
    params.insert(QStringLiteral("schema"), schema->data(SchemaRole).toString());
    request(schema, QStringLiteral("metaObjects"), params, [this](QStandardItem *item, const QCborMap &result) {
        const QString connectionId = item->data(ConnectionIdRole).toString();
        const QString schemaName = item->data(SchemaRole).toString();

        QMap<QString, QList<QCborMap>> groups;
        for (const QCborValue &value : result.value(QStringLiteral("objects")).toArray()) {
            const QCborMap object = value.toMap();
            groups[normalizedType(object.value(QStringLiteral("type")).toString())].append(object);
        }

        QStringList types = groups.keys();
        std::sort(types.begin(), types.end(), [](const QString &a, const QString &b) {
            const int oa = typeOrder(a), ob = typeOrder(b);
            return oa != ob ? oa < ob : a < b;
        });

        QStandardItem *firstFolder = nullptr;
        for (const QString &type : types) {
            QList<QCborMap> objects = groups.value(type);
            std::sort(objects.begin(), objects.end(), [](const QCborMap &a, const QCborMap &b) {
                return a.value(QStringLiteral("name")).toString().compare(b.value(QStringLiteral("name")).toString(),
                                                                          Qt::CaseInsensitive)
                       < 0;
            });

            auto *folder = new QStandardItem(Icons::get(QStringLiteral("folder")), folderTitle(type));
            folder->setEditable(false);
            folder->setData(FolderKind, KindRole);
            folder->setData(connectionId, ConnectionIdRole);
            folder->setData(schemaName, SchemaRole);
            folder->setData(true, LoadedRole);
            folder->setData(QString::number(objects.size()), DetailRole);

            for (const QCborMap &object : objects) {
                const QString name = object.value(QStringLiteral("name")).toString();
                const QString remarks = object.value(QStringLiteral("remarks")).toString().trimmed();
                auto *node = new QStandardItem(objectIcon(type), name);
                node->setEditable(false);
                node->setData(ObjectKind, KindRole);
                node->setData(connectionId, ConnectionIdRole);
                node->setData(schemaName, SchemaRole);
                node->setData(name, NameRole);
                node->setData(type, TypeRole);
                node->setData(remarks, DetailRole);
                node->setToolTip(remarks.isEmpty() ? type : QStringLiteral("%1\n%2").arg(type, remarks));
                if (hasColumns(type))
                    setPlaceholder(node, tr("Expand to load columns"));
                else
                    node->setData(true, LoadedRole);
                folder->appendRow(node);
            }
            item->appendRow(folder);
            if (!firstFolder)
                firstFolder = folder;
        }

        if (item->rowCount() == 0) {
            setPlaceholder(item, tr("No objects"));
            item->setData(true, LoadedRole);
            return;
        }
        if (result.value(QStringLiteral("truncated")).toBool()) {
            auto *note = new QStandardItem(tr("Only the first 20,000 objects are shown."));
            note->setData(PlaceholderKind, KindRole);
            note->setFlags(Qt::ItemIsEnabled);
            item->appendRow(note);
        }
        if (firstFolder)
            expandItem(firstFolder);
    });
}

void ObjectExplorer::loadColumns(QStandardItem *object)
{
    QCborMap params;
    params.insert(QStringLiteral("schema"), object->data(SchemaRole).toString());
    params.insert(QStringLiteral("table"), object->data(NameRole).toString());
    request(object, QStringLiteral("metaColumns"), params, [](QStandardItem *item, const QCborMap &result) {
        for (const QCborValue &value : result.value(QStringLiteral("columns")).toArray()) {
            const QCborMap column = value.toMap();
            const QString name = column.value(QStringLiteral("name")).toString();
            const bool primaryKey = column.value(QStringLiteral("primaryKey")).toBool();
            const bool nullable = column.value(QStringLiteral("nullable")).toBool();
            const QString remarks = column.value(QStringLiteral("remarks")).toString().trimmed();
            const QString defaultValue = column.value(QStringLiteral("default")).toString();

            QString detail = column.value(QStringLiteral("type")).toString();
            if (primaryKey)
                detail += QStringLiteral(" · PK");
            if (!nullable)
                detail += QStringLiteral(" · NOT NULL");
            if (!remarks.isEmpty())
                detail += QStringLiteral("   ") + remarks;

            auto *node = new QStandardItem(Icons::get(primaryKey ? QStringLiteral("key") : QStringLiteral("column")), name);
            node->setEditable(false);
            node->setData(ColumnKind, KindRole);
            node->setData(item->data(ConnectionIdRole), ConnectionIdRole);
            node->setData(item->data(SchemaRole), SchemaRole);
            node->setData(name, NameRole);
            node->setData(detail, DetailRole);
            QString tip = detail;
            if (!defaultValue.isEmpty())
                tip += QStringLiteral("\nDEFAULT %1").arg(defaultValue);
            node->setToolTip(tip);
            item->appendRow(node);
        }
        if (item->rowCount() == 0)
            item->appendRow(new QStandardItem(QObject::tr("No columns")));
    });
}

void ObjectExplorer::request(QStandardItem *target, const QString &method, const QCborMap &params,
                             ResultHandler onResult)
{
    const QPersistentModelIndex index(target->index());
    const QString profileId = target->data(ConnectionIdRole).toString();
    const int generation = m_generation;
    const bool refresh = m_refreshNext;
    target->setData(true, LoadingRole);
    setPlaceholder(target, tr("Loading..."));
    ++m_pendingRequests;

    if (!m_metadata) {
        --m_pendingRequests;
        target->setData(false, LoadingRole);
        setPlaceholder(target, tr("The engine is not available."), true);
        return;
    }

    // 연결 목록을 새로 만든 뒤에 도착한 응답은 버린다 (그 항목은 이미 없다).
    QPointer<ObjectExplorer> self(this);
    m_metadata->request(
        profileId, method, params,
        [self, index, onResult, generation](const QCborMap &result, const QString &error) {
            if (!self || generation != self->m_generation)
                return;
            --self->m_pendingRequests;
            QStandardItem *item = self->itemAt(index);
            if (!item)
                return;
            item->setData(false, LoadingRole);
            if (!error.isEmpty()) {
                self->setPlaceholder(item, error, true);
                return;
            }
            item->setData(true, LoadedRole);
            item->removeRows(0, item->rowCount());
            onResult(item, result);
        },
        true, refresh);
}

// ---------------------------------------------------------------------------
// 연결 끊기

void ObjectExplorer::disconnectProfile(const QString &profileId)
{
    if (m_metadata)
        m_metadata->disconnectProfile(profileId);
    if (QStandardItem *item = findConnection(profileId)) {
        m_tree->collapse(m_proxy->mapFromSource(item->index()));
        setConnectedLook(profileId, false);
        item->setData(false, LoadedRole);
        setPlaceholder(item, tr("Expand to connect"));
    }
}

// ---------------------------------------------------------------------------
// 두 번 클릭과 오른쪽 클릭 메뉴

void ObjectExplorer::onDoubleClicked(const QModelIndex &proxyIndex)
{
    QStandardItem *item = sourceItem(proxyIndex);
    switch (kindOf(item)) {
    case ConnectionKind:
        emit connectionActivated(item->data(ConnectionIdRole).toString());
        m_tree->setExpanded(proxyIndex, !m_tree->isExpanded(proxyIndex));
        break;
    case SchemaKind:
        if (usesDatabases(item->data(ConnectionIdRole).toString())) {
            // MySQL·MariaDB: 스키마 이름 없이 조회할 수 있게 그 연결의 탭에서 USE를 실행한다.
            emit useSchemaRequested(item->data(ConnectionIdRole).toString(), item->data(SchemaRole).toString());
            break;
        }
        m_tree->setExpanded(proxyIndex, !m_tree->isExpanded(proxyIndex));
        break;
    case FolderKind:
        m_tree->setExpanded(proxyIndex, !m_tree->isExpanded(proxyIndex));
        break;
    case ObjectKind:
        if (hasColumns(item->data(TypeRole).toString()))
            emit openObjectRequested(item->data(ConnectionIdRole).toString(), item->data(SchemaRole).toString(),
                                     item->data(NameRole).toString());
        break;
    case ColumnKind:
        emit insertTextRequested(item->data(NameRole).toString());
        break;
    default:
        break;
    }
}

void ObjectExplorer::showContextMenu(const QPoint &position)
{
    QStandardItem *item = sourceItem(m_tree->indexAt(position));
    if (!item || kindOf(item) == PlaceholderKind)
        return;
    const QString connectionId = item->data(ConnectionIdRole).toString();

    QMenu menu(this);
    switch (kindOf(item)) {
    case ConnectionKind:
        menu.addAction(tr("New Query Tab"), this, [this, connectionId] { emit newQueryRequested(connectionId); });
        menu.addAction(tr("Refresh"), this, [this, item] { reload(item); });
        menu.addAction(tr("Disconnect"), this, [this, connectionId] { disconnectProfile(connectionId); })
            ->setEnabled(m_metadata && m_metadata->isConnected(connectionId));
        menu.addSeparator();
        menu.addAction(tr("Edit Connection..."), this, [this, connectionId] { emit editConnectionRequested(connectionId); });
        break;
    case SchemaKind:
        if (usesDatabases(connectionId)) {
            const QString schema = item->data(SchemaRole).toString();
            QAction *use = menu.addAction(tr("Use This Database (USE)"), this,
                                          [this, connectionId, schema] { emit useSchemaRequested(connectionId, schema); });
            QFont font = use->font();
            font.setBold(true); // 두 번 클릭과 같은 동작
            use->setFont(font);
        }
        menu.addAction(tr("New Query Tab"), this, [this, connectionId] { emit newQueryRequested(connectionId); });
        menu.addAction(tr("Refresh"), this, [this, item] { reload(item); });
        break;
    case FolderKind:
        menu.addAction(tr("New Query Tab"), this, [this, connectionId] { emit newQueryRequested(connectionId); });
        menu.addAction(tr("Refresh"), this, [this, item] { reload(item); });
        break;
    case ObjectKind: {
        const QString schema = item->data(SchemaRole).toString();
        const QString name = item->data(NameRole).toString();
        if (hasColumns(item->data(TypeRole).toString())) {
            menu.addAction(tr("Select Rows"), this,
                           [this, connectionId, schema, name] { emit openObjectRequested(connectionId, schema, name); });
        }
        menu.addAction(tr("Show DDL"), this,
                       [this, connectionId, schema, name] { emit ddlRequested(connectionId, schema, name); });
        const QString qualified = qualifiedName(item);
        menu.addAction(tr("Copy Name"), this, [qualified] { QGuiApplication::clipboard()->setText(qualified); });
        menu.addAction(tr("Insert Name into Editor"), this, [this, qualified] { emit insertTextRequested(qualified); });
        if (hasColumns(item->data(TypeRole).toString()))
            menu.addAction(tr("Refresh"), this, [this, item] { reload(item); });
        break;
    }
    case ColumnKind: {
        const QString name = item->data(NameRole).toString();
        menu.addAction(tr("Copy Name"), this, [name] { QGuiApplication::clipboard()->setText(name); });
        menu.addAction(tr("Insert Name into Editor"), this, [this, name] { emit insertTextRequested(name); });
        break;
    }
    default:
        return;
    }
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

// ---------------------------------------------------------------------------
// 도우미

void ObjectExplorer::setPlaceholder(QStandardItem *item, const QString &text, bool isError)
{
    item->removeRows(0, item->rowCount());
    auto *child = new QStandardItem(text);
    child->setData(PlaceholderKind, KindRole);
    child->setFlags(Qt::ItemIsEnabled);
    QFont font = child->font();
    font.setItalic(true);
    child->setFont(font);
    if (isError) {
        child->setForeground(QColor(0xe0, 0x6c, 0x75));
        child->setToolTip(text);
    } else {
        child->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
    }
    item->appendRow(child);
}

QStandardItem *ObjectExplorer::sourceItem(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid())
        return nullptr;
    return m_model->itemFromIndex(m_proxy->mapToSource(proxyIndex));
}

QStandardItem *ObjectExplorer::itemAt(const QPersistentModelIndex &index) const
{
    return index.isValid() ? m_model->itemFromIndex(index) : nullptr;
}

QStandardItem *ObjectExplorer::findConnection(const QString &profileId) const
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QStandardItem *item = m_model->item(row);
        if (kindOf(item) == ConnectionKind && item->data(ConnectionIdRole).toString() == profileId)
            return item;
    }
    return nullptr;
}

const ConnectionProfile *ObjectExplorer::profile(const QString &id) const
{
    for (const ConnectionProfile &profile : m_profiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

QString ObjectExplorer::qualifiedName(const QStandardItem *object) const
{
    const ConnectionProfile *connection = profile(object->data(ConnectionIdRole).toString());
    const SqlDialect dialect = connection ? sqlDialectFor(connection->type) : SqlDialect::Generic;
    return SqlIdentifiers::qualified(object->data(SchemaRole).toString(), object->data(NameRole).toString(), dialect);
}

bool ObjectExplorer::selectedObject(QString *connectionId, QString *schema, QString *name) const
{
    QStandardItem *item = sourceItem(m_tree->currentIndex());
    if (kindOf(item) != ObjectKind)
        return false;
    *connectionId = item->data(ConnectionIdRole).toString();
    *schema = item->data(SchemaRole).toString();
    *name = item->data(NameRole).toString();
    return true;
}

bool ObjectExplorer::usesDatabases(const QString &connectionId) const
{
    const ConnectionProfile *connection = profile(connectionId);
    return connection && (connection->type == DbType::MySQL || connection->type == DbType::MariaDB);
}

void ObjectExplorer::expandItem(QStandardItem *item)
{
    const QModelIndex proxyIndex = m_proxy->mapFromSource(item->index());
    if (proxyIndex.isValid())
        m_tree->expand(proxyIndex);
}
