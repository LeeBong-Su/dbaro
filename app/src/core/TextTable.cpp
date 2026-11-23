#include "TextTable.h"

#include <QChar>

namespace {

int charWidth(char32_t c)
{
    if (c == 0)
        return 0;
    const QChar::Category category = QChar::category(c);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing || category == QChar::Other_Format)
        return 0;
    // 동아시아 넓은 글자 (Unicode East Asian Width W·F의 주요 구간)
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF)
        || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF)
        || (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0xFE10 && c <= 0xFE19) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60)
        || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF)
        || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

QString pad(const QString &text, int width, bool right)
{
    const QString fill(qMax(0, width - TextTable::displayWidth(text)), u' ');
    return right ? fill + text : text + fill;
}

} // namespace

int TextTable::displayWidth(const QString &text)
{
    int width = 0;
    for (const char32_t c : text.toUcs4())
        width += charWidth(c);
    return width;
}

QString TextTable::fitCell(const QString &text, int maxWidth)
{
    QString flat = text;
    for (QChar &ch : flat) {
        if (ch == u'\n' || ch == u'\r' || ch == u'\t')
            ch = u' ';
    }
    if (maxWidth <= 0 || displayWidth(flat) <= maxWidth)
        return flat;
    QString out;
    int width = 0;
    for (const char32_t c : flat.toUcs4()) {
        const int w = charWidth(c);
        if (width + w > maxWidth - 1)
            break;
        out += QString::fromUcs4(&c, 1);
        width += w;
    }
    return out + QStringLiteral("…");
}

QString TextTable::render(const QStringList &headers, const QList<QStringList> &rows, const QList<bool> &rightAlign,
                          int maxCellWidth, bool showHeader)
{
    const int columns = int(headers.size());
    QList<QStringList> cells;
    cells.reserve(rows.size());
    QList<int> widths(columns, 0);
    QStringList head;
    for (int c = 0; c < columns; ++c) {
        head << fitCell(headers[c], maxCellWidth);
        widths[c] = showHeader ? displayWidth(head[c]) : 1;
    }
    for (const QStringList &row : rows) {
        QStringList fitted;
        for (int c = 0; c < columns; ++c) {
            fitted << fitCell(c < row.size() ? row[c] : QString(), maxCellWidth);
            widths[c] = qMax(widths[c], displayWidth(fitted[c]));
        }
        cells.append(fitted);
    }

    QString border = QStringLiteral("+");
    for (int c = 0; c < columns; ++c)
        border += QString(widths[c] + 2, u'-') + u'+';
    border += u'\n';

    const auto line = [&](const QStringList &values, bool header) {
        QString text = QStringLiteral("|");
        for (int c = 0; c < columns; ++c) {
            const bool right = !header && c < rightAlign.size() && rightAlign[c];
            text += u' ' + pad(values[c], widths[c], right) + QStringLiteral(" |");
        }
        return text + u'\n';
    };

    if (!showHeader) {
        if (cells.isEmpty())
            return {};
        QString out = border;
        for (const QStringList &row : std::as_const(cells))
            out += line(row, false);
        return out + border;
    }
    QString out = border + line(head, true) + border;
    for (const QStringList &row : std::as_const(cells))
        out += line(row, false);
    if (!cells.isEmpty())
        out += border;
    return out;
}
