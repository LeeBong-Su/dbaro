#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace {

const auto kSettingsKey = QStringLiteral("appearance/theme");

struct Colors
{
    QColor panel;     // 툴바, 도구 창, 상태 표시줄
    QColor editor;    // 에디터, 표, 입력란 (가장 어두운/밝은 바탕)
    QColor alternate; // 현재 줄, 번갈아 칠하는 행
    QColor button;
    QColor text;
    QColor textDim;
    QColor disabled;
    QColor border;
    QColor hover;
    QColor pressed;
    QColor accent;
    QColor selection;
    QColor selectionText;
    QColor popup;
    QColor scroll;
    QColor icon;
};

QColor hex(QRgb value)
{
    return QColor(value);
}

Colors colorsFor(Theme::Kind kind)
{
    Colors c;
    if (kind == Theme::Kind::Dark) {
        c.panel = hex(0x2a2c30);
        c.editor = hex(0x1d1e21);
        c.alternate = hex(0x26282c);
        c.button = hex(0x34373c);
        c.text = hex(0xcfd2d6);
        c.textDim = hex(0x9a9ea6);
        c.disabled = hex(0x666a71);
        c.border = hex(0x393b40);
        c.hover = hex(0x3a3d43);
        c.pressed = hex(0x45484f);
        c.accent = hex(0x4c8df6);
        c.selection = hex(0x2d5ba6);
        c.selectionText = hex(0xffffff);
        c.popup = hex(0x303237);
        c.scroll = hex(0x4a4d53);
        c.icon = hex(0xb4b8bf);
    } else {
        c.panel = hex(0xf2f3f5);
        c.editor = hex(0xffffff);
        c.alternate = hex(0xf5f7fa);
        c.button = hex(0xe9ebee);
        c.text = hex(0x1f2328);
        c.textDim = hex(0x5e636b);
        c.disabled = hex(0xa0a4ab);
        c.border = hex(0xdadce0);
        c.hover = hex(0xe3e5e8);
        c.pressed = hex(0xd4d7dc);
        c.accent = hex(0x3574f0);
        c.selection = hex(0xcfe0fc);
        c.selectionText = hex(0x1f2328);
        c.popup = hex(0xffffff);
        c.scroll = hex(0xc3c6cb);
        c.icon = hex(0x5a5f66);
    }
    return c;
}

QPalette paletteFor(const Colors &c)
{
    QPalette p;
    p.setColor(QPalette::Window, c.panel);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.editor);
    p.setColor(QPalette::AlternateBase, c.alternate);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, c.selection);
    p.setColor(QPalette::HighlightedText, c.selectionText);
    p.setColor(QPalette::ToolTipBase, c.popup);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::PlaceholderText, c.textDim);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::LinkVisited, c.accent);
    p.setColor(QPalette::Light, c.border.lighter(125));
    p.setColor(QPalette::Midlight, c.border);
    p.setColor(QPalette::Mid, c.border.darker(110));
    p.setColor(QPalette::Dark, c.border.darker(140));
    p.setColor(QPalette::Shadow, c.border.darker(220));

    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, c.disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.border);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, c.disabled);
    return p;
}

QString styleSheetFor(const Colors &c)
{
    QString qss = QStringLiteral(R"(
QMainWindow::separator { background: @border@; width: 1px; height: 1px; }

QMenuBar { background: @panel@; color: @text@; }
QMenuBar::item { background: transparent; padding: 4px 8px; border-radius: 4px; }
QMenuBar::item:selected { background: @hover@; }
QMenu { background: @popup@; color: @text@; border: 1px solid @border@; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 22px; border-radius: 4px; }
QMenu::item:selected { background: @selection@; color: @selectionText@; }
QMenu::item:disabled { color: @disabled@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }

QToolBar { background: @panel@; border: none; spacing: 2px; }
QToolBar#mainToolBar { border-bottom: 1px solid @border@; padding: 3px 6px; }
QToolBar#toolWindowStripe { border-right: 1px solid @border@; padding: 4px 3px; }
QToolBar::separator { background: @border@; width: 1px; height: 1px; margin: 5px 6px; }
QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px; color: @text@; }
QToolButton:hover { background: @hover@; }
QToolButton:pressed, QToolButton:checked { background: @pressed@; }
QToolButton:disabled { color: @disabled@; }
QToolButton::menu-indicator { image: none; width: 0px; }
QToolButton#tabCloseButton { padding: 0px; border-radius: 3px; }

QWidget#dockTitleBar { background: @panel@; border-bottom: 1px solid @border@; }
QLabel#dockTitle { color: @textDim@; }

QTabWidget::pane { border: none; }
QTabBar { background: @panel@; }
QTabBar::tab { background: transparent; color: @textDim@; padding: 6px 12px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: @text@; border-bottom: 2px solid @accent@; }
QTabBar::tab:hover:!selected { background: @hover@; }

QStatusBar { background: @panel@; color: @textDim@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @textDim@; }

QSplitter::handle { background: @border@; }

QToolTip { color: @text@; background: @popup@; border: 1px solid @border@; padding: 4px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0px; }
QScrollBar::handle { background: @scroll@; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 24px; margin: 2px; }
QScrollBar::handle:horizontal { min-width: 24px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTableView#resultGrid::item { padding: 0px 10px; }
QTableView#resultGrid QHeaderView::section { background: @panel@; color: @text@; border: none; border-right: 1px solid @border@; border-bottom: 1px solid @border@; padding: 0px 10px 0px 13px; }
QTableView#resultGrid QHeaderView::section:vertical { color: @textDim@; padding: 0px 6px; }
QTableView#resultGrid QTableCornerButton::section { background: @panel@; border: none; border-right: 1px solid @border@; border-bottom: 1px solid @border@; }

QListView#completionPopup { background: @popup@; color: @text@; border: 1px solid @border@; padding: 2px; outline: 0; }
QListView#completionPopup::item { padding: 2px 6px; border-radius: 3px; }
QListView#completionPopup::item:selected { background: @selection@; color: @selectionText@; }

QTreeView#explorerTree { background: @panel@; }
QLineEdit#explorerFilter { background: @editor@; border: 1px solid @border@; border-radius: 4px; padding: 3px 6px; }

QWidget#findBar { background: @panel@; border-bottom: 1px solid @border@; }
QWidget#findBar QLineEdit { background: @editor@; border: 1px solid @border@; border-radius: 4px; padding: 2px 6px; }
QWidget#findBar QLineEdit:focus { border: 1px solid @accent@; }
QWidget#findBar QLineEdit[invalid="true"] { border: 1px solid #c75450; }
QWidget#findBar QToolButton { padding: 2px 5px; border-radius: 3px; }
QWidget#findBar QToolButton:checked { background: @selection@; color: @selectionText@; }
QLabel#findStatus { color: @textDim@; }
QLabel#findStatus[error="true"] { color: #e06c6c; }

QTableView#historyList { background: @editor@; }

QWidget#launcherWindow { background: @panel@; }
QLabel#launcherTitle { font-size: 22px; font-weight: 600; }
QLabel#launcherSubtitle, QLabel#launcherEmpty { color: @textDim@; }
QLineEdit#launcherFilter { background: @editor@; border: 1px solid @border@; border-radius: 4px; padding: 5px 8px; }
QTreeWidget#launcherList { background: @editor@; border: 1px solid @border@; }
QTreeWidget#launcherList::item { padding: 5px 2px; }
)");

    const QList<QPair<QString, QColor>> tokens{
        {QStringLiteral("@panel@"), c.panel},         {QStringLiteral("@editor@"), c.editor},
        {QStringLiteral("@text@"), c.text},           {QStringLiteral("@textDim@"), c.textDim},
        {QStringLiteral("@disabled@"), c.disabled},   {QStringLiteral("@border@"), c.border},
        {QStringLiteral("@hover@"), c.hover},         {QStringLiteral("@pressed@"), c.pressed},
        {QStringLiteral("@accent@"), c.accent},       {QStringLiteral("@selection@"), c.selection},
        {QStringLiteral("@selectionText@"), c.selectionText}, {QStringLiteral("@popup@"), c.popup},
        {QStringLiteral("@scroll@"), c.scroll},
    };
    for (const auto &[token, color] : tokens)
        qss.replace(token, color.name());
    return qss;
}

Theme::Kind g_current = Theme::Kind::Dark;

} // namespace

Theme::Kind Theme::current()
{
    return g_current;
}

Theme::Kind Theme::load()
{
    const QString value = QSettings().value(kSettingsKey, QStringLiteral("dark")).toString();
    return value == QStringLiteral("light") ? Kind::Light : Kind::Dark;
}

void Theme::save(Kind kind)
{
    QSettings().setValue(kSettingsKey, kind == Kind::Light ? QStringLiteral("light") : QStringLiteral("dark"));
}

void Theme::apply(Kind kind)
{
    g_current = kind;
    const Colors colors = colorsFor(kind);

    // 스타일시트를 걸면 style()이 스타일시트용 래퍼를 돌려주므로 이름으로 확인할 수 없다. 처음 한 번만 설정한다.
    static bool fusionApplied = false;
    if (!fusionApplied) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        fusionApplied = true;
    }

    // Windows 제목 표시줄 등 OS가 그리는 부분도 같은 밝기로 맞춘다.
    QGuiApplication::styleHints()->setColorScheme(kind == Kind::Dark ? Qt::ColorScheme::Dark
                                                                     : Qt::ColorScheme::Light);
    QApplication::setPalette(paletteFor(colors));
    qApp->setStyleSheet(styleSheetFor(colors));
}

QColor Theme::iconColor()
{
    return colorsFor(g_current).icon;
}

QColor Theme::searchMatchColor()
{
    return g_current == Kind::Dark ? QColor(0x4b, 0x43, 0x26) : QColor(0xff, 0xf1, 0xa8);
}

QColor Theme::currentMatchColor()
{
    return g_current == Kind::Dark ? QColor(0x8a, 0x6a, 0x14) : QColor(0xff, 0xc4, 0x3d);
}
