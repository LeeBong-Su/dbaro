#include "ConnectionDialog.h"

#include "UiHelpers.h"
#include "core/ConnectSpec.h"
#include "core/SecretStore.h"
#include "ipc/EngineClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

ConnectionDialog::ConnectionDialog(const QList<ConnectionProfile> &profiles, QWidget *parent)
    : QDialog(parent)
    , m_profiles(profiles)
{
    setWindowTitle(tr("Connections"));
    resize(880, 600);

    // 왼쪽: 연결 목록
    m_list = new QListWidget(this);
    m_addButton = new QPushButton(tr("Add"), this);
    m_duplicateButton = new QPushButton(tr("Duplicate"), this);
    m_deleteButton = new QPushButton(tr("Delete"), this);

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_addButton);
    listButtons->addWidget(m_duplicateButton);
    listButtons->addWidget(m_deleteButton);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_list);
    leftLayout->addLayout(listButtons);

    // 오른쪽: 선택한 연결의 설정
    m_pages = new QTabWidget(this);
    m_pages->addTab(createGeneralPage(), tr("General"));
    m_pages->addTab(createAdvancedPage(), tr("Advanced"));

    auto *splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(m_pages);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 630});
    splitter->setChildrenCollapsible(false);

    // 아래: 연결 테스트, 메시지, 저장/취소
    m_testButton = new QPushButton(tr("Test Connection"), this);
    m_testButton->setEnabled(false);
    m_testButton->setToolTip(tr("Connect with these settings and report the server version."));
    connect(m_testButton, &QPushButton::clicked, this, &ConnectionDialog::testConnection);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_testButton);
    bottom->addWidget(m_message, 1);
    bottom->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    connect(buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ConnectionDialog::reject);
    connect(m_addButton, &QPushButton::clicked, this, &ConnectionDialog::addConnection);
    connect(m_duplicateButton, &QPushButton::clicked, this, &ConnectionDialog::duplicateConnection);
    connect(m_deleteButton, &QPushButton::clicked, this, &ConnectionDialog::deleteConnection);
    connect(m_list, &QListWidget::currentRowChanged, this, &ConnectionDialog::onCurrentRowChanged);

    for (int row = 0; row < m_profiles.size(); ++row) {
        m_list->addItem(new QListWidgetItem);
        refreshListItem(row);
    }
    if (m_profiles.isEmpty())
        onCurrentRowChanged(-1);
    else
        m_list->setCurrentRow(0);
}

QWidget *ConnectionDialog::createGeneralPage()
{
    auto *page = new QWidget(this);
    m_generalForm = new QFormLayout(page);

    m_name = new QLineEdit(page);

    m_type = new QComboBox(page);
    for (DbType type : Db::allTypes())
        m_type->addItem(Db::typeName(type), static_cast<int>(type));

    m_host = new QLineEdit(page);
    m_port = new QSpinBox(page);
    m_port->setRange(1, 65535);
    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(m_host, 1);
    hostRow->addWidget(new QLabel(tr("Port"), page));
    hostRow->addWidget(m_port);

    m_oracleTarget = new QComboBox(page);
    m_oracleTarget->addItem(tr("Service name"));
    m_oracleTarget->addItem(tr("SID"));

    m_databaseLabel = new QLabel(page);
    m_database = new QLineEdit(page);

    m_user = new QLineEdit(page);

    m_password = new QLineEdit(page);
    m_password->setEchoMode(QLineEdit::Password);
    m_savePassword = new QCheckBox(tr("Save password in the OS credential store"), page);
    if (!SecretStore::isSupported()) {
        m_savePassword->setEnabled(false);
        m_savePassword->setToolTip(tr("Not supported on this OS yet. The password is asked when connecting."));
    }

    m_environment = new QComboBox(page);
    for (Environment env : Env::all())
        m_environment->addItem(colorSwatchIcon(Env::color(env)), Env::name(env), static_cast<int>(env));

    m_readOnly = new QCheckBox(tr("Read-only (block statements that change data)"), page);

    m_urlPreview = new QLineEdit(page);
    m_urlPreview->setReadOnly(true);

    m_generalForm->addRow(tr("Name"), m_name);
    m_generalForm->addRow(tr("Database type"), m_type);
    m_generalForm->addRow(tr("Host"), hostRow);
    m_generalForm->addRow(tr("Connect by"), m_oracleTarget);
    m_generalForm->addRow(m_databaseLabel, m_database);
    m_generalForm->addRow(tr("User"), m_user);
    m_generalForm->addRow(tr("Password"), m_password);
    m_generalForm->addRow(QString(), m_savePassword);
    m_generalForm->addRow(tr("Environment"), m_environment);
    m_generalForm->addRow(QString(), m_readOnly);
    m_generalForm->addRow(tr("JDBC URL"), m_urlPreview);

    connect(m_name, &QLineEdit::textEdited, this, &ConnectionDialog::onFormEdited);
    connect(m_type, &QComboBox::currentIndexChanged, this, &ConnectionDialog::onTypeChanged);
    connect(m_host, &QLineEdit::textEdited, this, &ConnectionDialog::onFormEdited);
    connect(m_port, &QSpinBox::valueChanged, this, &ConnectionDialog::onFormEdited);
    connect(m_oracleTarget, &QComboBox::currentIndexChanged, this, [this] {
        updateTypeDependentWidgets();
        onFormEdited();
    });
    connect(m_database, &QLineEdit::textEdited, this, &ConnectionDialog::onFormEdited);
    connect(m_user, &QLineEdit::textEdited, this, &ConnectionDialog::onFormEdited);
    connect(m_password, &QLineEdit::textEdited, this, &ConnectionDialog::onFormEdited);
    connect(m_savePassword, &QCheckBox::toggled, this, [this](bool checked) {
        m_password->setEnabled(checked);
        onFormEdited();
    });
    connect(m_environment, &QComboBox::currentIndexChanged, this, &ConnectionDialog::onFormEdited);
    connect(m_readOnly, &QCheckBox::toggled, this, &ConnectionDialog::onFormEdited);

    return page;
}

QWidget *ConnectionDialog::createAdvancedPage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_connectTimeout = new QSpinBox(page);
    m_connectTimeout->setRange(1, 600);
    m_connectTimeout->setSuffix(tr(" s"));

    m_queryTimeout = new QSpinBox(page);
    m_queryTimeout->setRange(0, 86400);
    m_queryTimeout->setSuffix(tr(" s"));
    m_queryTimeout->setSpecialValueText(tr("No limit"));

    m_initSql = new QPlainTextEdit(page);
    m_initSql->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_initSql->setMinimumHeight(110);

    m_driverPath = new QLineEdit(page);
    auto *browse = new QPushButton(tr("Browse..."), page);
    auto *driverRow = new QHBoxLayout;
    driverRow->addWidget(m_driverPath, 1);
    driverRow->addWidget(browse);

    m_driverHint = new QLabel(page);
    m_driverHint->setWordWrap(true);
    m_driverHint->setForegroundRole(QPalette::PlaceholderText);

    form->addRow(tr("Connect timeout"), m_connectTimeout);
    form->addRow(tr("Query timeout"), m_queryTimeout);
    form->addRow(tr("Run after connect"), m_initSql);
    form->addRow(tr("JDBC driver (.jar)"), driverRow);
    form->addRow(QString(), m_driverHint);

    connect(m_connectTimeout, &QSpinBox::valueChanged, this, &ConnectionDialog::onFormEdited);
    connect(m_queryTimeout, &QSpinBox::valueChanged, this, &ConnectionDialog::onFormEdited);
    connect(m_initSql, &QPlainTextEdit::textChanged, this, &ConnectionDialog::onFormEdited);
    connect(m_driverPath, &QLineEdit::textChanged, this, &ConnectionDialog::onFormEdited);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Select JDBC Driver"), m_driverPath->text(),
                                                          tr("JAR files (*.jar)"));
        if (!path.isEmpty())
            m_driverPath->setText(QDir::toNativeSeparators(path));
    });

    return page;
}

void ConnectionDialog::setEngine(EngineClient *engine)
{
    m_engine = engine;
    m_testButton->setEnabled(engine && m_currentRow >= 0 && !m_testing);
}

void ConnectionDialog::testConnection()
{
    if (!m_engine || m_currentRow < 0 || m_testing)
        return;
    storeForm();
    const ConnectionProfile profile = m_profiles[m_currentRow];

    // 입력 중인 비밀번호 → 저장된 비밀번호 → 직접 묻기 순서로 쓴다.
    QString password;
    const std::optional<QString> saved =
        profile.savePassword ? SecretStore::loadPassword(profile.id) : std::optional<QString>();
    if (m_pendingPasswords.contains(profile.id)) {
        password = m_pendingPasswords.value(profile.id);
    } else if (saved) {
        password = *saved;
    } else {
        bool ok = false;
        password = QInputDialog::getText(this, tr("Password"), tr("Password for %1:").arg(profile.user),
                                         QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
    }

    m_testing = true;
    m_testButton->setEnabled(false);
    showMessage(tr("Testing %1 ... (the JDBC driver is downloaded on first use)").arg(Db::jdbcUrl(profile)), false);

    QPointer<ConnectionDialog> self(this);
    m_engine->request(QStringLiteral("test"), ConnectSpec::fromProfile(profile, password, true).toCbor(),
                      [self](const QCborMap &result, const EngineError &error) {
                          if (!self)
                              return;
                          self->m_testing = false;
                          self->m_testButton->setEnabled(self->m_currentRow >= 0);
                          if (error.isError()) {
                              self->showMessage(tr("Connection failed: %1").arg(error.message), true);
                              return;
                          }
                          self->showMessage(
                              tr("Connected in %1 ms: %2 %3 (driver %4)")
                                  .arg(result.value(QStringLiteral("elapsedMs")).toInteger())
                                  .arg(result.value(QStringLiteral("product")).toString(),
                                       result.value(QStringLiteral("productVersion")).toString().section(u'\n', 0, 0),
                                       result.value(QStringLiteral("driverVersion")).toString()),
                              false);
                      });
}

void ConnectionDialog::startNewConnection()
{
    addConnection();
}

void ConnectionDialog::selectConnection(const QString &id)
{
    for (int row = 0; row < m_profiles.size(); ++row) {
        if (m_profiles[row].id == id) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

QString ConnectionDialog::selectedId() const
{
    if (m_currentRow < 0 || m_currentRow >= m_profiles.size())
        return {};
    return m_profiles[m_currentRow].id;
}

void ConnectionDialog::accept()
{
    storeForm();

    QString message;
    int badRow = -1;
    if (!validate(&message, &badRow)) {
        m_list->setCurrentRow(badRow);
        showMessage(message, true);
        return;
    }
    QDialog::accept();
}

void ConnectionDialog::addConnection()
{
    ConnectionProfile profile = ConnectionProfile::create(DbType::Oracle);
    profile.name = uniqueName(tr("New connection"));
    m_profiles.append(profile);

    m_list->addItem(new QListWidgetItem);
    refreshListItem(m_profiles.size() - 1);
    m_list->setCurrentRow(m_profiles.size() - 1);

    m_pages->setCurrentIndex(0);
    m_name->setFocus();
    m_name->selectAll();
}

void ConnectionDialog::duplicateConnection()
{
    if (m_currentRow < 0)
        return;
    storeForm();

    ConnectionProfile copy = m_profiles[m_currentRow];
    const QString sourceId = copy.id;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = uniqueName(tr("%1 (copy)").arg(copy.name));
    m_profiles.append(copy);

    // 비밀번호도 함께 복사한다. 저장은 대화상자를 저장할 때 일어난다.
    if (m_pendingPasswords.contains(sourceId)) {
        m_pendingPasswords.insert(copy.id, m_pendingPasswords.value(sourceId));
    } else if (copy.savePassword) {
        if (const std::optional<QString> saved = SecretStore::loadPassword(sourceId))
            m_pendingPasswords.insert(copy.id, *saved);
    }

    m_list->addItem(new QListWidgetItem);
    refreshListItem(m_profiles.size() - 1);
    m_list->setCurrentRow(m_profiles.size() - 1);
}

void ConnectionDialog::deleteConnection()
{
    if (m_currentRow < 0)
        return;

    const QString id = m_profiles[m_currentRow].id;
    const QString name = m_profiles[m_currentRow].name;
    if (QMessageBox::question(this, tr("Delete Connection"), tr("Delete the connection \"%1\"?").arg(name))
        != QMessageBox::Yes)
        return;

    m_removedIds.append(id);
    m_pendingPasswords.remove(id);

    const int row = m_currentRow;
    m_profiles.removeAt(row);
    m_currentRow = -1; // 목록에서 지우는 동안 폼이 엉뚱한 행에 저장되지 않게 한다
    delete m_list->takeItem(row);

    if (m_list->count() == 0)
        onCurrentRowChanged(-1);
    else
        m_list->setCurrentRow(qMin(row, m_list->count() - 1));
}

void ConnectionDialog::onCurrentRowChanged(int row)
{
    // 폼 내용은 편집할 때마다 저장되므로, 여기서는 새로 선택한 행만 불러온다.
    m_currentRow = row;
    const bool hasSelection = row >= 0 && row < m_profiles.size();
    m_pages->setEnabled(hasSelection);
    m_duplicateButton->setEnabled(hasSelection);
    m_deleteButton->setEnabled(hasSelection);
    m_testButton->setEnabled(hasSelection && m_engine && !m_testing);

    if (hasSelection) {
        loadForm(m_profiles[row]);
        showMessage(QString(), false);
    } else {
        clearForm();
        showMessage(m_profiles.isEmpty() ? tr("No connections yet. Click Add to create one.") : QString(), false);
    }
}

void ConnectionDialog::onTypeChanged()
{
    if (m_loading || m_currentRow < 0)
        return;

    // 포트를 바꾸지 않았다면 새 DB 종류의 기본 포트로 맞춘다.
    const DbType oldType = m_profiles[m_currentRow].type;
    const DbType newType = currentType();
    if (m_port->value() == Db::defaultPort(oldType))
        m_port->setValue(Db::defaultPort(newType));

    updateTypeDependentWidgets();
    onFormEdited();
}

void ConnectionDialog::onFormEdited()
{
    if (m_loading || m_currentRow < 0)
        return;
    storeForm();
    refreshListItem(m_currentRow);
    updateUrlPreview();
    showMessage(QString(), false);
}

void ConnectionDialog::loadForm(const ConnectionProfile &profile)
{
    m_loading = true;

    m_name->setText(profile.name);
    m_type->setCurrentIndex(m_type->findData(static_cast<int>(profile.type)));
    m_host->setText(profile.host);
    m_port->setValue(profile.port);
    m_oracleTarget->setCurrentIndex(profile.oracleUseSid ? 1 : 0);
    m_database->setText(profile.database);
    m_user->setText(profile.user);

    const bool secretsSupported = SecretStore::isSupported();
    const bool save = secretsSupported && profile.savePassword;
    m_savePassword->setChecked(save);
    m_password->setEnabled(save);
    if (m_pendingPasswords.contains(profile.id)) {
        m_password->setText(m_pendingPasswords.value(profile.id));
        m_password->setModified(true);
    } else {
        m_password->clear();
    }
    if (!save)
        m_password->setPlaceholderText(tr("Asked when connecting"));
    else if (SecretStore::hasPassword(profile.id))
        m_password->setPlaceholderText(tr("Saved. Type to change it."));
    else
        m_password->setPlaceholderText(tr("Not saved yet"));

    m_environment->setCurrentIndex(m_environment->findData(static_cast<int>(profile.environment)));
    m_readOnly->setChecked(profile.readOnly);
    m_connectTimeout->setValue(profile.connectTimeoutSec);
    m_queryTimeout->setValue(profile.queryTimeoutSec);
    m_initSql->setPlainText(profile.initSql);
    m_driverPath->setText(profile.driverPath);

    m_loading = false;

    updateTypeDependentWidgets();
    updateUrlPreview();
}

void ConnectionDialog::clearForm()
{
    m_loading = true;
    m_name->clear();
    m_host->clear();
    m_database->clear();
    m_user->clear();
    m_password->clear();
    m_password->setPlaceholderText(QString());
    m_initSql->clear();
    m_driverPath->clear();
    m_urlPreview->clear();
    m_loading = false;
}

void ConnectionDialog::storeForm()
{
    if (m_loading || m_currentRow < 0 || m_currentRow >= m_profiles.size())
        return;

    ConnectionProfile &profile = m_profiles[m_currentRow];
    profile.name = m_name->text().trimmed();
    profile.type = currentType();
    profile.host = m_host->text().trimmed();
    profile.port = m_port->value();
    profile.oracleUseSid = m_oracleTarget->currentIndex() == 1;
    profile.database = m_database->text().trimmed();
    profile.user = m_user->text().trimmed();
    profile.savePassword = m_savePassword->isChecked();
    profile.environment = static_cast<Environment>(m_environment->currentData().toInt());
    profile.readOnly = m_readOnly->isChecked();
    profile.connectTimeoutSec = m_connectTimeout->value();
    profile.queryTimeoutSec = m_queryTimeout->value();
    profile.initSql = m_initSql->toPlainText();
    profile.driverPath = m_driverPath->text().trimmed();

    if (m_password->isModified())
        m_pendingPasswords.insert(profile.id, m_password->text());
}

void ConnectionDialog::updateTypeDependentWidgets()
{
    const DbType type = currentType();
    const bool oracle = type == DbType::Oracle;
    const bool sid = oracle && m_oracleTarget->currentIndex() == 1;

    m_generalForm->setRowVisible(m_oracleTarget, oracle);
    m_databaseLabel->setText(Db::databaseLabel(type, sid));
    m_database->setPlaceholderText(Db::databasePlaceholder(type, sid));
    m_initSql->setPlaceholderText(Db::initSqlHint(type));
    QString hint = Db::requiresDriverPath(type)
                       ? tr("Required. Select tibero7-jdbc.jar from the Tibero client (client/lib/jar). "
                            "The vendor driver cannot be bundled with DBaro.")
                       : tr("Leave empty to download the driver automatically on first connect.");
    if (type == DbType::Oracle)
        hint += u' '
                + tr("orai18n.jar (for KO16MSWIN949, KO16KSC5601 and other character sets) is downloaded too; "
                     "with your own driver, put it in the same folder.");
    hint += u' ' + tr("Separate several jar files with ;.");
    m_driverHint->setText(hint);
}

void ConnectionDialog::updateUrlPreview()
{
    if (m_currentRow < 0 || m_currentRow >= m_profiles.size()) {
        m_urlPreview->clear();
        return;
    }
    m_urlPreview->setText(Db::jdbcUrl(m_profiles[m_currentRow]));
}

void ConnectionDialog::refreshListItem(int row)
{
    QListWidgetItem *item = m_list->item(row);
    if (!item || row >= m_profiles.size())
        return;

    const ConnectionProfile &profile = m_profiles[row];
    item->setText(profile.name.isEmpty() ? tr("(unnamed)") : profile.name);
    item->setIcon(colorSwatchIcon(Env::color(profile.environment)));
    item->setToolTip(QStringLiteral("%1 — %2").arg(Db::typeName(profile.type), Db::jdbcUrl(profile)));
}

void ConnectionDialog::showMessage(const QString &text, bool isError)
{
    m_message->setStyleSheet(isError ? QStringLiteral("color: #c62828;") : QString());
    m_message->setText(text);
}

DbType ConnectionDialog::currentType() const
{
    return static_cast<DbType>(m_type->currentData().toInt());
}

QString ConnectionDialog::uniqueName(const QString &base) const
{
    auto taken = [this](const QString &name) {
        for (const ConnectionProfile &profile : m_profiles) {
            if (profile.name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(candidate))
            return candidate;
    }
}

bool ConnectionDialog::validate(QString *message, int *badRow) const
{
    QSet<QString> names;
    for (int row = 0; row < m_profiles.size(); ++row) {
        const ConnectionProfile &profile = m_profiles[row];
        const QString label = profile.name.isEmpty() ? tr("(unnamed)") : profile.name;
        auto fail = [&](const QString &text) {
            *message = QStringLiteral("%1: %2").arg(label, text);
            *badRow = row;
            return false;
        };

        if (profile.name.isEmpty())
            return fail(tr("Enter a name."));
        const QString key = profile.name.toLower();
        if (names.contains(key))
            return fail(tr("Another connection has the same name."));
        names.insert(key);

        if (profile.host.isEmpty())
            return fail(tr("Enter a host."));
        if (profile.database.isEmpty() && Db::databaseRequired(profile.type))
            return fail(tr("%1 is required.").arg(Db::databaseLabel(profile.type, profile.oracleUseSid)));
        if (Db::requiresDriverPath(profile.type) && profile.driverPath.isEmpty())
            return fail(tr("Select the Tibero JDBC driver (.jar) on the Advanced tab."));
    }
    return true;
}
