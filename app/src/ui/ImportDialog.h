#pragma once

#include "core/DataImport.h"

#include <QDialog>
#include <QPointer>

class MetadataService;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

// 엑셀(.xlsx)·CSV 파일을 테이블로 불러오기.
// 1) 파일과 시트를 고른다 (첫 줄 머리글, 빈 칸은 NULL, 앞뒤 공백 지우기)
// 2) 대상: 있는 테이블(컬럼을 읽어 와 머리글 이름으로 짝짓는다) 또는 새 테이블(값으로 컬럼 타입을 짐작, 이름·타입 고칠 수 있음)
// 3) 컬럼 짝과 앞쪽 50줄 미리보기를 확인하고 Import (또는 SQL로 열어 직접 고쳐 실행)
class ImportDialog : public QDialog
{
    Q_OBJECT

public:
    ImportDialog(MetadataService *metadata, const QString &connectionId, SqlDialect dialect, const QString &schema,
                 const QString &table, QWidget *parent = nullptr);

    QStringList statements() const; // 새 테이블이면 CREATE TABLE이 맨 앞
    int dataRowCount() const;
    bool createsTable() const;
    bool stopOnError() const;
    bool openAsSql() const { return m_openAsSql; }
    QString targetDisplay() const;

    // 테스트·자동화용
    bool loadFile(const QString &path);
    void setTargetTable(const QString &qualified); // 있는 테이블 (컬럼을 읽어 온다)
    void setNewTable(const QString &name);
    void setHasHeader(bool on);
    bool isReady() const;
    QString statusText() const;
    QTableWidget *mappingTable() const { return m_mapping; }
    QTableWidget *previewTable() const { return m_preview; }

    void accept() override; // 준비가 안 됐으면 무엇이 빠졌는지 알려 준다

private:
    void browse();
    void loadTargetColumns();
    void rebuild();          // 시트·머리글·대상이 바뀌면 짝과 미리보기를 다시
    void rebuildPreview();
    void setStatus(const QString &text, bool error = false);
    QStringList header() const;
    QList<QStringList> dataRows() const;
    QList<ImportColumn> targetColumns() const; // 새 테이블이면 짝 표에서 고친 이름·타입
    QList<int> mapping() const;
    QString targetName() const;

    QPointer<MetadataService> m_metadata;
    QString m_connectionId;
    SqlDialect m_dialect;
    QString m_defaultSchema;
    QList<ImportSheet> m_sheets;
    QList<ImportColumn> m_existingColumns;
    QString m_existingName; // 읽어 온 테이블의 따옴표 붙인 이름
    bool m_openAsSql = false;
    bool m_rebuilding = false;

    QLineEdit *m_path = nullptr;
    QComboBox *m_sheet = nullptr;
    QCheckBox *m_header = nullptr;
    QCheckBox *m_emptyAsNull = nullptr;
    QCheckBox *m_trim = nullptr;
    QRadioButton *m_existing = nullptr;
    QRadioButton *m_new = nullptr;
    QLineEdit *m_table = nullptr;
    QLineEdit *m_newTable = nullptr;
    QPushButton *m_loadColumns = nullptr;
    QTableWidget *m_mapping = nullptr;
    QTableWidget *m_preview = nullptr;
    QCheckBox *m_stopOnError = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_asSql = nullptr;
};
