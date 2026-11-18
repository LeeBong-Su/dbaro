#pragma once

#include "core/QueryHistory.h"
#include "core/SqlDialect.h"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// 실행 기록 (F8, Ctrl+Alt+E). 최근 것이 위. 검색어의 모든 단어가 들어 있는 문장만 보여 준다.
// 두 번 클릭이나 Enter(Insert)로 지금 에디터에 넣는다.
class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    HistoryDialog(const QList<HistoryEntry> &entries, const QString &currentConnectionId, SqlDialect dialect,
                  QWidget *parent = nullptr);

    QString selectedSql() const;
    bool clearRequested() const { return m_clearRequested; }

    void setFilterText(const QString &text);
    void setCurrentConnectionOnly(bool on);
    int visibleCount() const;
    void selectVisible(int index); // 보이는 항목 중 index번째를 고른다

private:
    void applyFilter();
    void updatePreview();

    QList<HistoryEntry> m_entries; // 최근 것이 앞
    QString m_connectionId;
    QLineEdit *m_filter = nullptr;
    QCheckBox *m_currentOnly = nullptr;
    QTreeWidget *m_list = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_insert = nullptr;
    bool m_clearRequested = false;
};
