#pragma once

#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"
#include "core/QueryHistory.h"
#include "core/SqlDialect.h"

#include <QCborArray>
#include <QCborMap>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class ConnectionStore;
class EngineClient;
struct EngineError;
struct SqlStatement;

// dbaro-cli: 저장된 연결(GUI와 같은 connections.json, 비밀번호, 실행 기록)로 SQL을 실행하는 명령줄 도구.
// 같은 코드를 GUI 안의 CLI 탭(ConsoleTab)도 쓴다. 이때는 GUI의 엔진에 세션을 하나 더 열고, 입력한 줄을 submitLine으로 받는다.
//
//   dbaro-cli -l                              저장된 연결 목록
//   dbaro-cli dev "select * from emp"         한 번 실행하고 끝 (여러 문장 가능)
//   dbaro-cli -c dev -f script.sql --format csv -o out.csv
//   dbaro-cli dev                             대화형 (문장은 ; 로 끝, PL/SQL 블록은 / 줄)
//   type script.sql | dbaro-cli dev           표준 입력의 스크립트 실행
//
// 대화형과 스크립트 파일은 한 줄씩 같은 길로 처리한다 (SQL*Plus처럼):
// - @이름 [인자...], START 이름: DBMS별 스크립트 폴더(ScriptFolders)에서 찾아 실행 (.sql은 생략 가능). @@이름은 부른 스크립트 옆 파일
// - 인자는 &1, &2 ...로, DEFINE으로 정한 값은 &이름으로 넣는다 (Oracle·Tibero 치환 변수)
// - SQL*Plus 명령: PROMPT, REM, SET ECHO/DEFINE/FEEDBACK/AUTOCOMMIT(나머지 SET은 무시), WHENEVER SQLERROR, DEFINE, UNDEFINE,
//   EXEC, EXIT(스크립트 끝내기). MySQL·MariaDB는 DELIMITER
// - 문장 끝을 \G로 쓰면 결과를 세로(레코드)로 (MySQL처럼)
//
// 엔진과는 모두 비동기로 주고받는다. 명령줄 프로그램은 한 동작이 끝날 때까지 이벤트 루프를 돌리며 기다리고,
// GUI 탭은 기다리지 않는다 (화면이 멈추지 않는다).
// 끝 코드: 0 성공, 1 실패한 문장이 있음, 2 사용법·연결 오류
class CliApp : public QObject
{
    Q_OBJECT

public:
    enum class Format { Table, Csv, Tsv, Json };
    using Writer = std::function<void(const QString &text)>;
    using LineReader = std::function<bool(QString *line)>;
    using Confirm = std::function<bool(const QString &question)>;

    explicit CliApp(QObject *parent = nullptr);
    ~CliApp() override;

    // ---- 명령줄 프로그램
    int run(const QStringList &arguments);
    static QString usage();

    // 입출력 (기본은 콘솔). 테스트 도구와 GUI 탭이 바꾼다.
    void setOutput(Writer out, Writer err);
    void setInput(LineReader readLine, LineReader readHidden, bool interactive);
    void setSpecOverride(std::function<void(const ConnectionProfile &, ConnectSpec *)> override);

    // ---- GUI 안의 CLI 탭
    // engine은 GUI가 가진 엔진 (끝낼 때 멈추지 않는다). 접속하면 안내 글을 쓰고 done(성공 여부).
    void startEmbedded(EngineClient *engine, const ConnectionProfile &profile, const ConnectSpec &spec,
                       std::function<void(bool ok)> done);
    // 한 줄 처리: 명령(\...)이면 실행, 문장이 끝났으면(; 나 / 줄) 실행, 아니면 다음 줄을 기다린다. 다 되면 done.
    void submitLine(const QString &line, std::function<void()> done);
    void discardInput(); // 쓰던 문장을 버린다 (Ctrl+C)
    QString prompt() const; // 지금 줄의 프롬프트. 이어 쓰는 중이면 "-> "
    bool hasPartialInput() const { return !m_buffer.isEmpty(); }
    bool isBusy() const { return m_busy; }
    bool isConnected() const { return !m_sessionId.isEmpty(); }
    bool hasPendingTransaction() const { return m_pendingTransaction; }
    bool autoCommit() const { return m_autoCommit; }
    QString currentSchema() const { return m_currentSchema; }
    QString serverInfo() const { return m_serverInfo; }
    Format format() const { return m_format; }
    void setFormat(Format format) { m_format = format; }
    // 실행 기록을 파일 대신 이 함수로 (GUI는 F8 목록에 바로 보이도록 AppController의 기록에 넣는다)
    void setHistorySink(std::function<void(const HistoryEntry &)> sink) { m_historySink = std::move(sink); }
    // WHERE 없는 UPDATE·DELETE, 운영 DB의 DROP·TRUNCATE 전에 묻는다. 없으면 묻지 않는다.
    void setConfirm(Confirm confirm) { m_confirm = std::move(confirm); }
    // 값을 묻는다 (ACCEPT, 정하지 않은 치환 변수 &이름). prompt는 보여 줄 글, hide면 입력을 가린다. 없으면 오류
    using AskValue = std::function<bool(const QString &prompt, bool hide, QString *value)>;
    void setAskValue(AskValue ask) { m_askValue = std::move(ask); }
    // 이 연결의 DBMS 스크립트 폴더
    QString scriptFolder() const;
    bool isRunningScript() const { return !m_frames.isEmpty(); }
    void disconnectSession();

    static QString formatName(Format format);
    static bool parseFormat(const QString &text, Format *format);

public slots:
    void cancelRunning(); // Ctrl+C: 실행 중인 문장을 취소한다

signals:
    void busyChanged(bool busy);
    void stateChanged(); // 프롬프트·스키마·트랜잭션 상태가 바뀜
    void quitRequested(); // \q

private:
    struct ScriptRun;
    struct StatementRun;
    struct ScriptFrame;
    using FramePtr = std::shared_ptr<ScriptFrame>;
    using Reply = std::function<void(const QCborMap &result, const EngineError &error)>;

    void request(const QString &method, const QCborMap &params, Reply reply);
    void await(const std::function<void(std::function<void()> finish)> &start); // 명령줄 프로그램 전용
    const ConnectionProfile *findConnection(const QString &key, QString *error) const;
    bool passwordFor(const ConnectionProfile &profile, QString *password);
    void connectAsync(const ConnectSpec &spec, std::function<void(bool ok)> done);

    // script를 문장으로 나눠 차례로 실행한다. frame·firstLine은 오류 위치(파일:줄)와 집계, vertical은 마지막 문장을 세로로
    void runScriptAsync(const QString &script, std::function<void(bool ok)> done, const FramePtr &frame = {},
                        int firstLine = 0, bool vertical = false);
    void runNext(const std::shared_ptr<ScriptRun> &script);
    void runStatementAsync(const SqlStatement &statement, bool vertical, const QCborArray &binds, const QList<int> &outVars,
                           std::function<void(bool ok)> done);
    // :이름 바인드 변수(VARIABLE로 정한 것)를 ?로 바꾸고 값을 붙인다. PL/SQL 블록은 바뀐 값을 돌려받게 감싼다
    bool prepareBinds(SqlStatement *statement, QCborArray *binds, QList<int> *outVars);
    void handleVariable(const QString &rest, std::function<void(bool ok)> done);
    void handlePrint(const QString &rest, std::function<void(bool ok)> done);
    void handleAccept(const QString &rest, std::function<void(bool ok)> done);
    int findBindVar(const QString &name) const;
    // 한 줄 처리 (대화형은 m_buffer, 스크립트는 frame의 buffer)
    void handleLine(const QString &line, QString *buffer, int lineNumber, const FramePtr &frame,
                    std::function<void(bool ok)> done);
    bool takeStatement(const QString &buffer, QString *script, bool *vertical) const;
    QString withDelimiter(const QString &text) const; // MySQL DELIMITER를 바꿨으면 앞에 붙인다
    bool isScriptCommand(const QString &trimmed) const;
    bool isClientCommand(const QString &trimmed) const;
    void runScriptCommand(const QString &trimmed, const FramePtr &parent, std::function<void(bool ok)> done);
    void handleClientCommand(const QString &trimmed, const FramePtr &frame, std::function<void(bool ok)> done);
    void runFileText(const QString &text, const QString &path, const QStringList &args, std::function<void(bool ok)> done);
    void runFrameNext(const FramePtr &frame);
    void finishFrame(const FramePtr &frame);
    void abortScripts();
    bool substitute(SqlStatement *statement, const FramePtr &frame);
    QString substituteText(const QString &text, const FramePtr &frame) const;
    void printRecords(const QCborArray &columns, const QList<QCborArray> &rows);
    void fetchRest(const std::shared_ptr<StatementRun> &run);
    void finishResult(const std::shared_ptr<StatementRun> &run);
    bool confirmSafe(const QList<SqlStatement> &statements);
    void printResult(const QCborArray &columns, const QList<QCborArray> &rows, bool vertical = false);
    void printConnections();
    void openGui(const ConnectionProfile &profile);
    int repl();
    void handleCommand(const QString &line, std::function<void(bool ok)> done);
    bool isCommand(const QString &trimmed) const;
    bool isComplete(const QString &buffer) const;
    void refreshSchemaAsync(std::function<void()> done);
    void recordHistory(const SqlStatement &statement, bool ok, qint64 elapsedMs, qint64 rows, const QString &error);
    void setBusy(bool busy);
    void printBanner();

    void out(const QString &text);  // 결과 (-o면 파일)
    void err(const QString &text);  // 오류·경고
    void info(const QString &text); // 행 수·걸린 시간 (표 형식이면 결과와 함께, 아니면 표준 오류로)

    Writer m_out;
    Writer m_err;
    LineReader m_readLine;
    LineReader m_readHidden;
    bool m_interactiveInput = false;
    std::function<void(const ConnectionProfile &, ConnectSpec *)> m_specOverride;
    std::function<void(const HistoryEntry &)> m_historySink;
    Confirm m_confirm;
    AskValue m_askValue;

    std::unique_ptr<ConnectionStore> m_store;
    std::unique_ptr<QueryHistory> m_history;
    EngineClient *m_engine = nullptr;
    bool m_ownsEngine = false;
    bool m_embedded = false;
    QFile m_outputFile;

    ConnectionProfile m_profile;
    SqlDialect m_dialect = SqlDialect::Generic;
    QString m_sessionId;
    QString m_serverInfo;
    QString m_currentSchema;
    QString m_schemaOption; // --schema
    QString m_buffer;       // 아직 끝나지 않은 문장 (대화형)
    Format m_format = Format::Table;
    int m_maxRows = 1000;     // 0이면 모두
    int m_maxCellWidth = 60;  // 표 칸 하나의 최대 글자 폭 (넘으면 …). 0이면 자르지 않는다
    bool m_stopOnError = true;
    bool m_autoCommit = true;
    bool m_pendingTransaction = false;
    bool m_quiet = false;
    bool m_recordHistory = true;
    bool m_bom = false;
    bool m_busy = false;
    bool m_quit = false;
    bool m_anyFailed = false;
    int m_resultCount = 0;

    // 스크립트 (@파일)와 SQL*Plus 설정
    QList<FramePtr> m_frames;          // 실행 중인 스크립트 (안쪽이 뒤)
    QHash<QString, QString> m_defines; // DEFINE (이름은 대문자)
    QString m_location;                // 지금 문장의 위치 (파일:줄), 오류 글에 붙인다
    QString m_delimiter = QStringLiteral(";"); // MySQL DELIMITER
    bool m_echo = false;               // SET ECHO ON: 스크립트의 문장을 보여 준다
    bool m_feedback = true;            // SET FEEDBACK OFF: 행 수·걸린 시간을 보여 주지 않는다
    bool m_substitution = true;        // SET DEFINE OFF: &를 치환하지 않는다
    bool m_heading = true;             // SET HEADING OFF / SET PAGESIZE 0: 머리글 없이
    bool m_pageSizeZero = false;
    bool m_scriptStopOnError = false;  // 스크립트(@파일) 오류: SQL*Plus처럼 기본은 계속, WHENEVER SQLERROR EXIT면 멈춤
    // VARIABLE로 정한 바인드 변수 (정한 순서)
    struct BindVar
    {
        QString name;  // 대문자
        QString type;  // 정한 그대로 (VARCHAR2(30), NUMBER ...)
        QString kind;  // 엔진에 넘길 종류: VARCHAR, NUMERIC, CLOB
        QCborValue value;
    };
    QList<BindVar> m_bindVars;

    std::atomic<bool> m_running {false};
    std::atomic<bool> m_interrupted {false};
    std::atomic<bool> m_clearInput {false};
};
