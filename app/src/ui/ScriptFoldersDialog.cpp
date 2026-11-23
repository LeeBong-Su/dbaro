#include "ScriptFoldersDialog.h"

#include "core/ScriptFolders.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

ScriptFoldersDialog::ScriptFoldersDialog(QWidget *parent, DbType focus)
    : QDialog(parent)
{
    setWindowTitle(tr("Script Folders"));
    auto *intro = new QLabel(tr("In a CLI tab, @name runs name.sql from the folder of the connection's DBMS.\n"
                                "@@name runs a file next to the script that calls it. Full paths work too."),
                             this);
    intro->setWordWrap(true);

    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;
    for (const DbType type : {DbType::Oracle, DbType::Tibero, DbType::MariaDB, DbType::MySQL, DbType::PostgreSQL}) {
        auto *edit = new QLineEdit(ScriptFolders::folder(type), this);
        edit->setPlaceholderText(tr("Not set"));
        edit->setClearButtonEnabled(true);
        edit->setMinimumWidth(420);
        auto *browse = new QPushButton(tr("Browse..."), this);
        connect(browse, &QPushButton::clicked, this, [this, edit, type] {
            const QString chosen = QFileDialog::getExistingDirectory(
                this, tr("%1 Script Folder").arg(Db::typeName(type)), edit->text().isEmpty() ? QDir::homePath() : edit->text());
            if (!chosen.isEmpty())
                edit->setText(QDir::toNativeSeparators(chosen));
        });
        auto *open = new QPushButton(tr("Open"), this);
        open->setToolTip(tr("Show the folder in the file manager"));
        connect(open, &QPushButton::clicked, this, [edit] {
            if (QFileInfo(edit->text()).isDir())
                QDesktopServices::openUrl(QUrl::fromLocalFile(edit->text()));
        });
        grid->addWidget(new QLabel(Db::typeName(type), this), row, 0);
        grid->addWidget(edit, row, 1);
        grid->addWidget(browse, row, 2);
        grid->addWidget(open, row, 3);
        m_paths.insert(int(type), edit);
        if (type == focus)
            edit->setFocus();
        ++row;
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ScriptFoldersDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(grid);
    layout->addWidget(buttons);
}

void ScriptFoldersDialog::accept()
{
    // 없는 폴더는 저장하지 않고 알려 준다
    for (auto it = m_paths.constBegin(); it != m_paths.constEnd(); ++it) {
        const QString path = it.value()->text().trimmed();
        if (!path.isEmpty() && !QFileInfo(path).isDir()) {
            QMessageBox::warning(this, windowTitle(), tr("This folder does not exist:\n%1").arg(path));
            it.value()->setFocus();
            return;
        }
    }
    for (auto it = m_paths.constBegin(); it != m_paths.constEnd(); ++it)
        ScriptFolders::setFolder(DbType(it.key()), it.value()->text());
    QDialog::accept();
}
