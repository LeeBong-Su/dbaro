#include "SqlCompleter.h"

#include "Icons.h"
#include "MetadataService.h"
#include "SqlEditor.h"
#include "core/SqlIdentifiers.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCborArray>
#include <QCompleter>
#include <QKeyEvent>
#include <QListView>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextBlock>

#include <algorithm>

namespace {

enum Role { InsertRole = Qt::UserRole + 1, DetailRole, CaretBackRole };
enum Kind { ColumnKind, KeyColumnKind, AliasKind, TableKind, ViewKind, SchemaKind, FunctionKind, RoutineKind, ObjectKind, KeywordKind };

constexpr int kMaxItems = 300;
constexpr qint64 kRetryAfterMs = 30'000;

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'#';
}

QIcon iconFor(int kind)
{
    switch (kind) {
    case ColumnKind: return Icons::get(QStringLiteral("column"));
    case KeyColumnKind: return Icons::get(QStringLiteral("key"));
    case AliasKind:
    case TableKind: return Icons::get(QStringLiteral("table"));
    case ViewKind: return Icons::get(QStringLiteral("view"));
    case SchemaKind: return Icons::get(QStringLiteral("schema"));
    case FunctionKind:
    case RoutineKind: return Icons::get(QStringLiteral("routine"));
    case ObjectKind: return Icons::get(QStringLiteral("object"));
    default: return {};
    }
}

// 이름 뒤에 보조 정보(타입·테이블)를 흐리게 그린다.
class CompletionDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        // 목록의 현재 항목을 선택된 모양으로 그린다 (QCompleter 기본 델리게이트가 하던 일).
        QStyleOptionViewItem base = option;
        if (auto *view = qobject_cast<const QAbstractItemView *>(parent()); view && view->currentIndex() == index)
            base.state |= QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, base, index);
        const QString detail = index.data(DetailRole).toString();
        if (detail.isEmpty())
            return;
        QStyleOptionViewItem opt = base;
        initStyleOption(&opt, index);
        // 이름 바로 뒤부터 오른쪽 끝까지, 오른쪽 정렬
        const int textEnd = opt.rect.left() + opt.decorationSize.width() + 12 + opt.fontMetrics.horizontalAdvance(opt.text);
        const int left = textEnd + 16;
        const QRect rect(left, opt.rect.top(), opt.rect.right() - 8 - left, opt.rect.height());
        if (rect.width() < 20)
            return;
        painter->save();
        const bool selected = opt.state & QStyle::State_Selected;
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(rect, Qt::AlignRight | Qt::AlignVCenter, opt.fontMetrics.elidedText(detail, Qt::ElideRight, rect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const QString detail = index.data(DetailRole).toString();
        if (!detail.isEmpty())
            size.rwidth() += option.fontMetrics.horizontalAdvance(detail) + 32;
        size.setHeight(qMax(size.height(), option.fontMetrics.height() + 8));
        return size;
    }
};

// 키워드·함수는 입력한 글자가 모두 소문자면 소문자로 넣는다.
QString matchCase(const QString &word, const QString &prefix)
{
    if (!prefix.isEmpty() && prefix == prefix.toLower() && prefix != prefix.toUpper())
        return word.toLower();
    return word;
}

// 0: 접두어로 시작, 1: 중간에 포함, -1: 맞지 않음
int matchClass(const QString &candidate, const QString &prefix)
{
    if (prefix.isEmpty() || candidate.startsWith(prefix, Qt::CaseInsensitive))
        return 0;
    if (prefix.size() >= 2 && candidate.contains(prefix, Qt::CaseInsensitive))
        return 1;
    return -1;
}

bool withoutParentheses(const QString &function)
{
    static const QSet<QString> names{QStringLiteral("CURRENT_DATE"), QStringLiteral("CURRENT_TIMESTAMP"),
                                     QStringLiteral("SYSDATE"), QStringLiteral("SYSTIMESTAMP"), QStringLiteral("ROWNUM")};
    return names.contains(function);
}

QCborMap params(std::initializer_list<std::pair<QString, QString>> items)
{
    QCborMap map;
    for (const auto &[key, value] : items)
        map.insert(key, value);
    return map;
}

} // namespace

SqlCompleter::SqlCompleter(SqlEditor *editor)
    : QObject(editor)
    , m_editor(editor)
{
    m_model = new QStandardItemModel(this);
    m_completer = new QCompleter(m_model, this);
    m_completer->setWidget(editor);
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setMaxVisibleItems(12);
    m_completer->setWrapAround(false);

    auto *list = new QListView;
    list->setObjectName(QStringLiteral("completionPopup"));
    list->setUniformItemSizes(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_completer->setPopup(list);
    list->setItemDelegate(new CompletionDelegate(list)); // setPopup이 기본 델리게이트로 바꾸므로 그 뒤에 둔다

    connect(m_completer, qOverload<const QModelIndex &>(&QCompleter::activated), this, &SqlCompleter::insert);
}

void SqlCompleter::setConnection(MetadataService *metadata, const QString &connectionId)
{
    if (m_metadata == metadata && m_connectionId == connectionId)
        return;
    if (m_metadata)
        disconnect(m_metadata, nullptr, this, nullptr);
    m_metadata = metadata;
    m_connectionId = connectionId;
    m_currentSchema.clear();
    m_failedAt.invalidate();
    hide();
    if (m_metadata) {
        connect(m_metadata, &MetadataService::loaded, this, [this](const QString &profileId) {
            // 기다리던 메타데이터가 오면 목록을 새로 그린다 (목록이 열려 있거나 열려던 참일 때만).
            // 그 사이 사용자가 앱 안의 다른 곳으로 옮겨 갔으면 띄우지 않는다.
            const bool editing = m_editor->hasFocus() || (!QApplication::focusWidget() && m_editor->isVisible());
            if (profileId == m_connectionId && (isPopupVisible() || m_waitingForMetadata) && editing)
                update(m_explicit);
        });
    }
}

void SqlCompleter::retryMetadata()
{
    m_failedAt.invalidate();
}

void SqlCompleter::setCurrentSchema(const QString &schema)
{
    m_currentSchema = schema;
}

bool SqlCompleter::isPopupVisible() const
{
    return m_completer->popup()->isVisible();
}

QAbstractItemView *SqlCompleter::popup() const
{
    return m_completer->popup();
}

QStringList SqlCompleter::suggestions() const
{
    QStringList texts;
    for (int row = 0; row < m_model->rowCount(); ++row)
        texts << m_model->item(row)->text();
    return texts;
}

void SqlCompleter::hide()
{
    m_explicit = false;
    m_waitingForMetadata = false;
    m_completer->popup()->hide();
}

bool SqlCompleter::keyPressBefore(QKeyEvent *event)
{
    if (!isPopupVisible())
        return false;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Escape:
        // 목록(QCompleter)이 처리한다: Enter/Tab은 넣기, Esc는 닫기
        event->ignore();
        return true;
    default:
        return false;
    }
}

void SqlCompleter::keyPressAfter(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_Hangul:
    case Qt::Key_Hangul_Hanja:
        return;
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
        if (isPopupVisible())
            update(m_explicit);
        return;
    default:
        break;
    }

    const QString text = event->text();
    const bool typing = !text.isEmpty() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
                        && (isWordChar(text.at(0)) || text.at(0) == u'.' || text.at(0) == u'"' || text.at(0) == u'`');
    if (typing)
        update(m_explicit && isPopupVisible());
    else
        hide();
}

void SqlCompleter::trigger(bool explicitRequest)
{
    update(explicitRequest);
}

void SqlCompleter::update(bool explicitRequest)
{
    if (m_editor->hasExtraCursors() || m_editor->textCursor().hasSelection()) {
        hide();
        return;
    }

    // 커서가 있는 문장(없으면 앞 문장 끝부터 커서까지)을 분석한다.
    const int cursor = m_editor->textCursor().position();
    const QString document = m_editor->toPlainText();
    int start = 0;
    int end = cursor;
    for (const SqlStatement &statement : m_editor->statements()) {
        if (statement.start <= cursor && cursor <= statement.end) {
            start = statement.start;
            end = qMax(statement.end, cursor);
            break;
        }
        if (statement.terminatorEnd <= cursor)
            start = qMax(start, statement.terminatorEnd);
    }
    m_regionStart = start;
    m_context = SqlCompletion::analyze(document.mid(start, end - start), cursor - start, m_editor->dialect());

    const SqlCompletionContext &context = m_context;
    const bool wanted = explicitRequest || isPopupVisible() || context.prefix.size() >= 2 || !context.qualifier.isEmpty()
                        || (context.kind == SqlCompletionContext::Kind::Table && !context.prefix.isEmpty());
    if (context.kind == SqlCompletionContext::Kind::None || !wanted
        || (!explicitRequest && context.prefix.isEmpty() && context.qualifier.isEmpty())) {
        hide();
        return;
    }

    m_waitingForMetadata = false;
    const QList<Item> items = collect(context);
    m_explicit = explicitRequest;

    // 이미 다 입력한 단어 하나만 남았으면 굳이 띄우지 않는다.
    const bool onlyTyped = items.size() == 1 && items.first().text.compare(context.prefix, Qt::CaseInsensitive) == 0;
    if (items.isEmpty() || (onlyTyped && !explicitRequest)) {
        const bool keepWaiting = m_waitingForMetadata; // 메타데이터가 오면 다시 그린다
        m_completer->popup()->hide();
        m_waitingForMetadata = keepWaiting;
        return;
    }

    m_model->clear();
    for (const Item &item : items) {
        auto *row = new QStandardItem(iconFor(item.kind), item.text);
        row->setData(item.insert, InsertRole);
        row->setData(item.detail, DetailRole);
        row->setData(item.caretBack, CaretBackRole);
        row->setEditable(false);
        m_model->appendRow(row);
    }
    m_completer->setCompletionPrefix(QString());

    QAbstractItemView *list = m_completer->popup();
    QTextCursor at = m_editor->textCursor();
    at.setPosition(m_regionStart + context.prefixStart);
    QRect rect = m_editor->cursorRect(at);
    // 목록 폭: 가장 긴 "이름 + 보조 정보"에 맞춘다 (항목 크기가 같다고 보는 목록이라 직접 잰다).
    const QFontMetrics metrics(list->font());
    int widest = 0;
    for (const Item &item : items) {
        const int detail = item.detail.isEmpty() ? 0 : metrics.horizontalAdvance(item.detail) + 32;
        widest = qMax(widest, metrics.horizontalAdvance(item.text) + detail);
    }
    const int chrome = list->iconSize().width() + 40 + list->verticalScrollBar()->sizeHint().width();
    const int width = qBound(260, widest + chrome, 640);
    rect.setWidth(width);
    rect.translate(m_editor->viewport()->pos());
    m_completer->complete(rect);
    list->setCurrentIndex(m_completer->completionModel()->index(0, 0));
}

void SqlCompleter::insert(const QModelIndex &index)
{
    const QString text = index.data(InsertRole).toString();
    if (text.isEmpty())
        return;
    const int caretBack = index.data(CaretBackRole).toInt();
    QTextCursor cursor = m_editor->textCursor();
    const int end = cursor.position();
    cursor.beginEditBlock();
    cursor.setPosition(m_regionStart + m_context.prefixStart);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    // 닫는 따옴표가 이미 있으면 그것까지 바꾼다 ("Mix|ed" 처럼 따옴표 안에서 고른 경우)
    if (m_context.quoted) {
        const QString document = m_editor->toPlainText();
        int stop = end;
        while (stop < document.size() && document.at(stop) != u'"' && document.at(stop) != u'`' && !document.at(stop).isSpace())
            ++stop;
        if (stop < document.size() && (document.at(stop) == u'"' || document.at(stop) == u'`'))
            cursor.setPosition(stop + 1, QTextCursor::KeepAnchor);
    }
    cursor.insertText(text);
    cursor.setPosition(cursor.position() - caretBack);
    cursor.endEditBlock();
    m_editor->setTextCursor(cursor);
    hide();
}

// ---------------------------------------------------------------------------
// 제안 모으기

QList<SqlCompleter::Item> SqlCompleter::collect(const SqlCompletionContext &context)
{
    QList<Item> items;
    using Kind = SqlCompletionContext::Kind;

    if (!context.qualifier.isEmpty()) {
        if (!context.outerQualifier.isEmpty()) {
            addColumns(items, context.outerQualifier, context.qualifier, 0); // schema.table.|
        } else if (context.kind == Kind::Table) {
            addObjects(items, context.qualifier, 0); // FROM schema.|
        } else if (const SqlTableRef *ref = SqlCompletion::findTable(context, context.qualifier)) {
            addColumns(items, ref->schema, ref->name, 0); // alias.|
        } else {
            // 문장에 없는 이름: 현재 스키마의 테이블이면 그 컬럼, 아니면 스키마로 보고 그 안의 객체
            const QString table = actualTable(QString(), context.qualifier);
            if (!table.isEmpty())
                addColumns(items, QString(), table, 0);
            else if (!actualSchema(context.qualifier).isEmpty())
                addObjects(items, context.qualifier, 0);
        }
    } else if (context.kind == Kind::Table) {
        addObjects(items, QString(), 0);
        for (const QString &schema : schemas())
            items.append({schema, SqlIdentifiers::quote(schema, m_editor->dialect()), tr("schema"), SchemaKind, 1, 0});
    } else if (context.kind == Kind::Column) {
        QSet<QString> seen;
        for (const SqlTableRef &ref : context.tables) {
            addColumns(items, ref.schema, ref.name, 0);
            const QString label = ref.alias.isEmpty() ? ref.name : ref.alias;
            if (!seen.contains(label.toUpper())) {
                seen.insert(label.toUpper());
                items.append({label, label, ref.alias.isEmpty() ? tr("table") : ref.name, AliasKind, 1, 0});
            }
        }
        addFunctions(items, context.prefix, 2);
        addKeywords(items, context.prefix, 3);
    } else {
        addKeywords(items, context.prefix, 0);
    }

    // 접두어로 거르고 정렬한다: 접두어로 시작하는 것 → 문맥별 순서 → 이름
    struct Ranked
    {
        Item item;
        int match;
    };
    QList<Ranked> ranked;
    QSet<QString> unique;
    for (const Item &item : std::as_const(items)) {
        const int match = matchClass(item.text, context.prefix);
        if (match < 0)
            continue;
        const QString key = QString::number(item.kind) + u'|' + item.text.toUpper() + u'|' + item.detail;
        if (unique.contains(key))
            continue;
        unique.insert(key);
        ranked.append({item, match});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) {
        if (a.match != b.match)
            return a.match < b.match;
        if (a.item.group != b.item.group)
            return a.item.group < b.item.group;
        return a.item.text.compare(b.item.text, Qt::CaseInsensitive) < 0;
    });

    QList<Item> result;
    for (const Ranked &entry : std::as_const(ranked)) {
        result.append(entry.item);
        if (result.size() >= kMaxItems)
            break;
    }
    return result;
}

void SqlCompleter::addKeywords(QList<Item> &items, const QString &prefix, int group)
{
    for (const QString &keyword : SqlCompletion::keywords())
        items.append({keyword, matchCase(keyword, prefix), QString(), KeywordKind, group, 0});
}

void SqlCompleter::addFunctions(QList<Item> &items, const QString &prefix, int group)
{
    for (const QString &function : SqlCompletion::functions(m_editor->dialect())) {
        const QString name = matchCase(function, prefix);
        if (withoutParentheses(function))
            items.append({function, name, tr("function"), FunctionKind, group, 0});
        else
            items.append({function, name + QStringLiteral("()"), tr("function"), FunctionKind, group, 1});
    }
}

void SqlCompleter::addObjects(QList<Item> &items, const QString &schemaName, int group)
{
    const QString schema = schemaName.isEmpty() ? currentSchema() : actualSchema(schemaName);
    if (schema.isEmpty())
        return;
    const std::optional<QCborMap> result = fetch(QStringLiteral("metaObjects"), params({{QStringLiteral("schema"), schema}}));
    if (!result)
        return;
    const SqlDialect dialect = m_editor->dialect();
    for (const QCborValue &value : result->value(QStringLiteral("objects")).toArray()) {
        const QCborMap object = value.toMap();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QString type = object.value(QStringLiteral("type")).toString().toUpper();
        int kind = ObjectKind;
        if (type.contains(u"VIEW"))
            kind = ViewKind;
        else if (type.contains(u"TABLE"))
            kind = TableKind;
        else if (type == u"FUNCTION" || type == u"PROCEDURE")
            kind = RoutineKind;
        const QString remarks = object.value(QStringLiteral("remarks")).toString().trimmed();
        const QString detail = remarks.isEmpty() ? type.toLower() : remarks;
        items.append({name, SqlIdentifiers::quote(name, dialect), detail, kind, group, 0});
    }
}

void SqlCompleter::addColumns(QList<Item> &items, const QString &schemaName, const QString &tableName, int group)
{
    const QString schema = schemaName.isEmpty() ? currentSchema() : actualSchema(schemaName);
    if (schema.isEmpty())
        return;
    const QString table = actualTable(schema, tableName);
    if (table.isEmpty())
        return;
    const std::optional<QCborMap> result = fetch(
        QStringLiteral("metaColumns"), params({{QStringLiteral("schema"), schema}, {QStringLiteral("table"), table}}));
    if (!result)
        return;
    const SqlDialect dialect = m_editor->dialect();
    for (const QCborValue &value : result->value(QStringLiteral("columns")).toArray()) {
        const QCborMap column = value.toMap();
        const QString name = column.value(QStringLiteral("name")).toString();
        const bool key = column.value(QStringLiteral("primaryKey")).toBool();
        const QString detail = QStringLiteral("%1 · %2").arg(column.value(QStringLiteral("type")).toString(), table);
        items.append({name, SqlIdentifiers::quote(name, dialect), detail, key ? KeyColumnKind : ColumnKind, group, 0});
    }
}

// ---------------------------------------------------------------------------
// 메타데이터 (캐시에서 꺼내고, 없으면 뒤에서 가져온다)

bool SqlCompleter::metadataAvailable()
{
    if (!m_metadata || m_connectionId.isEmpty())
        return false;
    return !(m_failedAt.isValid() && m_failedAt.elapsed() < kRetryAfterMs);
}

std::optional<QCborMap> SqlCompleter::fetch(const QString &method, const QCborMap &requestParams)
{
    if (!metadataAvailable())
        return std::nullopt;
    if (std::optional<QCborMap> hit = m_metadata->cached(m_connectionId, method, requestParams))
        return hit;
    m_waitingForMetadata = true;
    if (!m_metadata->isLoading(m_connectionId, method, requestParams)) {
        QPointer<SqlCompleter> self(this);
        m_metadata->request(
            m_connectionId, method, requestParams,
            [self](const QCborMap &, const QString &error) {
                if (self && !error.isEmpty()) {
                    self->m_failedAt.start(); // 접속할 수 없으면 한동안 키워드만 제안한다
                    self->m_waitingForMetadata = false;
                }
            },
            false);
    }
    // 위 요청이 캐시에서 바로 끝났을 수도 있다
    return m_metadata->cached(m_connectionId, method, requestParams);
}

QStringList SqlCompleter::schemas()
{
    QStringList names;
    if (const std::optional<QCborMap> result = fetch(QStringLiteral("metaSchemas"), {})) {
        for (const QCborValue &value : result->value(QStringLiteral("names")).toArray())
            names << value.toString();
    }
    return names;
}

QString SqlCompleter::currentSchema()
{
    if (!m_currentSchema.isEmpty())
        return actualSchema(m_currentSchema);
    if (const std::optional<QCborMap> result = fetch(QStringLiteral("metaSchemas"), {}))
        return result->value(QStringLiteral("current")).toString();
    return {};
}

QString SqlCompleter::actualSchema(const QString &name)
{
    // 저장된 대소문자로 맞춘다 (Oracle은 대문자, PostgreSQL은 소문자로 저장되는 경우가 많다).
    const QStringList names = schemas();
    for (const QString &schema : names) {
        if (schema.compare(name, Qt::CaseInsensitive) == 0)
            return schema;
    }
    return {}; // 목록에 없거나 아직 모름
}

QString SqlCompleter::actualTable(const QString &schemaName, const QString &name)
{
    const QString schema = schemaName.isEmpty() ? currentSchema() : actualSchema(schemaName);
    if (schema.isEmpty())
        return {};
    const std::optional<QCborMap> result = fetch(QStringLiteral("metaObjects"), params({{QStringLiteral("schema"), schema}}));
    if (!result)
        return {};
    for (const QCborValue &value : result->value(QStringLiteral("objects")).toArray()) {
        const QString object = value.toMap().value(QStringLiteral("name")).toString();
        if (object.compare(name, Qt::CaseInsensitive) == 0)
            return object;
    }
    return {};
}
