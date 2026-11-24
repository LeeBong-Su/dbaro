#pragma once

#include "core/SqlCompletion.h"

#include <QCborMap>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <optional>

class MetadataService;
class QAbstractItemView;
class QCompleter;
class QKeyEvent;
class QModelIndex;
class QStandardItemModel;
class SqlEditor;

// SQL 에디터 자동완성.
// - 입력하는 동안(두 글자 이상, 점 바로 뒤, FROM 뒤 한 글자) 또는 Ctrl+Space로 목록을 띄운다.
// - 문맥에 맞춰 제안한다: FROM/JOIN 뒤는 테이블·뷰·스키마, SELECT/WHERE 뒤는 문장에 나온 테이블의 컬럼과
//   별칭·함수·키워드, "별칭." 뒤는 그 테이블의 컬럼, "스키마." 뒤는 그 스키마의 테이블.
// - 메타데이터는 MetadataService 캐시에서 꺼내고, 없으면 뒤에서 가져와 목록을 새로 그린다
//   (비밀번호를 묻지 않는다: 저장했거나 이미 입력한 연결만).
// - Enter/Tab으로 넣고, Esc로 닫는다. 키워드·함수는 입력한 글자의 대소문자를 따른다.
// - 외래 키로 JOIN 잇기: "JOIN dept d ON " 뒤에는 앞의 테이블과 외래 키로 이은 조건(d.DEPT_ID = e.DEPT_ID)을,
//   "JOIN " 뒤에는 앞의 테이블과 외래 키로 이어진 테이블을 별칭·ON 조건까지(DEPT d ON d.DEPT_ID = e.DEPT_ID) 맨 위에 제안한다.
//   ON·JOIN 다음에 띄어쓰면 이런 제안이 있을 때만 목록이 저절로 열린다. 외래 키는 엔진 tableInfo로 읽어 캐시한다.
class SqlCompleter : public QObject
{
    Q_OBJECT

public:
    explicit SqlCompleter(SqlEditor *editor);

    // 메타데이터를 가져올 연결. 비어 있으면 키워드와 함수만 제안한다.
    void setConnection(MetadataService *metadata, const QString &connectionId);
    // MySQL·MariaDB에서 USE로 바꾼 데이터베이스 (비어 있으면 접속한 스키마)
    void setCurrentSchema(const QString &schema);
    // 탭이 접속한 뒤처럼 이제 메타데이터를 가져올 수 있을 때 (실패 뒤 기다리던 것을 풀어 준다)
    void retryMetadata();

    bool isPopupVisible() const;
    QAbstractItemView *popup() const;
    QStringList suggestions() const; // 지금 목록에 보이는 항목 (테스트용)

    // SqlEditor가 부른다. keyPressBefore가 true면 에디터는 그 키를 처리하지 않는다 (목록이 처리).
    bool keyPressBefore(QKeyEvent *event);
    void keyPressAfter(QKeyEvent *event);
    void trigger(bool explicitRequest);
    void hide();

private:
    struct Item
    {
        QString text;   // 목록에 보이는 이름
        QString insert; // 넣을 글자
        QString detail; // 흐리게 보이는 보조 정보 (타입, 테이블 등)
        int kind = 0;
        int group = 0;  // 정렬 순서 (문맥별)
        int caretBack = 0; // 넣은 뒤 커서를 앞으로 옮길 글자 수 (함수 괄호 안)
    };

    void update(bool explicitRequest);
    void insert(const QModelIndex &index);
    QList<Item> collect(const SqlCompletionContext &context);
    void addKeywords(QList<Item> &items, const QString &prefix, int group);
    void addFunctions(QList<Item> &items, const QString &prefix, int group);
    void addObjects(QList<Item> &items, const QString &schema, int group);
    void addColumns(QList<Item> &items, const QString &schema, const QString &table, int group);
    void addJoinConditions(QList<Item> &items, const SqlCompletionContext &context, int group);
    void addJoinTables(QList<Item> &items, const SqlCompletionContext &context, int group);
    // 테이블의 외래 키 (tableInfo). 아직 모르면 가져오기를 시작하고 빈 값
    std::optional<QCborMap> tableInfo(const QString &schema, const QString &table);

    // 캐시에서 꺼내기. 없으면 가져오기를 시작하고 빈 값을 돌려준다.
    QStringList schemas();
    QString currentSchema();
    QString actualSchema(const QString &name);
    QString actualTable(const QString &schema, const QString &name);
    bool metadataAvailable();
    std::optional<QCborMap> fetch(const QString &method, const QCborMap &params);

    SqlEditor *m_editor = nullptr;
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_model = nullptr;
    QPointer<MetadataService> m_metadata;
    QString m_connectionId;
    QString m_currentSchema;
    int m_regionStart = 0;          // 분석한 문장이 문서에서 시작하는 위치
    SqlCompletionContext m_context; // 마지막으로 분석한 문맥
    bool m_explicit = false;        // Ctrl+Space로 연 목록 (빈 접두어에도 유지)
    bool m_waitingForMetadata = false;
    bool m_joinOnly = false;        // ON·JOIN 뒤에 띄어써서 연 목록: 외래 키 제안만
    QSet<QString> m_failedTables;   // tableInfo를 읽지 못한 테이블 (다시 묻지 않는다)
    QElapsedTimer m_failedAt;       // 메타데이터 가져오기에 실패하면 잠시 다시 시도하지 않는다
};
