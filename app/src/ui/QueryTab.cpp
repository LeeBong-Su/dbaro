#include "QueryTab.h"

#include "BusyIndicator.h"
#include "ConnectionBar.h"
#include "FindBar.h"
#include "MetadataService.h"
#include "SqlCompleter.h"
#include "QuerySession.h"
#include "ResultPanel.h"
#include "SqlEditor.h"
#include "ResultModel.h"
#include "core/ResultEdits.h"
#include "core/SqlIdentifiers.h"

#include <QCborArray>
#include <QFrame>
#include <QLocale>
#include <QMessageBox>
#include <QSet>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

QueryTab::QueryTab(EngineClient *engine, QWidget *parent)
    : QWidget(parent)
{
    m_connectionBar = new ConnectionBar(this);

    m_editor = new SqlEditor(this);
    m_editor->setObjectName(QStringLiteral("sqlEditor"));
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setPlaceholderText(tr("Write SQL here. Ctrl+Enter runs the current statement, F5 runs the script, "
                                    "Ctrl+E shows its estimated plan, Alt+X its actual plan."));

    m_results = new ResultPanel(this);

    // 찾기 막대는 에디터 바로 위에 붙는다 (Ctrl+F로 열 때만 보인다).
    auto *editorPane = new QWidget(this);
    m_findBar = new FindBar(m_editor, editorPane);
    auto *editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(m_findBar);
    editorLayout->addWidget(m_editor);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(editorPane);
    m_splitter->addWidget(m_results);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(2);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_connectionBar);
    layout->addWidget(m_splitter);

    connect(m_editor, &SqlEditor::cursorPositionChanged, this, [this] {
        const QTextCursor cursor = m_editor->textCursor();
        emit cursorPositionChanged(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
    });

    // 세션 → 결과 패널
    m_session = new QuerySession(engine, this);
    connect(m_session, &QuerySession::message, m_results, &ResultPanel::appendLog);
    connect(m_session, &QuerySession::serverOutput, m_results, &ResultPanel::appendOutput);
    connect(m_session, &QuerySession::resultStarted, m_results, &ResultPanel::startResult);
    connect(m_session, &QuerySession::rowsReceived, m_results, &ResultPanel::appendRows);
    connect(m_session, &QuerySession::statementFailed, this, &QueryTab::showError);
    connect(m_results, &ResultPanel::moreRowsRequested, m_session, [this] { m_session->fetchMore(); });
    connect(m_results, &ResultPanel::fetchAllRequested, m_session, [this] { m_session->fetchMore(5000); });
    connect(m_results, &ResultPanel::editRequested, this, &QueryTab::prepareEditing);
    connect(m_results, &ResultPanel::applyEditsRequested, this, &QueryTab::applyEdits);
    connect(m_session, &QuerySession::statementFinished, this, [this](const SqlStatement &statement, bool ok) {
        // 내보내기(SQL INSERT)에서 테이블 이름을 짐작할 수 있게 결과를 만든 문장을 알려 준다.
        if (ok && m_session->executionInfo().kind == ExecutionInfo::Kind::ResultSet)
            m_results->setResultSql(statement.originalText());
    });
    connect(m_session, &QuerySession::stateChanged, this, &QueryTab::updateConnectionBar);
    connect(m_session, &QuerySession::executionInfoChanged, m_results, &ResultPanel::showExecutionInfo);
    connect(m_session, &QuerySession::executionInfoChanged, this, [this](const ExecutionInfo &info) {
        // SELECT가 아닌 문장(DML, DDL, USE 등)은 결과 표가 없으니 실행 로그를 보여 준다.
        // 스크립트는 문장마다 바뀌므로 마지막 문장 기준이 된다 (SELECT면 결과 탭).
        if (info.kind == ExecutionInfo::Kind::Update || info.kind == ExecutionInfo::Kind::Other)
            m_results->showLog();
    });
    connect(m_session, &QuerySession::planReceived, m_results, &ResultPanel::showPlan);
    connect(m_session, &QuerySession::activityChanged, m_results, &ResultPanel::setActivity);
    connect(m_results, &ResultPanel::cancelRequested, m_session, &QuerySession::cancel);
    m_badge = new BusyBadge(m_editor->viewport());
    connect(m_session, &QuerySession::statementSucceeded, this, &QueryTab::onStatementSucceeded);
    connect(m_session, &QuerySession::schemaChanged, this, &QueryTab::onSchemaChanged);
    connect(m_connectionBar, &ConnectionBar::schemaChosen, this, &QueryTab::schemaSwitchRequested);
    connect(m_connectionBar, &ConnectionBar::databaseChosen, this, &QueryTab::databaseSwitchRequested);
}

int QueryTab::beginActivity(const QString &text)
{
    return m_badge->begin(text);
}

void QueryTab::endActivity(int ticket)
{
    m_badge->end(ticket);
}

QString QueryTab::currentSchema() const
{
    return m_session ? m_session->currentSchema() : QString();
}

QString QueryTab::useTarget(const QString &sql)
{
    static const QRegularExpression use(QStringLiteral("^\\s*USE\\s+(?:`((?:[^`]|``)+)`|([A-Za-z0-9_$]+))\\s*;?\\s*$"),
                                        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = use.match(sql);
    if (!match.hasMatch())
        return {};
    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1).replace(QStringLiteral("``"), QStringLiteral("`"));
}

void QueryTab::setConnectionId(const QString &id)
{
    m_connectionId = id;
    m_editor->completer()->setConnection(m_metadata, metadataId());
}

void QueryTab::setDatabase(const QString &database)
{
    if (m_database == database)
        return;
    m_database = database;
    m_editor->completer()->setConnection(m_metadata, metadataId());
    m_connectionBar->setDatabase(database);
    m_connectionBar->setSchemas({}); // 다른 DB의 스키마 목록은 다시 접속한 뒤에 받는다
}

QString QueryTab::metadataId() const
{
    return m_connectionId.isEmpty() ? QString() : Db::metadataId(m_connectionId, m_database);
}

void QueryTab::setFile(const QString &path, const TextFileFormat &format)
{
    m_filePath = path;
    m_fileFormat = format;
}

bool QueryTab::hasUnsavedFile() const
{
    return !m_filePath.isEmpty() && m_editor->document()->isModified();
}

void QueryTab::setMetadataService(MetadataService *metadata)
{
    m_metadata = metadata;
    m_editor->completer()->setConnection(metadata, metadataId());
}

void QueryTab::onStatementSucceeded(const SqlStatement &statement)
{
    // 객체를 만들거나 바꾸면 자동완성용 메타데이터를 다시 읽게 한다.
    static const QRegularExpression ddl(QStringLiteral("^\\s*(CREATE|ALTER|DROP|RENAME|TRUNCATE|COMMENT)\\b"),
                                        QRegularExpression::CaseInsensitiveOption);
    if (m_metadata && !m_connectionId.isEmpty() && ddl.match(statement.text).hasMatch()) {
        m_metadata->invalidate(metadataId());
        loadDatabaseList(); // CREATE·DROP DATABASE면 고를 수 있는 DB가 바뀐다
    }

    // 스키마를 바꾸는 문장을 직접 실행했으면 서버에 지금 스키마를 물어 막대·자동완성에 반영하고,
    // 다시 접속할 때도 그 스키마로 바꾸도록 기억한다.
    static const QRegularExpression schemaChange(
        QStringLiteral("^\\s*(?:ALTER\\s+SESSION\\s+SET\\s+CURRENT_SCHEMA\\b|USE\\s|"
                       "SET\\s+(?:SESSION\\s+|LOCAL\\s+)?(?:SEARCH_PATH|SCHEMA)\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (schemaChange.match(statement.text).hasMatch()) {
        QPointer<QueryTab> self(this);
        m_session->refreshCurrentSchema([self](const QString &schema) {
            if (self && !schema.isEmpty())
                self->m_preferredSchema = schema;
        });
    }
}

void QueryTab::onSchemaChanged(const QString &schema)
{
    m_connectionBar->setSchema(schema);
    m_editor->completer()->setCurrentSchema(schema);
}

void QueryTab::loadSchemaList()
{
    if (!m_metadata || m_connectionId.isEmpty())
        return;
    // 탐색기·자동완성과 같은 메타데이터 캐시를 쓴다 (탭 세션은 쿼리용으로 비워 둔다).
    QPointer<QueryTab> self(this);
    m_metadata->request(
        metadataId(), QStringLiteral("metaSchemas"), {},
        [self](const QCborMap &result, const QString &error) {
            if (!self || !error.isEmpty() || !self->m_session->isConnected())
                return;
            QStringList names;
            for (const QCborValue &value : result.value(QStringLiteral("names")).toArray())
                names << value.toString();
            self->m_connectionBar->setSchemas(names);
        },
        false);
}

void QueryTab::loadDatabaseList()
{
    if (!m_metadata || m_connectionId.isEmpty() || !m_canSwitchDatabase)
        return;
    // PostgreSQL: 같은 서버의 데이터베이스 (고르면 그 DB로 다시 접속한다)
    QPointer<QueryTab> self(this);
    m_metadata->request(
        metadataId(), QStringLiteral("metaDatabases"), {},
        [self](const QCborMap &result, const QString &error) {
            if (!self || !error.isEmpty() || !self->m_session->isConnected())
                return;
            QStringList names;
            for (const QCborValue &value : result.value(QStringLiteral("names")).toArray())
                names << value.toString();
            self->m_connectionBar->setDatabases(names);
        },
        false);
}

void QueryTab::setDialect(SqlDialect dialect)
{
    m_dialect = dialect;
    m_editor->setDialect(dialect);
    m_results->setDialect(dialect);
}

void QueryTab::setConnectionProfile(const ConnectionProfile *profile)
{
    m_canSwitchDatabase = profile && Db::canSwitchDatabase(profile->type);
    m_connectionBar->setProfile(profile);
    updateConnectionBar();
}

QueryTab::~QueryTab()
{
    // 세션을 먼저 정리한다. 세션은 연결을 끊으며 신호를 보내는데, 자식 위젯(연결 막대 등)이
    // 먼저 지워진 뒤에 그 신호가 이 탭에 오면 이미 없는 위젯을 건드리게 된다.
    disconnect(m_session, nullptr, this, nullptr);
    delete m_session;
    m_session = nullptr;
}

void QueryTab::updateConnectionBar()
{
    m_results->setRunning(m_session->isBusy());
    const bool connected = m_session->isConnected();
    if (connected && !m_wasConnected) {
        // 고른 스키마로 바꾸지 못했으면(지워짐, 권한 없음) 다음 접속부터는 기본 스키마를 쓴다.
        const QString current = m_session->currentSchema();
        if (!m_preferredSchema.isEmpty() && !current.isEmpty()
            && current.compare(m_preferredSchema, Qt::CaseInsensitive) != 0)
            m_preferredSchema.clear();
        loadSchemaList();
        loadDatabaseList();
    } else if (!connected && m_wasConnected) {
        m_connectionBar->setSchemas({});
        m_connectionBar->setDatabases({});
    }
    m_wasConnected = connected;
    if (connected)
        m_editor->completer()->retryMetadata(); // 이제 비밀번호를 알고 있으니 메타데이터를 가져올 수 있다
    ConnectionBar::State state = ConnectionBar::State::NotConnected;
    if (m_session->isConnecting())
        state = ConnectionBar::State::Connecting;
    else if (m_session->isConnected())
        state = m_session->isBusy() ? ConnectionBar::State::Running : ConnectionBar::State::Connected;
    m_connectionBar->setState(state, m_session->hasPendingTransaction(), m_session->serverInfo());
}

void QueryTab::showError(const SqlStatement &statement, const EngineError &error)
{
    // AI 도우미의 "Fix Error"가 쓴다
    m_lastError = QStringLiteral("```sql\n%1\n```\nError: %2").arg(statement.originalText().trimmed(), error.message.trimmed());
    // 에디터에 없는 문장(탐색기에서 보낸 USE 등)이면 로그만 보여 준다.
    if (statement.start < 0) {
        m_results->showLog();
        return;
    }
    // 오류 위치(없으면 문장 시작)로 커서를 옮기고 그 단어를 선택한다.
    QTextDocument *document = m_editor->document();
    int position = statement.start;
    if (error.position >= 0) {
        // 바인드 변수에 값을 넣었으면 오류 위치를 넣기 전 글(에디터) 기준으로 되돌린다.
        position = statement.start + statement.sourcePosition(error.position);
    } else if (error.line >= 1) {
        const QTextBlock first = document->findBlock(statement.start);
        const QTextBlock target = document->findBlockByNumber(first.blockNumber() + error.line - 1);
        if (target.isValid())
            position = qMax(statement.start, target.position());
    }
    position = qBound(0, position, qMax(0, document->characterCount() - 1));

    QTextCursor cursor(document);
    cursor.setPosition(position);
    cursor.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
    m_results->showLog();
}

void QueryTab::prepareEditing()
{
    // 결과가 테이블 하나에서 나왔는지, 그 테이블의 기본 키가 결과에 있는지 확인한다.
    const std::optional<QualifiedName> target = ResultEdits::editTarget(m_results->resultSql(), m_dialect);
    if (!target) {
        m_results->editUnavailable(tr("Only the result of a single-table query can be edited:\n"
                                      "SELECT ... FROM one table [WHERE ...] [ORDER BY ...], "
                                      "without joins, GROUP BY, DISTINCT or UNION."));
        return;
    }
    if (!m_metadata || m_connectionId.isEmpty()) {
        m_results->editUnavailable(tr("This tab has no connection."));
        return;
    }
    QCborMap params;
    if (!target->schema.isEmpty())
        params.insert(QStringLiteral("schema"), target->schema);
    params.insert(QStringLiteral("name"), target->name);
    params.insert(QStringLiteral("schemaQuoted"), target->schemaQuoted);
    params.insert(QStringLiteral("nameQuoted"), target->nameQuoted);
    QPointer<QueryTab> self(this);
    m_metadata->request(
        metadataId(), QStringLiteral("tableInfo"), params,
        [self](const QCborMap &table, const QString &error) {
            if (!self)
                return;
            if (!error.isEmpty()) {
                self->m_results->editUnavailable(tr("Could not read the table: %1").arg(error));
                return;
            }
            const QList<ResultModel::Column> &resultColumns = self->m_results->model()->columns();
            QSet<QString> tableColumns;
            for (const QCborValue &column : table.value(QStringLiteral("columns")).toArray())
                tableColumns.insert(column.toMap().value(QStringLiteral("name")).toString());
            QList<bool> editable;
            for (const ResultModel::Column &column : resultColumns)
                editable.append(tableColumns.contains(column.name));

            const QCborArray primaryKey = table.value(QStringLiteral("primaryKey")).toArray();
            if (primaryKey.isEmpty()) {
                self->m_results->editUnavailable(
                    tr("%1 has no primary key, so the changed rows cannot be found safely.")
                        .arg(table.value(QStringLiteral("name")).toString()));
                return;
            }
            QList<int> keys;
            for (const QCborValue &key : primaryKey) {
                int found = -1;
                for (int c = 0; c < resultColumns.size(); ++c) {
                    if (resultColumns[c].name == key.toString())
                        found = c;
                }
                if (found < 0) {
                    self->m_results->editUnavailable(
                        tr("Add the primary key column %1 to the SELECT list to edit this result.").arg(key.toString()));
                    return;
                }
                keys.append(found);
            }
            const QString qualified = SqlIdentifiers::qualified(table.value(QStringLiteral("schema")).toString(),
                                                                table.value(QStringLiteral("name")).toString(), self->m_dialect);
            self->m_results->beginEditing(qualified, editable, keys);
        },
        true, true);
}

void QueryTab::applyEdits(const QStringList &statements)
{
    if (!m_session->isConnected() || m_session->isBusy()) {
        QMessageBox::information(this, tr("Apply Changes"), tr("The tab is busy or not connected. Try again in a moment."));
        return;
    }
    for (const QString &sql : statements)
        m_results->appendLog(tr("SQL> %1").arg(sql));
    QPointer<QueryTab> self(this);
    const int count = int(statements.size());
    m_session->executeBatch(statements, true, [self, count](const QCborMap &result, const EngineError &error) {
        if (!self)
            return;
        const qint64 failed = result.value(QStringLiteral("failed")).toInteger();
        if (error.isError() || failed > 0) {
            QString message = error.message;
            for (const QCborValue &item : result.value(QStringLiteral("errors")).toArray())
                message = item.toMap().value(QStringLiteral("message")).toString();
            self->m_results->appendLog(tr("Changes were not applied: %1").arg(message), true);
            QMessageBox::warning(self, tr("Apply Changes"),
                                 tr("A statement failed, so the changes stay in the grid.\n%1").arg(message));
            return;
        }
        const qint64 affected = result.value(QStringLiteral("affected")).toInteger();
        self->m_results->acceptEdits();
        QString message = tr("%n statement(s) applied, %1 row(s) affected.", nullptr, count).arg(QLocale().toString(affected));
        if (affected < count)
            message += u' ' + tr("Some rows were not found (changed by someone else?).");
        if (!self->m_session->autoCommit())
            message += u' ' + tr("Not committed yet: commit to keep the changes.");
        self->m_results->appendLog(message, affected < count);
    });
}
