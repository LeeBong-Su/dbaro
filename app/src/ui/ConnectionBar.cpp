#include "ConnectionBar.h"

#include "UiHelpers.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

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

    m_schemaLabel = new QLabel(this);
    m_schemaLabel->setObjectName(QStringLiteral("connectionSchema"));
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
    layout->addWidget(m_schemaLabel);
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
    refresh();
}

QString ConnectionBar::schemaText() const
{
    return m_schemaLabel->isVisibleTo(this) ? m_schemaLabel->text() : QString();
}

QString ConnectionBar::nameText() const
{
    return m_name->text();
}

QString ConnectionBar::stateText() const
{
    return m_stateLabel->text();
}

QString ConnectionBar::target(const ConnectionProfile &profile)
{
    QString where = QStringLiteral("%1:%2").arg(profile.host).arg(profile.port);
    if (!profile.database.isEmpty())
        where += (profile.type == DbType::Oracle && profile.oracleUseSid ? QStringLiteral(":") : QStringLiteral("/"))
                 + profile.database;
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
        for (QLabel *label : {m_schemaLabel, m_environment, m_readOnly, m_pendingBadge, m_stateLabel})
            label->hide();
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

    // USE로 바꾼 데이터베이스는 이름 옆에 테두리 칩으로 보여 준다.
    m_schemaLabel->setVisible(!m_schema.isEmpty());
    if (!m_schema.isEmpty()) {
        m_schemaLabel->setText(tr("USE %1").arg(m_schema));
        m_schemaLabel->setToolTip(tr("Default database for this tab (changed with USE)"));
        m_schemaLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; border: 1px solid %2; border-radius: 4px; padding: 1px 7px; font-weight: 600; }")
                .arg(pal.color(QPalette::WindowText).name(), pal.color(QPalette::Mid).name()));
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
