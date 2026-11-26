#include "ClaudeCodeDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const QString kServerName = QStringLiteral("dbaro");

QString quoted(const QString &argument)
{
    return argument.contains(u' ') || argument.isEmpty() ? QStringLiteral("\"%1\"").arg(argument) : argument;
}

QPlainTextEdit *codeBox(QWidget *parent, int lines)
{
    auto *box = new QPlainTextEdit(parent);
    box->setReadOnly(true);
    box->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    box->setFont(font);
    box->setFixedHeight(box->fontMetrics().lineSpacing() * lines + 14);
    return box;
}

QPushButton *copyButton(QWidget *parent, QPlainTextEdit *source)
{
    auto *button = new QPushButton(QObject::tr("Copy"), parent);
    QObject::connect(button, &QPushButton::clicked, parent, [source, button] {
        QApplication::clipboard()->setText(source->toPlainText());
        button->setText(QObject::tr("Copied"));
        QTimer::singleShot(1500, button, [button] { button->setText(QObject::tr("Copy")); });
    });
    return button;
}

} // namespace

ClaudeCodeDialog::ClaudeCodeDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Use DBaro from Claude Code"));
    m_claude = QStandardPaths::findExecutable(QStringLiteral("claude"));

    auto *intro = new QLabel(
        tr("Claude Code and Claude Desktop can use DBaro's database tools with <b>your own Claude login</b>, without an API key. "
           "DBaro only provides the tools and runs them on this PC: list schemas and tables, describe tables, DDL, read-only "
           "queries, estimated plans, and opening SQL in a DBaro query tab (it never runs statements that change data)."),
        this);
    intro->setWordWrap(true);

    QSettings settings;
    m_production = new QCheckBox(tr("Allow queries on production connections"), this);
    m_production->setChecked(settings.value(QStringLiteral("claudeCode/allowProduction"), false).toBool());
    m_production->setToolTip(tr("Off: on production connections Claude can read structure and plans, but not run queries."));
    m_noRows = new QCheckBox(tr("Send only column names and row counts (no values)"), this);
    m_noRows->setChecked(settings.value(QStringLiteral("claudeCode/noRows"), false).toBool());
    m_maxRows = new QSpinBox(this);
    m_maxRows->setRange(1, 1000);
    m_maxRows->setValue(settings.value(QStringLiteral("claudeCode/maxRows"), 100).toInt());
    auto *rowsRow = new QHBoxLayout;
    rowsRow->addWidget(new QLabel(tr("Rows per query"), this));
    rowsRow->addWidget(m_maxRows);
    rowsRow->addStretch();
    for (QCheckBox *box : {m_production, m_noRows})
        connect(box, &QCheckBox::toggled, this, &ClaudeCodeDialog::updateTexts);
    connect(m_maxRows, &QSpinBox::valueChanged, this, &ClaudeCodeDialog::updateTexts);

    // Claude Code
    auto *codeGroup = new QGroupBox(tr("Claude Code (terminal, IDE, desktop app)"), this);
    m_command = codeBox(codeGroup, 3);
    m_add = new QPushButton(tr("Add to Claude Code"), codeGroup);
    m_add->setEnabled(!m_claude.isEmpty());
    m_add->setToolTip(m_claude.isEmpty() ? tr("The claude command was not found. Install Claude Code, or run the command yourself.")
                                         : tr("Runs the command above with %1").arg(QDir::toNativeSeparators(m_claude)));
    connect(m_add, &QPushButton::clicked, this, &ClaudeCodeDialog::addToClaudeCode);
    m_status = new QLabel(codeGroup);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *codeButtons = new QHBoxLayout;
    codeButtons->addWidget(copyButton(codeGroup, m_command));
    codeButtons->addWidget(m_add);
    codeButtons->addWidget(m_status, 1);
    auto *codeHint = new QLabel(tr("Run it once in a terminal. Then ask Claude Code, for example: \"Which tables in my dev "
                                   "connection hold orders? Write a query for last month's totals.\""),
                                codeGroup);
    codeHint->setWordWrap(true);
    codeHint->setForegroundRole(QPalette::PlaceholderText);
    auto *codeLayout = new QVBoxLayout(codeGroup);
    codeLayout->addWidget(m_command);
    codeLayout->addLayout(codeButtons);
    codeLayout->addWidget(codeHint);

    // Claude Desktop
    auto *desktopGroup = new QGroupBox(tr("Claude Desktop"), this);
    m_desktop = codeBox(desktopGroup, 9);
    auto *desktopHint = new QLabel(
        tr("Settings → Developer → Edit Config opens %1. Add the \"dbaro\" entry under \"mcpServers\", then quit and restart "
           "Claude Desktop.")
#ifdef Q_OS_MACOS
            .arg(QStringLiteral("~/Library/Application Support/Claude/claude_desktop_config.json")),
#else
            .arg(QDir::toNativeSeparators(QStringLiteral("%APPDATA%/Claude/claude_desktop_config.json"))),
#endif
        desktopGroup);
    desktopHint->setWordWrap(true);
    desktopHint->setForegroundRole(QPalette::PlaceholderText);
    auto *desktopButtons = new QHBoxLayout;
    desktopButtons->addWidget(copyButton(desktopGroup, m_desktop));
    desktopButtons->addStretch();
    auto *desktopLayout = new QVBoxLayout(desktopGroup);
    desktopLayout->addWidget(m_desktop);
    desktopLayout->addLayout(desktopButtons);
    desktopLayout->addWidget(desktopHint);

    auto *notes = new QLabel(tr("• Connections need a saved password (Edit Connection → Save password); the tools cannot ask for one.\n"
                                "• Claude asks you before it uses a tool, unless you allowed it. Queries run in a separate "
                                "read-only session with a 60-second limit.\n"
                                "• What the tools return (structure, comments, query results) goes to Claude under your "
                                "Claude account's terms. Do not use it on data you are not allowed to share."),
                             this);
    notes->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_production);
    layout->addWidget(m_noRows);
    layout->addLayout(rowsRow);
    layout->addWidget(codeGroup);
    layout->addWidget(desktopGroup);
    layout->addWidget(notes);
    layout->addWidget(buttons);
    resize(720, sizeHint().height());
    updateTexts();
}

QString ClaudeCodeDialog::cliPath()
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro-cli.exe"));
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/dbaro-cli");
#endif
}

QStringList ClaudeCodeDialog::serverArgumentsFor(bool allowProduction, bool noRows, int maxRows)
{
    QStringList arguments{QStringLiteral("mcp")};
    if (allowProduction)
        arguments << QStringLiteral("--allow-production");
    if (noRows)
        arguments << QStringLiteral("--no-rows");
    if (maxRows != 100)
        arguments << QStringLiteral("--max-rows") << QString::number(maxRows);
    return arguments;
}

QStringList ClaudeCodeDialog::savedServerArguments()
{
    QSettings settings;
    return serverArgumentsFor(settings.value(QStringLiteral("claudeCode/allowProduction"), false).toBool(),
                              settings.value(QStringLiteral("claudeCode/noRows"), false).toBool(),
                              settings.value(QStringLiteral("claudeCode/maxRows"), 100).toInt());
}

QStringList ClaudeCodeDialog::serverArguments() const
{
    return serverArgumentsFor(m_production->isChecked(), m_noRows->isChecked(), m_maxRows->value());
}

QString ClaudeCodeDialog::claudeCodeCommand() const
{
    QStringList parts{QStringLiteral("claude mcp add --transport stdio --scope user"), kServerName, QStringLiteral("--"), quoted(cliPath())};
    parts << serverArguments();
    return parts.join(u' ');
}

QString ClaudeCodeDialog::desktopConfig() const
{
    const QJsonObject server{{QStringLiteral("command"), cliPath()},
                             {QStringLiteral("args"), QJsonArray::fromStringList(serverArguments())}};
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("mcpServers"), QJsonObject{{kServerName, server}}}}).toJson(QJsonDocument::Indented))
        .trimmed();
}

void ClaudeCodeDialog::updateTexts()
{
    m_command->setPlainText(claudeCodeCommand());
    m_desktop->setPlainText(desktopConfig());
    QSettings settings;
    settings.setValue(QStringLiteral("claudeCode/allowProduction"), m_production->isChecked());
    settings.setValue(QStringLiteral("claudeCode/noRows"), m_noRows->isChecked());
    settings.setValue(QStringLiteral("claudeCode/maxRows"), m_maxRows->value());
}

void ClaudeCodeDialog::runClaude(const QStringList &arguments, std::function<void(int, const QString &)> done)
{
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [process, done](int exitCode) {
        const QString output = QString::fromUtf8(process->readAll()).trimmed();
        process->deleteLater();
        done(exitCode, output);
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        done(-1, process->errorString());
    });
    process->start(m_claude, arguments);
}

void ClaudeCodeDialog::addToClaudeCode()
{
    if (m_claude.isEmpty())
        return;
    QStringList add{QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--transport"), QStringLiteral("stdio"),
                    QStringLiteral("--scope"), QStringLiteral("user"), kServerName, QStringLiteral("--"), cliPath()};
    add << serverArguments();
    m_add->setEnabled(false);
    m_status->setText(tr("Adding..."));
    QPointer<ClaudeCodeDialog> self(this);
    runClaude(add, [self, add](int exitCode, const QString &output) {
        if (!self)
            return;
        if (exitCode == 0) {
            self->m_add->setEnabled(true);
            self->m_status->setText(tr("Added. Start a new Claude Code session to use it (/mcp shows it)."));
            return;
        }
        if (!output.contains(QLatin1String("already exists"), Qt::CaseInsensitive)) {
            self->m_add->setEnabled(true);
            self->m_status->setText(tr("Could not add it: %1").arg(output.left(300)));
            return;
        }
        // 이미 있으면 지금 옵션으로 바꿀지 묻는다
        if (QMessageBox::question(self, self->windowTitle(),
                                  tr("Claude Code already has an MCP server named \"dbaro\". Replace it with these settings?"))
            != QMessageBox::Yes) {
            self->m_add->setEnabled(true);
            self->m_status->setText(tr("Kept the existing \"dbaro\" server."));
            return;
        }
        self->runClaude({QStringLiteral("mcp"), QStringLiteral("remove"), kServerName, QStringLiteral("--scope"), QStringLiteral("user")},
                        [self, add](int, const QString &) {
                            if (!self)
                                return;
                            self->runClaude(add, [self](int code, const QString &text) {
                                if (!self)
                                    return;
                                self->m_add->setEnabled(true);
                                self->m_status->setText(code == 0 ? tr("Replaced. Start a new Claude Code session to use it.")
                                                                  : tr("Could not add it: %1").arg(text.left(300)));
                            });
                        });
    });
}
