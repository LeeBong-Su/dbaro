#include "ConnectionBar.h"

#include "UiHelpers.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>

namespace {

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

QColor blend(const QColor &base, const QColor &tint, qreal amount)
{
    return QColor::fromRgbF(base.redF() + (tint.redF() - base.redF()) * amount,
                            base.greenF() + (tint.greenF() - base.greenF()) * amount,
                            base.blueF() + (tint.blueF() - base.blueF()) * amount);
}

QString badgeStyle(const QColor &background, const QColor &text)
{
    return QStringLiteral("QLabel { background: %1; color: %2; border-radius: 4px; padding: 2px 8px; font-weight: 600; }")
        .arg(background.name(), text.name());
}

QString environmentLabel(Environment env)
{
    switch (env) {
    case Environment::Development: return QStringLiteral("DEVELOPMENT");
    case Environment::Test: return QStringLiteral("TEST");
    case Environment::Staging: return QStringLiteral("STAGING");
    case Environment::Production: return QStringLiteral("PRODUCTION");
    case Environment::None: break;
    }
    return {};
}

} // namespace

ConnectionBar::ConnectionBar(QWidget *parent)
    : QWidget(parent)
{
    m_dot = new QLabel(this);
    m_name = new QLabel(this);
    QFont nameFont = m_name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() + 2);
    nameFont.setBold(true);
    m_name->setFont(nameFont);

    m_databaseCaption = new QLabel(tr("Database"), this);
    m_databaseCaption->setObjectName(QStringLiteral("connectionDatabaseCaption"));
    m_databaseCombo = new QComboBox(this);
    m_databaseCombo->setObjectName(QStringLiteral("databaseSwitcher"));
    m_databaseCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_databaseCombo->setMaxVisibleItems(25);
    m_databaseCombo->setFocusPolicy(Qt::ClickFocus);
    connect(m_databaseCombo, &QComboBox::activated, this, [this](int index) {
        const QString chosen = m_databaseCombo->itemText(index);
        if (!chosen.isEmpty() && chosen != shownDatabase())
            emit databaseChosen(chosen);
    });

    m_schemaCaption = new QLabel(this);
    m_schemaCaption->setObjectName(QStringLiteral("connectionSchemaCaption"));
    m_schemaCombo = new QComboBox(this);
    m_schemaCombo->setObjectName(QStringLiteral("schemaSwitcher"));
    m_schemaCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_schemaCombo->setMaxVisibleItems(25);
    m_schemaCombo->setFocusPolicy(Qt::ClickFocus); // Tab으로 에디터를 오갈 때 끼어들지 않게
    connect(m_schemaCombo, &QComboBox::activated, this, [this](int index) {
        const QString chosen = m_schemaCombo->itemText(index);
        if (!chosen.isEmpty() && chosen != m_schema)
            emit schemaChosen(chosen);
    });
    m_details = new QLabel(this);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_environment = new QLabel(this);
    m_readOnly = new QLabel(tr("READ-ONLY"), this);
    m_pendingBadge = new QLabel(tr("UNCOMMITTED CHANGES"), this);
    m_stateLabel = new QLabel(this);
    QFont stateFont = m_stateLabel->font();
    stateFont.setBold(true);
    m_stateLabel->setFont(stateFont);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 7, 12, 7);
    layout->setSpacing(10);
    layout->addWidget(m_dot);
    layout->addWidget(m_name);
    layout->addWidget(m_databaseCaption);
    layout->addWidget(m_databaseCombo);
    layout->addWidget(m_schemaCaption);
    layout->addWidget(m_schemaCombo);
    layout->addWidget(m_details, 1);
    layout->addWidget(m_environment);
    layout->addWidget(m_readOnly);
    layout->addWidget(m_pendingBadge);
    layout->addWidget(m_stateLabel);

    refresh();
}

void ConnectionBar::setProfile(const ConnectionProfile *profile)
{
    if (profile)
        m_profile = *profile;
    else
        m_profile.reset();
    refreshDatabaseItems(); // 고른 DB가 없으면 접속 설정의 DB를 보인다
    refresh();
}

void ConnectionBar::setState(State state, bool pendingTransaction, const QString &serverInfo)
{
    m_state = state;
    m_pending = pendingTransaction;
    m_serverInfo = serverInfo;
    refresh();
}

void ConnectionBar::setSchema(const QString &schema)
{
    m_schema = schema;
    refreshSchemaItems();
    refresh();
}

void ConnectionBar::setSchemas(const QStringList &schemas)
{
    m_schemas = schemas;
    refreshSchemaItems();
    refresh();
}

void ConnectionBar::refreshSchemaItems()
{
    const QSignalBlocker blocker(m_schemaCombo);
    QStringList items = m_schemas;
    if (!m_schema.isEmpty() && !items.contains(m_schema))
        items.prepend(m_schema); // 목록을 아직 못 받았거나 목록에 없는 스키마 (권한)
    m_schemaCombo->clear();
    m_schemaCombo->addItems(items);
    m_schemaCombo->setCurrentIndex(int(items.indexOf(m_schema)));
}

void ConnectionBar::setDatabase(const QString &database)
{
    m_database = database;
    refreshDatabaseItems();
    refresh();
}

void ConnectionBar::setDatabases(const QStringList &databases)
{
    m_databases = databases;
    refreshDatabaseItems();
    refresh();
}

QString ConnectionBar::shownDatabase() const
{
    if (!m_database.isEmpty())
        return m_database;
    return m_profile ? m_profile->database : QString();
}

void ConnectionBar::refreshDatabaseItems()
{
    const QSignalBlocker blocker(m_databaseCombo);
    const QString current = shownDatabase();
    QStringList items = m_databases;
    if (!current.isEmpty() && !items.contains(current))
        items.prepend(current);
    m_databaseCombo->clear();
    m_databaseCombo->addItems(items);
    m_databaseCombo->setCurrentIndex(int(items.indexOf(current)));
}

QString ConnectionBar::databaseText() const
{
    return m_databaseCombo->isVisibleTo(this) ? shownDatabase() : QString();
}

QString ConnectionBar::schemaText() const
{
    return m_schemaCombo->isVisibleTo(this) ? m_schema : QString();
}

QString ConnectionBar::nameText() const
{
    return m_name->text();
}

QString ConnectionBar::stateText() const
{
    return m_stateLabel->text();
}

QString ConnectionBar::target(const ConnectionProfile &profile) const
{
    QString where = QStringLiteral("%1:%2").arg(profile.host).arg(profile.port);
    const QString database = shownDatabase();
    if (!database.isEmpty())
        where += (profile.type == DbType::Oracle && profile.oracleUseSid ? QStringLiteral(":") : QStringLiteral("/"))
                 + database;
    QString text = QStringLiteral("%1  ·  %2").arg(Db::typeName(profile.type), where);
    if (!profile.user.isEmpty())
        text += QStringLiteral("  ·  %1").arg(profile.user);
    return text;
}

void ConnectionBar::refresh()
{
    const QPalette pal = palette();
    const bool dark = isDark(pal);
    const QColor dim = pal.color(QPalette::PlaceholderText);

    if (!m_profile) {
        m_dot->clear();
        m_name->setText(tr("No connection"));
        m_name->setStyleSheet(QStringLiteral("color: %1;").arg(dim.name()));
        m_details->setText(tr("Choose a connection in the toolbar or double-click one in Database Explorer."));
        m_details->setStyleSheet(QStringLiteral("color: %1;").arg(dim.name()));
        for (QLabel *label : {m_databaseCaption, m_schemaCaption, m_environment, m_readOnly, m_pendingBadge, m_stateLabel})
            label->hide();
        m_schemaCombo->hide();
        m_databaseCombo->hide();
        update();
        return;
    }

    const ConnectionProfile &profile = *m_profile;
    const QColor envColor = Env::color(profile.environment);
    m_dot->setPixmap(colorSwatchIcon(envColor.isValid() ? envColor : dim, 14).pixmap(14, 14));
    m_name->setText(profile.name);
    m_name->setStyleSheet(QString());
    m_details->setText(target(profile));
    m_details->setStyleSheet(QStringLiteral("color: %1;").arg(dim.name()));

    // 이 탭의 지금 스키마: 접속해 있으면 이름 옆에 목록으로 (고르면 바로 바꾼다)
    const bool online = m_state == State::Connected || m_state == State::Running;

    // PostgreSQL: 그 앞에 데이터베이스 목록 (고르면 그 DB로 다시 접속한다)
    const bool showDatabase = online && Db::canSwitchDatabase(profile.type) && !m_databases.isEmpty();
    m_databaseCaption->setVisible(showDatabase);
    m_databaseCombo->setVisible(showDatabase);
    m_databaseCombo->setEnabled(m_state == State::Connected);
    if (showDatabase) {
        m_databaseCaption->setStyleSheet(QStringLiteral("color: %1;").arg(dim.name()));
        const QString tip = tr("Database of this tab. PostgreSQL cannot switch databases within a session,\n"
                               "so choosing another one reconnects this tab to it.");
        m_databaseCombo->setToolTip(tip);
        m_databaseCaption->setToolTip(tip);
    }

    const bool showSchema = online && (!m_schema.isEmpty() || !m_schemas.isEmpty());
    m_schemaCaption->setVisible(showSchema);
    m_schemaCombo->setVisible(showSchema);
    m_schemaCombo->setEnabled(m_state == State::Connected);
    if (showSchema) {
        const bool databases = profile.type == DbType::MySQL || profile.type == DbType::MariaDB;
        m_schemaCaption->setText(databases ? tr("Database") : tr("Schema"));
        m_schemaCaption->setStyleSheet(QStringLiteral("color: %1;").arg(dim.name()));
        QString tip;
        switch (profile.type) {
        case DbType::Oracle:
        case DbType::Tibero:
            tip = tr("Current schema of this tab (ALTER SESSION SET CURRENT_SCHEMA).\n"
                     "Names without a schema, completion, DDL and DESC use it.");
            break;
        case DbType::PostgreSQL:
            tip = tr("Schema searched first in this tab (SET search_path).\n"
                     "Names without a schema, completion, DDL and DESC use it.");
            break;
        default:
            tip = tr("Default database of this tab (USE).\n"
                     "Names without a database, completion, DDL and DESC use it.");
            break;
        }
        m_schemaCombo->setToolTip(tip);
        m_schemaCaption->setToolTip(tip);
    }

    const QString envText = environmentLabel(profile.environment);
    m_environment->setVisible(!envText.isEmpty());
    if (!envText.isEmpty()) {
        m_environment->setText(envText);
        m_environment->setStyleSheet(badgeStyle(envColor, Qt::white));
    }

    m_readOnly->setVisible(profile.readOnly);
    m_readOnly->setStyleSheet(badgeStyle(dark ? QColor(0x6b, 0x55, 0x16) : QColor(0xb7, 0x79, 0x1f), Qt::white));

    m_pendingBadge->setVisible(m_pending);
    m_pendingBadge->setStyleSheet(badgeStyle(dark ? QColor(0x9a, 0x45, 0x07) : QColor(0xc2, 0x41, 0x0c), Qt::white));

    QString stateText;
    QColor stateColor;
    switch (m_state) {
    case State::NotConnected:
        stateText = tr("○  Not connected");
        stateColor = dim;
        break;
    case State::Connecting:
        stateText = tr("◌  Connecting...");
        stateColor = pal.color(QPalette::Link);
        break;
    case State::Connected:
        stateText = m_serverInfo.isEmpty() ? tr("●  Connected") : tr("●  Connected · %1").arg(m_serverInfo);
        stateColor = dark ? QColor(0x4a, 0xc2, 0x6b) : QColor(0x1a, 0x7f, 0x37);
        break;
    case State::Running:
        stateText = tr("▶  Running...");
        stateColor = pal.color(QPalette::Link);
        break;
    }
    m_stateLabel->setText(stateText);
    m_stateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(stateColor.name()));
    m_stateLabel->show();
    update();
}

void ConnectionBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor panel = palette().color(QPalette::Window);
    const QColor envColor = m_profile ? Env::color(m_profile->environment) : QColor();

    // 환경 색을 배경에 은은하게 섞는다. 운영 DB는 더 진하게 해서 바로 눈에 띄게 한다.
    QColor background = panel;
    if (envColor.isValid()) {
        const qreal amount = m_profile->environment == Environment::Production ? 0.22 : 0.10;
        background = blend(panel, envColor, amount);
    }
    painter.fillRect(rect(), background);
    if (envColor.isValid())
        painter.fillRect(QRect(0, 0, 4, height()), envColor);
    painter.fillRect(QRect(0, height() - 1, width(), 1), palette().color(QPalette::Midlight));
}

void ConnectionBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        refresh();
}
