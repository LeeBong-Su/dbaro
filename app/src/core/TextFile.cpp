#include "TextFile.h"

#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringEncoder>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
} // namespace

bool TextFile::read(const QString &path, QString *text, TextFileFormat *format, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    TextFileFormat detected;

    QString decoded;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        detected.encoding = TextFileFormat::Encoding::Utf8;
        detected.bom = true;
        decoded = QString::fromUtf8(bytes.mid(3));
    } else if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        detected.encoding = TextFileFormat::Encoding::Utf16;
        detected.bom = true;
        QStringDecoder decoder(QStringConverter::Utf16); // BOM으로 바이트 순서를 정한다
        decoded = decoder(bytes);
    } else {
        QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        decoded = utf8(bytes);
        if (utf8.hasError()) {
            // UTF-8이 아니다: Toad 등에서 저장한 오래된 파일은 대개 OS 코드 페이지(CP949)다.
            detected.encoding = TextFileFormat::Encoding::System;
            QStringDecoder system(QStringConverter::System);
            decoded = system(bytes);
        }
    }

    detected.crlf = decoded.contains(QStringLiteral("\r\n"));
    decoded.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    decoded.replace(u'\r', u'\n');
    *text = decoded;
    *format = detected;
    return true;
}

bool TextFile::write(const QString &path, const QString &text, const TextFileFormat &format, QString *error,
                     bool *unencodable)
{
    if (unencodable)
        *unencodable = false;
    QString content = text;
    if (format.crlf)
        content.replace(u'\n', QStringLiteral("\r\n"));

    QByteArray bytes;
    switch (format.encoding) {
    case TextFileFormat::Encoding::Utf8:
        if (format.bom)
            bytes = QByteArray("\xEF\xBB\xBF");
        bytes += content.toUtf8();
        break;
    case TextFileFormat::Encoding::Utf16: {
        QStringEncoder encoder(QStringConverter::Utf16LE);
        bytes = QByteArray("\xFF\xFE");
        bytes += QByteArray(encoder(content));
        break;
    }
    case TextFileFormat::Encoding::System: {
        QStringEncoder encoder(QStringConverter::System, QStringConverter::Flag::Stateless);
        bytes = encoder(content);
        // 코드 페이지에 없는 글자는 '?'로 바뀐다. 다시 읽어서 같은지로 확인한다.
        QStringDecoder decoder(QStringConverter::System);
        if (encoder.hasError() || QString(decoder(bytes)) != content) {
            if (unencodable)
                *unencodable = true;
            setError(error, QStringLiteral("Some characters cannot be saved in %1.").arg(encodingName(format)));
            return false;
        }
        break;
    }
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    file.write(bytes);
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

QString TextFile::encodingName(const TextFileFormat &format)
{
    switch (format.encoding) {
    case TextFileFormat::Encoding::Utf8:
        return format.bom ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8");
    case TextFileFormat::Encoding::Utf16:
        return QStringLiteral("UTF-16");
    case TextFileFormat::Encoding::System:
#ifdef Q_OS_WIN
        return QStringLiteral("CP%1").arg(GetACP());
#else
        return QStringLiteral("System");
#endif
    }
    return {};
}

QString TextFile::toKey(TextFileFormat::Encoding encoding)
{
    switch (encoding) {
    case TextFileFormat::Encoding::Utf8: return QStringLiteral("utf8");
    case TextFileFormat::Encoding::Utf16: return QStringLiteral("utf16");
    case TextFileFormat::Encoding::System: return QStringLiteral("system");
    }
    return QStringLiteral("utf8");
}

TextFileFormat::Encoding TextFile::fromKey(const QString &key)
{
    if (key == u"utf16")
        return TextFileFormat::Encoding::Utf16;
    if (key == u"system")
        return TextFileFormat::Encoding::System;
    return TextFileFormat::Encoding::Utf8;
}
