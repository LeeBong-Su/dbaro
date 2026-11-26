#include "AiSettingsDialog.h"

#include "ai/AiSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

AiSettingsDialog::AiSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AI Assistant Settings"));
    const AiSettings settings = AiSettings::load();

    m_key = new QLineEdit(this);
    m_key->setObjectName(QStringLiteral("aiApiKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(AiSettings::hasStoredApiKey() ? tr("Saved (type to replace)") : tr("sk-ant-..."));
    m_keyState = new QLabel(this);
    m_keyState->setForegroundRole(QPalette::PlaceholderText);
    m_keyState->setWordWrap(true);
    if (AiSettings::hasStoredApiKey())
#ifdef Q_OS_MACOS
        m_keyState->setText(tr("A key is saved in the login keychain."));
#else
        m_keyState->setText(tr("A key is saved in Windows Credential Manager (DBaro/connection/ai-anthropic)."));
#endif
    else if (!qEnvironmentVariable("ANTHROPIC_API_KEY").isEmpty())
        m_keyState->setText(tr("Using the ANTHROPIC_API_KEY environment variable."));
    else
#ifdef Q_OS_MACOS
        m_keyState->setText(tr("Create a key at console.anthropic.com. It is kept in the login keychain, not in a file."));
#else
        m_keyState->setText(tr("Create a key at console.anthropic.com. It is kept in Windows Credential Manager, not in a file."));
#endif
    auto *remove = new QPushButton(tr("Remove"), this);
    remove->setEnabled(AiSettings::hasStoredApiKey());
    connect(remove, &QPushButton::clicked, this, [this, remove] {
        AiSettings::removeApiKey();
        remove->setEnabled(false);
        m_key->setPlaceholderText(tr("sk-ant-..."));
        m_keyState->setText(tr("The saved key was removed."));
    });
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(remove);

    m_model = new QComboBox(this);
    for (const auto &[id, name] : AiSettings::models())
        m_model->addItem(name, id);
    m_model->setCurrentIndex(qMax(0, m_model->findData(settings.model)));
    m_effort = new QComboBox(this);
    m_effort->addItems(AiSettings::efforts());
    m_effort->setCurrentText(settings.effort);
    m_effort->setToolTip(tr("How hard the model thinks. Higher is more thorough but slower and uses more tokens."));
    m_baseUrl = new QLineEdit(settings.baseUrl, this);
    m_baseUrl->setToolTip(tr("Change only for an Anthropic-compatible gateway or proxy your company provides."));

    m_sendRows = new QCheckBox(tr("Send query results to the AI (otherwise only column names and row counts)"), this);
    m_sendRows->setChecked(settings.sendRows);
    m_maxRows = new QSpinBox(this);
    m_maxRows->setRange(1, 1000);
    m_maxRows->setValue(settings.maxRows);
    m_ask = new QCheckBox(tr("Ask before every query the AI runs (always on for production connections)"), this);
    m_ask->setChecked(settings.askBeforeQuery);

    auto *form = new QFormLayout;
    form->addRow(tr("Anthropic API key"), keyRow);
    form->addRow(QString(), m_keyState);
    form->addRow(tr("Model"), m_model);
    form->addRow(tr("Effort"), m_effort);
    form->addRow(tr("API address"), m_baseUrl);
    form->addRow(tr("Rows per query"), m_maxRows);

    auto *note = new QLabel(tr("The assistant only runs read-only queries. Statements that change data or structure are shown "
                               "to you to open in a query tab. Requests are billed to the API key's account."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AiSettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_sendRows);
    layout->addWidget(m_ask);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(560, sizeHint().height());
}

void AiSettingsDialog::save()
{
    const QString key = m_key->text().trimmed();
    if (!key.isEmpty()) {
        QString error;
        if (!AiSettings::saveApiKey(key, &error)) {
            QMessageBox::warning(this, windowTitle(), tr("Could not save the API key: %1").arg(error));
            return;
        }
    }
    AiSettings settings = AiSettings::load();
    settings.model = m_model->currentData().toString();
    settings.effort = m_effort->currentText();
    settings.baseUrl = m_baseUrl->text().trimmed();
    settings.sendRows = m_sendRows->isChecked();
    settings.maxRows = m_maxRows->value();
    settings.askBeforeQuery = m_ask->isChecked();
    settings.save();
    accept();
}
