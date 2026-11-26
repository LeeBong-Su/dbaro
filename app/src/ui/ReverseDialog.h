#pragma once

#include <QDialog>
#include <QPointer>

class MetadataService;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// 리버스 엔지니어링 대상 고르기: 스키마, 테이블(·뷰) 목록에서 체크, 뷰 포함.
// 목록은 탐색기·자동완성과 같은 메타데이터 캐시에서 읽는다.
class ReverseDialog : public QDialog
{
    Q_OBJECT

public:
    ReverseDialog(MetadataService *metadata, const QString &metadataId, const QString &schema, QWidget *parent = nullptr);

    QString schema() const;
    // 고른 테이블·뷰. 모두 골랐으면 빈 목록 (스키마 전체)
    QStringList tables() const;
    bool includeViews() const;
    QListWidget *list() const { return m_list; }

private:
    void loadSchemas(const QString &preferred);
    void loadObjects();
    void applyFilter();
    void updateCount();

    QPointer<MetadataService> m_metadata;
    QString m_metadataId;
    QComboBox *m_schema = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QCheckBox *m_views = nullptr;
    QLabel *m_count = nullptr;
    QPushButton *m_ok = nullptr;
    int m_generation = 0;
};
