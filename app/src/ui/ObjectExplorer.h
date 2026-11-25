#pragma once

#include "core/ConnectionProfile.h"
#include "core/SqlDialect.h"

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QModelIndex>
#include <QPointer>
#include <QWidget>

#include <functional>

class MetadataService;
class QLineEdit;
class QModelIndex;
class QPersistentModelIndex;
class QSortFilterProxyModel;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

// 왼쪽 Database Explorer. 연결 → 스키마 → 객체 종류(테이블, 뷰, 함수 ...) → 객체 → 컬럼.
// - 노드를 펼칠 때 필요한 만큼만 메타데이터를 가져온다 (MetadataService: 자동완성과 같은 세션·캐시를 쓴다).
// - 두 번 클릭: 연결은 그 연결의 탭으로 이동, MySQL·MariaDB 스키마는 그 탭에서 USE,
//   테이블·뷰는 데이터 조회 탭 열기, 컬럼은 이름을 에디터에 넣기
// - 오른쪽 클릭: 새 쿼리 탭, 새로고침, 연결 끊기, 이름 복사 등
// - 여러 항목을 함께 골라(Ctrl·Shift+클릭) 에디터로 끌어다 놓거나 Ctrl+C로 복사하면 이름이 ", "로 이어져 들어간다
class ObjectExplorer : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectExplorer(MetadataService *metadata, QWidget *parent = nullptr);
    ~ObjectExplorer() override;

    void setConnections(const QList<ConnectionProfile> &profiles);
    // 연결 항목을 펼친다 (스키마 목록을 가져오고 지금 스키마를 펼친다)
    void expandConnection(const QString &connectionId);
    bool isConnectionExpanded(const QString &connectionId) const;

    QTreeView *tree() const { return m_tree; }
    QStandardItemModel *model() const { return m_model; }
    bool isLoading() const { return m_pendingRequests > 0; }

    // 트리에서 고른 객체(테이블·뷰 등). 객체가 아니면 false.
    bool selectedObject(QString *connectionId, QString *schema, QString *name) const;

    // 고른 객체·컬럼·스키마의 이름 (트리 순서, SQL에 바로 쓸 수 있게 필요하면 따옴표로 감싼다)
    QStringList selectedNames() const;
    QStringList namesOf(QModelIndexList proxyIndexes) const;

signals:
    void connectionActivated(const QString &connectionId);
    void newQueryRequested(const QString &connectionId);
    void consoleRequested(const QString &connectionId); // CLI 탭 열기
    void openObjectRequested(const QString &connectionId, const QString &schema, const QString &name);
    void useSchemaRequested(const QString &connectionId, const QString &schema); // 지금 탭의 스키마 바꾸기
    void ddlRequested(const QString &connectionId, const QString &schema, const QString &name);
    void tableDetailsRequested(const QString &connectionId, const QString &schema, const QString &name);
    void importRequested(const QString &connectionId, const QString &schema, const QString &name); // 이름이 비면 새 테이블
    void insertTextRequested(const QString &text);
    void editConnectionRequested(const QString &connectionId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using ResultHandler = std::function<void(QStandardItem *item, const QCborMap &result)>;

    void onExpanded(const QModelIndex &proxyIndex);
    void onDoubleClicked(const QModelIndex &proxyIndex);
    void showContextMenu(const QPoint &position);

    void load(QStandardItem *item);
    void loadSchemas(QStandardItem *connection);
    void loadObjects(QStandardItem *schema);
    void loadColumns(QStandardItem *object);
    void reload(QStandardItem *item);
    void request(QStandardItem *target, const QString &method, const QCborMap &params, ResultHandler onResult);

    void disconnectProfile(const QString &profileId);
    void setConnectedLook(const QString &profileId, bool connected);

    void setPlaceholder(QStandardItem *item, const QString &text, bool isError = false);
    QStandardItem *sourceItem(const QModelIndex &proxyIndex) const;
    QStandardItem *itemAt(const QPersistentModelIndex &index) const;
    QStandardItem *findConnection(const QString &profileId) const;
    const ConnectionProfile *profile(const QString &id) const;
    QString qualifiedName(const QStandardItem *object) const;
    QString nameOf(const QStandardItem *item) const; // 끌어다 놓기·복사에 쓰는 이름. 이름이 없는 항목은 빈 값
    SqlDialect dialectOf(const QStandardItem *item) const;
    bool usesDatabases(const QString &connectionId) const; // MySQL·MariaDB: 스키마 = 데이터베이스 (USE)
    void expandItem(QStandardItem *item);

    QPointer<MetadataService> m_metadata;
    QList<ConnectionProfile> m_profiles;
    bool m_refreshNext = false; // 다음 요청은 캐시를 무시하고 다시 가져온다 (새로고침)
    int m_pendingRequests = 0;
    int m_generation = 0; // 연결 목록을 새로 만들 때마다 증가 (늦게 온 응답 무시)

    QLineEdit *m_filter = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
};
