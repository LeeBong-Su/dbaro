#pragma once

#include "core/ConnectSpec.h"
#include "core/ConnectionProfile.h"

#include <QPlainTextEdit>
#include <QStringList>
#include <QTextCharFormat>
#include <QWidget>

class CliApp;
class EngineClient;
class QComboBox;
class QLabel;
class QToolButton;

// 터미널처럼 쓰는 화면: 출력 맨 아래의 프롬프트 뒤에서만 입력할 수 있다.
// 프롬프트 앞(지난 출력)은 고를 수 있지만 고칠 수 없고, 거기서 글자를 치면 입력 자리로 옮겨 간다.
// 깜박이는 커서는 입력 자리에만 보인다 (지난 출력을 눌러도 커서는 입력 자리로 돌아간다).
// 프롬프트가 보이는 동안 들어온 출력은 프롬프트 줄 위에 끼워 넣는다.
// Ctrl(또는 Alt)+드래그는 사각형(세로) 선택: 표의 컬럼처럼 화면에 보이는 사각형만 골라 복사한다.
class ConsoleView : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit ConsoleView(QWidget *parent = nullptr);

    void appendOutput(const QString &text, const QTextCharFormat &format);
    void showPrompt(const QString &prompt, const QTextCharFormat &format);
    // 입력 줄을 끝낸다 (suffix를 붙이고 줄을 바꾼다). 프롬프트는 다음 showPrompt까지 없다.
    void commitInput(const QString &suffix = {}, const QTextCharFormat &suffixFormat = {});
    bool promptShown() const { return m_promptShown; }
    QString promptText() const { return m_promptShown ? m_prompt : QString(); }
    QString input() const;
    void setInput(const QString &text);
    void setInputFormat(const QTextCharFormat &format) { m_inputFormat = format; }
    void clearScreen(); // 쓰던 입력은 남긴다
    // 사각형 선택 (Ctrl·Alt+드래그). 줄마다 고른 글을 줄바꿈으로 이은 것
    bool hasBlockSelection() const { return !m_blockCursors.isEmpty(); }
    QString blockSelectionText() const;
    void selectBlock(const QPoint &from, const QPoint &to); // 화면(viewport) 좌표
    void clearBlockSelection();

signals:
    void submitted();                 // Enter
    void interruptRequested();        // 고른 글자 없이 Ctrl+C
    void historyRequested(int step);  // 위(-1) / 아래(+1)
    void linesPasted(const QStringList &lines, const QString &rest); // 여러 줄 붙여 넣기

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override; // Ctrl + 휠: 글자 크기
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    bool selectionInInput() const;
    void moveToInputEnd(bool keepScroll = false);
    void trim();
    void updateCaret();
    void updateBlockSelection();
    void copySelection();

    // 화면에 남기는 줄 수. 넘으면 오래된 줄부터 지운다 (결과 하나는 이보다 길어도 끝까지 나온다).
    static constexpr int kMaxBlocks = 200000;
    bool m_blockDragging = false;
    int m_blockAnchorLine = 0;
    int m_blockHeadLine = 0;
    qreal m_blockAnchorX = 0; // 문서 기준 x (가로 스크롤 포함)
    qreal m_blockHeadX = 0;
    QList<QTextCursor> m_blockCursors;
    int m_promptStart = 0;
    int m_inputStart = 0;
    bool m_promptShown = false;
    QString m_prompt;
    QTextCharFormat m_inputFormat;
};

// 연결 창 안의 CLI 탭: dbaro-cli의 대화형 모드를 그대로 쓴다 (같은 명령 \schema, \format ..., 같은 출력).
// GUI의 엔진에 세션을 하나 더 열어서 쿼리 탭과 따로 실행된다.
// - 출력 맨 아래의 프롬프트 뒤에 쓰고 Enter. 문장은 ; 로 끝내고(PL/SQL 블록은 / 줄), 여러 줄을 붙여 넣으면 차례로 실행한다
// - 위/아래: 입력 기록, Ctrl+C: 실행 중이면 취소·아니면 쓰던 문장 버리기(글자를 고른 채면 복사), Esc: 입력 지우기, Ctrl+L: 화면 지우기
// - Scripts: 이 DBMS의 스크립트 폴더를 정하고 그 안의 .sql 파일을 고르면 @파일명으로 실행한다
class ConsoleTab : public QWidget
{
    Q_OBJECT

public:
    ConsoleTab(EngineClient *engine, const ConnectionProfile &profile, QWidget *parent = nullptr);
    ~ConsoleTab() override;

    // 접속한다. 실패하면 화면에 오류를 남기고 Reconnect로 다시 할 수 있다.
    void start(const ConnectSpec &spec);
    // 입력하고 Enter를 누른 것처럼 (여러 줄이면 줄마다)
    void submit(const QString &text);

    CliApp *cli() const { return m_cli; }
    ConsoleView *terminal() const { return m_view; }
    QPlainTextEdit *output() const { return m_view; }
    QComboBox *formatBox() const { return m_format; }
    QToolButton *scriptsButton() const { return m_scriptsButton; }
    QString promptText() const;   // 지금 보이는 프롬프트 (실행 중이면 빈 값)
    QString inputText() const;    // 프롬프트 뒤에 쓴 글
    void setInputText(const QString &text);
    bool isBusy() const;
    bool hasPendingTransaction() const;
    bool isConnected() const;
    void focusInput();

signals:
    void closeRequested(); // \q
    void stateChanged();   // 실행 중·트랜잭션 상태가 바뀜 (창의 상태 표시줄)

private:
    enum class Kind { Normal, Error, Note };
    void append(const QString &text, Kind kind);
    void runInput(const QString &line);
    void afterLine();
    void processQueue();
    void showPrompt();
    void updateHeader();
    void interrupt();
    void showHistory(int step);
    void fillScriptsMenu(QMenu *menu);
    QTextCharFormat format(Kind kind, bool table) const;
    static QStringList tableFontFamilies();

    CliApp *m_cli = nullptr;
    EngineClient *m_engine = nullptr;
    ConnectionProfile m_profile;
    ConnectSpec m_spec;
    bool m_connecting = false;
    QStringList m_queue;   // 아직 넘기지 않은 줄 (여러 줄 붙여 넣기)
    QString m_tail;        // 붙여 넣은 글의 마지막 줄 (다 실행한 뒤 입력 자리에 남긴다)
    bool m_hasTail = false;
    QStringList m_history; // 입력 기록
    int m_historyIndex = -1;
    QString m_draft;       // 기록을 보기 전에 쓰던 입력

    QLabel *m_title = nullptr;
    QComboBox *m_format = nullptr;
    QToolButton *m_stopButton = nullptr;
    QToolButton *m_clearButton = nullptr;
    QToolButton *m_reconnectButton = nullptr;
    QToolButton *m_scriptsButton = nullptr;
    ConsoleView *m_view = nullptr;
};
