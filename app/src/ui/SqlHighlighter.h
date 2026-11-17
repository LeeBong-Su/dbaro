#pragma once

#include "core/SqlDialect.h"

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// SQL 문법 하이라이트. 키워드, 자료형, 문자열, 숫자, 주석, 인용 식별자, 바인드 변수를 칠한다.
// 여러 줄에 걸친 주석·문자열·$$ 본문·q'[...]'도 줄 상태로 이어서 처리한다.
class SqlHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit SqlHighlighter(QTextDocument *document);

    void setDialect(SqlDialect dialect);
    void setDarkTheme(bool dark);

protected:
    void highlightBlock(const QString &text) override;

private:
    enum State { Normal = 0, BlockComment, SingleQuote, EscapedQuote, DoubleQuote, Backtick, Dollar, QQuote };

    void setupFormats(bool dark);
    void continueState(int state, const QString &closer);

    SqlDialect m_dialect = SqlDialect::Generic;
    QTextCharFormat m_keyword;
    QTextCharFormat m_type;
    QTextCharFormat m_string;
    QTextCharFormat m_number;
    QTextCharFormat m_comment;
    QTextCharFormat m_identifier;
    QTextCharFormat m_parameter;
};
