#pragma once

#include "core/SqlRename.h"
#include "core/TextFile.h"

#include <QCborMap>
#include <QDialog>
#include <QHash>
#include <QPointer>

class MetadataService;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// 이름 바꾸기 리팩토링 (테이블 또는 컬럼).
// 새 이름을 넣고 Find References를 누르면 그 이름을 쓰는 곳을 모은다:
// - DB 객체: 엔진 renameImpact (뷰·프로시저·패키지·트리거·시노님. PostgreSQL이 스스로 고치는 것은 안내만)
// - SQL 파일: 스크립트 폴더(와 더한 폴더) 아래의 .sql 등 (열린 탭의 파일은 탭으로 고친다)
// - 열린 쿼리 탭
// 바꿀 곳마다 체크할 수 있고(확실하지 않은 곳은 꺼 둔다), 고르면 바뀌기 전·후 줄을 보인다.
// Apply: 파일은 바로 쓰고(인코딩·줄 끝 유지), 탭과 DB 스크립트는 부른 쪽이 tabEdits()·script()로 처리한다.
// DB는 스크립트를 쿼리 탭에 열 뿐이라 실행해야 바뀐다.
class RenameDialog : public QDialog
{
    Q_OBJECT

public:
    struct OpenText
    {
        QString key;      // 부른 쪽이 탭을 찾는 값
        QString title;    // 탭 제목
        QString text;     // 에디터 글
        QString filePath; // 파일 탭이면 그 경로 (폴더 검색에서 빼려고)
    };

    RenameDialog(MetadataService *metadata, const QString &metadataId, SqlDialect dialect, const QString &schema,
                 const QString &table, const QString &column, const QList<OpenText> &openTabs, const QStringList &folders,
                 QWidget *parent = nullptr);

    RenameTarget target() const { return m_target; }
    QLineEdit *nameEdit() const { return m_name; }
    QTreeWidget *tree() const { return m_tree; }
    QPlainTextEdit *preview() const { return m_preview; }
    bool isSearching() const { return m_pending > 0; }
    int changeCount() const; // 체크한 곳 수

    // Apply 뒤에 쓴다
    QString script() const { return m_script; }                                 // DB 스크립트 (없으면 빈 값)
    QHash<QString, QList<RenameMatch>> tabEdits() const { return m_tabEdits; }   // 탭 key → 바꿀 곳
    QStringList writtenFiles() const { return m_written; }
    QStringList failedFiles() const { return m_failed; }

    void findReferences();
    bool applyChanges(bool confirm = true); // confirm이면 묻는다. 적용했으면 true

private:
    enum class Kind { Database, File, Tab };
    struct Source
    {
        Kind kind = Kind::File;
        QString key;   // 파일 경로, 탭 key, DB 객체 "schema.name"
        QString title;
        QString text;
        QList<RenameMatch> matches;
        // DB 객체
        QString type;
        QStringList before;
        QString terminator;
        bool autoUpdated = false;
        QString note;
        // 파일
        TextFileFormat format;
    };

    void addSource(const Source &source);
    QTreeWidgetItem *group(Kind kind);
    void searchFiles();
    void showDatabase(const QCborMap &result, const QString &error);
    void showPreview();
    void updateSummary();
    QList<RenameMatch> checkedMatches(int sourceIndex) const;
    QString buildScript() const;
    void addFolder();

    QPointer<MetadataService> m_metadata;
    QString m_metadataId;
    RenameTarget m_target;
    QList<OpenText> m_openTabs;
    QStringList m_folders;
    QList<Source> m_sources;
    QHash<int, QTreeWidgetItem *> m_groups; // int(Kind) → 묶음
    int m_pending = 0;
    int m_generation = 0;

    QLineEdit *m_name = nullptr;
    QCheckBox *m_includeRename = nullptr;
    QCheckBox *m_searchDatabase = nullptr;
    QCheckBox *m_searchFiles = nullptr;
    QCheckBox *m_searchTabs = nullptr;
    QLabel *m_foldersLabel = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_notes = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_find = nullptr;
    QPushButton *m_apply = nullptr;

    QString m_script;
    QHash<QString, QList<RenameMatch>> m_tabEdits;
    QStringList m_written;
    QStringList m_failed;
};
