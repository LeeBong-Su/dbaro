#include "AiPanel.h"

#include "Icons.h"
#include "ai/AiAgent.h"
#include "ai/AiSettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// Ctrl+Enter로 보낸다 (Enter는 줄바꿈)
class InputFilter : public QObject
{
public:
    InputFilter(QObject *parent, std::function<void()> send)
        : QObject(parent)
        , m_send(std::move(send))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && (key->modifiers() & Qt::ControlModifier)) {
                m_send();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_send;
};

QString quoted(const QString &text)
{
    QStringList lines = text.trimmed().split(u'\n');
    for (QString &line : lines)
        line = QStringLiteral("> ") + line;
    return lines.join(u'\n');
}

QString shortSql(QString sql)
{
    sql = sql.simplified();
    return sql.size() > 160 ? sql.left(160) + QStringLiteral("…") : sql;
}

QString dialectName(DbType type)
{
    switch (type) {
    case DbType::Oracle: return QStringLiteral("Oracle");
    case DbType::Tibero: return QStringLiteral("Tibero (Oracle-compatible SQL and PL/SQL)");
    case DbType::MariaDB: return QStringLiteral("MariaDB");
    case DbType::MySQL: return QStringLiteral("MySQL");
    case DbType::PostgreSQL: return QStringLiteral("PostgreSQL");
    }
    return QStringLiteral("SQL");
}

} // namespace

AiPanel::AiPanel(const AiDbTools::Context &context, const Hooks &hooks, QWidget *parent)
    : QWidget(parent)
    , m_context(context)
    , m_hooks(hooks)
{
    setObjectName(QStringLiteral("aiPanel"));
    m_agent = new AiAgent(this);
    m_tools = new AiDbTools(context, this);
    m_agent->setTools(AiDbTools::definitions());
    m_agent->setToolHandler([this](const QString &name, const QJsonObject &input, AiAgent::ToolDone done) {
        m_tools->run(name, input, std::move(done));
    });

    m_model = new QLabel(this);
    m_model->setForegroundRole(QPalette::PlaceholderText);
    auto *newChat = new QToolButton(this);
    newChat->setText(tr("New Chat"));
    newChat->setToolTip(tr("Start a new conversation"));
    auto *settings = new QToolButton(this);
    settings->setText(tr("Settings..."));
    auto *claudeCode = new QToolButton(this);
    claudeCode->setText(tr("Claude Code"));
    claudeCode->setToolTip(tr("Open Claude Code in a terminal for this connection, with your Claude login (Ctrl+Alt+C)"));
    connect(newChat, &QToolButton::clicked, this, &AiPanel::newChat);
    connect(settings, &QToolButton::clicked, this, &AiPanel::settingsRequested);
    connect(claudeCode, &QToolButton::clicked, this, &AiPanel::claudeCodeRequested);
    auto *top = new QHBoxLayout;
    top->addWidget(m_model, 1);
    top->addWidget(newChat);
    top->addWidget(claudeCode);
    top->addWidget(settings);

    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setTextFormat(Qt::RichText);

    m_transcript = new QTextBrowser(this);
    m_transcript->setObjectName(QStringLiteral("aiTranscript"));
    m_transcript->setOpenLinks(false);
    m_transcript->setOpenExternalLinks(false);
    m_transcript->setPlaceholderText(tr("Ask about this database: \"Which tables hold orders?\", \"Write a query for monthly sales by "
                                        "department\", \"Why is this query slow?\""));
    connect(m_transcript, &QTextBrowser::anchorClicked, this, &AiPanel::onLink);

    auto *quickRow = new QHBoxLayout;
    const QList<QPair<QString, QString>> actions = {
        {QStringLiteral("explain"), tr("Explain SQL")},
        {QStringLiteral("optimize"), tr("Tune SQL")},
        {QStringLiteral("fix"), tr("Fix Error")},
    };
    for (const auto &[kind, label] : actions) {
        auto *button = new QPushButton(label, this);
        connect(button, &QPushButton::clicked, this, [this, kind] { quickAction(kind); });
        quickRow->addWidget(button);
        m_quick << button;
    }
    m_quick.at(0)->setToolTip(tr("Explain what the SQL in the editor does"));
    m_quick.at(1)->setToolTip(tr("Look at the plan of the SQL in the editor and suggest how to make it faster"));
    m_quick.at(2)->setToolTip(tr("Find out why the last statement of the tab failed and suggest a fix"));
    quickRow->addStretch();

    m_input = new QPlainTextEdit(this);
    m_input->setObjectName(QStringLiteral("aiInput"));
    m_input->setPlaceholderText(tr("Ask the AI assistant (Ctrl+Enter to send)"));
    m_input->setMaximumHeight(110);
    m_input->installEventFilter(new InputFilter(this, [this] { send(); }));
    m_includeSql = new QCheckBox(tr("Include the editor's SQL"), this);
    m_includeSql->setToolTip(tr("Send the selected text (or the whole editor) with the question"));
    m_send = new QPushButton(tr("Send"), this);
    connect(m_send, &QPushButton::clicked, this, [this] {
        if (m_agent->isRunning())
            m_agent->stop();
        else
            send();
    });
    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(m_includeSql, 1);
    inputRow->addWidget(m_send);
    m_status = new QLabel(this);
    m_status->setForegroundRole(QPalette::PlaceholderText);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->addLayout(top);
    layout->addWidget(m_banner);
    layout->addWidget(m_transcript, 1);
    layout->addLayout(quickRow);
    layout->addWidget(m_input);
    layout->addLayout(inputRow);
    layout->addWidget(m_status);

    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(60);
    connect(m_renderTimer, &QTimer::timeout, this, qOverload<>(&AiPanel::render));

    // 에이전트 → 대화 화면
    connect(m_agent, &AiAgent::blockStarted, this, [this](const QString &type) {
        if (type == u"text")
            m_entries.append({Entry::Assistant});
        else if (type == u"thinking")
            m_entries.append({Entry::Thinking});
        scheduleRender();
    });
    connect(m_agent, &AiAgent::textDelta, this, [this](const QString &text) {
        if (m_entries.isEmpty() || m_entries.last().kind != Entry::Assistant)
            m_entries.append({Entry::Assistant});
        m_entries.last().text += text;
        scheduleRender();
    });
    connect(m_agent, &AiAgent::thinkingDelta, this, [this](const QString &text) {
        if (m_entries.isEmpty() || m_entries.last().kind != Entry::Thinking)
            m_entries.append({Entry::Thinking});
        m_entries.last().text += text;
        scheduleRender();
    });
    connect(m_agent, &AiAgent::toolStarted, this, [this](const QString &id, const QString &name, const QJsonObject &input) {
        Entry entry{Entry::Tool};
        entry.id = id;
        entry.title = name;
        if (input.contains(QStringLiteral("sql")))
            entry.text = shortSql(input.value(QStringLiteral("sql")).toString());
        else if (input.contains(QStringLiteral("table")))
            entry.text = input.value(QStringLiteral("table")).toString();
        else if (input.contains(QStringLiteral("name")))
            entry.text = input.value(QStringLiteral("name")).toString();
        else if (input.contains(QStringLiteral("filter")))
            entry.text = input.value(QStringLiteral("filter")).toString();
        entry.detail = tr("running...");
        m_entries.append(entry);
        scheduleRender();
    });
    connect(m_agent, &AiAgent::toolFinished, this, [this](const QString &id, const QString &, const QString &result, bool isError) {
        for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
            if (m_entries[i].kind == Entry::Tool && m_entries[i].id == id) {
                m_entries[i].detail = result.section(u'\n', 0, 0).left(160);
                m_entries[i].error = isError;
                break;
            }
        }
        scheduleRender();
    });
    connect(m_tools, &AiDbTools::sqlProposed, this, [this](const QString &sql, const QString &title, const QString &explanation) {
        Entry entry{Entry::Proposal};
        entry.text = sql;
        entry.title = title.isEmpty() ? tr("Proposed SQL") : title;
        entry.detail = explanation;
        m_entries.append(entry);
        scheduleRender();
    });
    connect(m_agent, &AiAgent::fallbackUsed, this, [this](const QString &from, const QString &to) {
        m_entries.append({Entry::Notice, tr("%1 declined part of this request; %2 continued.").arg(from, to)});
        scheduleRender();
    });
    connect(m_agent, &AiAgent::finished, this, [this](const QString &, const QString &note) {
        if (!note.isEmpty())
            m_entries.append({Entry::Notice, note});
        setRunning(false);
        render();
    });
    connect(m_agent, &AiAgent::failed, this, [this](const QString &message) {
        m_entries.append({Entry::Error, message});
        setRunning(false);
        render();
    });
    connect(m_agent, &AiAgent::status, this, [this](const QString &text) {
        m_status->setText(text.isEmpty() ? tr("Tokens: %1 in (%2 from cache), %3 out").arg(m_inputTokens).arg(m_cacheRead).arg(m_outputTokens)
                                         : text);
    });
    connect(m_agent, &AiAgent::usage, this, [this](const QJsonObject &usage, const QString &model) {
        m_inputTokens += usage.value(QStringLiteral("input_tokens")).toInteger() + usage.value(QStringLiteral("cache_read_input_tokens")).toInteger()
                         + usage.value(QStringLiteral("cache_creation_input_tokens")).toInteger();
        m_cacheRead += usage.value(QStringLiteral("cache_read_input_tokens")).toInteger();
        m_outputTokens += usage.value(QStringLiteral("output_tokens")).toInteger();
        if (!model.isEmpty())
            m_model->setText(AiSettings::modelName(model));
    });
    refreshBanner();
}

void AiPanel::refreshBanner()
{
    const AiSettings settings = AiSettings::load();
    m_model->setText(AiSettings::modelName(settings.model));
    QStringList notes;
    if (AiSettings::apiKey().isEmpty())
        notes << tr("Enter an Anthropic API key in <b>Settings...</b> to start. Have a Claude subscription instead? "
                     "Use DBaro from Claude Code: <b>Database → Use with Claude Code...</b>");
    const ConnectionProfile profile = m_context.profile ? m_context.profile() : ConnectionProfile();
    if (profile.environment == Environment::Production)
        notes << tr("<b>Production database</b>: every query the assistant runs needs your approval.");
    if (!settings.sendRows)
        notes << tr("Query results are not sent to the AI (Settings).");
    m_banner->setText(notes.join(QStringLiteral("<br>")));
    m_banner->setVisible(!notes.isEmpty());
}

QString AiPanel::systemPrompt() const
{
    const ConnectionProfile profile = m_context.profile ? m_context.profile() : ConnectionProfile();
    const AiSettings settings = AiSettings::load();
    const QString server = m_hooks.serverInfo ? m_hooks.serverInfo() : QString();
    const QString schema = m_context.currentSchema ? m_context.currentSchema() : QString();
    QString prompt = QStringLiteral(
        "You are the database assistant inside DBaro, a SQL IDE. The user is connected to a %1 database%2 "
        "(connection \"%3\"%4). The current schema of the user's query tab is %5.\n\n"
        "How to work:\n"
        "- Look before you write SQL: use list_tables, describe_table and get_ddl to learn the real table and column names, "
        "keys and comments (comments are often in Korean and explain what columns mean). Do not guess names.\n"
        "- run_query runs one read-only statement and returns at most %6 rows. Use it to check data or to test a query before "
        "you give it to the user. Keep queries light on big tables (filter, aggregate, limit rows).\n"
        "- Never try to change data or structure. For INSERT, UPDATE, DELETE, MERGE, DDL or anything the user should run "
        "and keep, call propose_sql: the user sees it with a button to open it in a query tab.\n"
        "- For slow queries, use explain_query and the table's indexes before suggesting changes.\n"
        "- Write SQL for %1 (its dialect and functions). Show SQL in ```sql code blocks.\n"
        "- Answer in the language the user writes in. Be brief: lead with the answer, then the SQL.")
                         .arg(dialectName(profile.type),
                              server.isEmpty() ? QString() : QStringLiteral(" running %1").arg(server.section(u'\n', 0, 0)),
                              profile.name,
                              profile.environment == Environment::Production ? QStringLiteral(", a PRODUCTION database") : QString(),
                              schema.isEmpty() ? QStringLiteral("unknown") : schema)
                         .arg(settings.maxRows);
    if (!settings.sendRows)
        prompt += QStringLiteral("\n- The user does not share row values: run_query returns only column names and row counts.");
    return prompt;
}

bool AiPanel::ensureReady()
{
    AiSettings settings = AiSettings::load();
    if (AiSettings::apiKey().isEmpty()) {
        emit settingsRequested();
        return !AiSettings::apiKey().isEmpty();
    }
    if (!settings.consented) {
        const auto answer = QMessageBox::question(
            this, tr("AI Assistant"),
            tr("The AI assistant sends your questions to Anthropic's Claude API (%1), together with:\n\n"
               "• the database type and version, and the connection name\n"
               "• table, column and index definitions and comments it reads\n"
               "• the editor's SQL and error messages when you include them\n"
               "• results of the read-only queries it runs (up to %2 rows each)%3\n\n"
               "Do not use it on data you are not allowed to share. Continue?")
                .arg(settings.baseUrl, QString::number(settings.maxRows),
                     settings.sendRows ? QString() : tr(" - turned off in Settings: only column names and row counts")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return false;
        settings.consented = true;
        settings.save();
    }
    return true;
}

void AiPanel::send()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    QString question = text;
    if (m_includeSql->isChecked() && m_hooks.editorSql) {
        const QString sql = m_hooks.editorSql().trimmed();
        if (!sql.isEmpty())
            question += QStringLiteral("\n\nSQL in my editor:\n```sql\n%1\n```").arg(sql);
    }
    m_input->clear();
    ask(question);
}

void AiPanel::quickAction(const QString &kind)
{
    const QString sql = m_hooks.editorSql ? m_hooks.editorSql().trimmed() : QString();
    if (kind == u"fix") {
        const QString error = m_hooks.lastError ? m_hooks.lastError() : QString();
        if (error.isEmpty()) {
            m_status->setText(tr("No failed statement in this tab."));
            return;
        }
        ask(QStringLiteral("This statement failed. Find the cause (check the tables and columns it uses) and give me a fixed version.\n\n%1").arg(error));
        return;
    }
    if (sql.isEmpty()) {
        m_status->setText(tr("The editor is empty: write or select SQL first."));
        return;
    }
    if (kind == u"explain")
        ask(QStringLiteral("Explain what this SQL does, step by step, in plain words.\n\n```sql\n%1\n```").arg(sql));
    else
        ask(QStringLiteral("Review this SQL for performance: look at its estimated plan and the indexes of the tables, then suggest "
                           "concrete changes (rewrite or index) and why.\n\n```sql\n%1\n```").arg(sql));
}

void AiPanel::ask(const QString &question)
{
    if (m_agent->isRunning() || !ensureReady())
        return;
    const AiSettings settings = AiSettings::load();
    m_agent->setConfig(settings.config());
    if (m_newConversation) {
        // 대화 동안은 바꾸지 않는다 (앞부분이 같아야 캐시가 맞는다)
        m_agent->setSystemPrompt(systemPrompt());
        m_newConversation = false;
    }
    m_entries.append({Entry::User, question});
    setRunning(true);
    render();
    m_agent->ask(question);
}

void AiPanel::newChat()
{
    m_agent->reset();
    m_tools->disconnectSession();
    m_entries.clear();
    m_newConversation = true;
    m_inputTokens = m_outputTokens = m_cacheRead = 0;
    m_status->clear();
    refreshBanner();
    render();
}

void AiPanel::setRunning(bool running)
{
    m_send->setText(running ? tr("Stop") : tr("Send"));
    for (QPushButton *button : std::as_const(m_quick))
        button->setEnabled(!running);
}

void AiPanel::scheduleRender()
{
    if (!m_renderTimer->isActive())
        m_renderTimer->start();
}

QString AiPanel::render(const Entry &entry, QStringList *snippets) const
{
    auto withLinks = [snippets](const QString &markdown) {
        // ```sql 블록 뒤에 "쿼리 탭에서 열기·복사" 링크를 단다
        static const QRegularExpression fence(QStringLiteral("```(?:sql|SQL|plsql)?[ \\t]*\\n([\\s\\S]*?)```"));
        QString out;
        qsizetype last = 0;
        auto it = fence.globalMatch(markdown);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += markdown.mid(last, m.capturedEnd() - last);
            const qsizetype index = snippets->size();
            snippets->append(m.captured(1).trimmed());
            out += QStringLiteral("\n\n[Open in query tab](dbaro-sql:%1) · [Copy](dbaro-copy:%1)\n").arg(index);
            last = m.capturedEnd();
        }
        out += markdown.mid(last);
        return out;
    };
    switch (entry.kind) {
    case Entry::User:
        return QStringLiteral("**%1**\n\n%2").arg(tr("You"), quoted(entry.text));
    case Entry::Assistant:
        return withLinks(entry.text);
    case Entry::Thinking: {
        QString summary = entry.text.simplified();
        if (summary.isEmpty())
            return {};
        if (summary.size() > 300)
            summary = summary.left(300) + QStringLiteral("…");
        return QStringLiteral("*%1: %2*").arg(tr("Thinking"), summary.replace(u'*', u' '));
    }
    case Entry::Tool:
        return QStringLiteral("`⚙ %1`%2 — %3%4")
            .arg(entry.title, entry.text.isEmpty() ? QString() : QStringLiteral(" `%1`").arg(QString(entry.text).replace(u'`', u'\'')),
                 entry.error ? QStringLiteral("**") : QString(), entry.detail + (entry.error ? QStringLiteral("**") : QString()));
    case Entry::Proposal:
        return withLinks(QStringLiteral("**%1**\n\n%2%3```sql\n%4\n```")
                             .arg(entry.title, entry.detail, entry.detail.isEmpty() ? QString() : QStringLiteral("\n\n"), entry.text));
    case Entry::Notice:
        return QStringLiteral("*%1*").arg(entry.text);
    case Entry::Error:
        return QStringLiteral("**%1** %2").arg(tr("Error:"), entry.text);
    }
    return {};
}

QString AiPanel::transcriptMarkdown() const
{
    QStringList snippets;
    QStringList parts;
    for (const Entry &entry : m_entries) {
        if (entry.kind == Entry::User && !parts.isEmpty())
            parts << QStringLiteral("---");
        const QString part = render(entry, &snippets);
        if (!part.isEmpty())
            parts << part;
    }
    return parts.join(QStringLiteral("\n\n"));
}

void AiPanel::render()
{
    QScrollBar *bar = m_transcript->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    m_snippets.clear();
    QStringList snippets;
    QStringList parts;
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.kind == Entry::User && !parts.isEmpty())
            parts << QStringLiteral("---");
        const QString part = render(entry, &snippets);
        if (!part.isEmpty())
            parts << part;
    }
    m_snippets = snippets;
    m_transcript->setMarkdown(parts.join(QStringLiteral("\n\n")));
    if (atBottom)
        bar->setValue(bar->maximum());
}

void AiPanel::onLink(const QUrl &url)
{
    const int index = url.path().toInt();
    if (index < 0 || index >= m_snippets.size())
        return;
    const QString sql = m_snippets.at(index);
    if (url.scheme() == u"dbaro-sql" && m_hooks.openSql)
        m_hooks.openSql(sql, tr("AI SQL"));
    else if (url.scheme() == u"dbaro-copy")
        QApplication::clipboard()->setText(sql);
}
