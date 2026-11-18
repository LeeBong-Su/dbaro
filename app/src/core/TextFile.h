#pragma once

#include <QString>

// SQL 파일을 읽고 쓸 때의 형식. 읽은 형식 그대로 다시 저장한다.
struct TextFileFormat
{
    enum class Encoding {
        Utf8,    // UTF-8 (BOM은 bom으로)
        Utf16,   // UTF-16 LE (BOM 있음)
        System,  // OS 기본 코드 페이지 (한국어 Windows는 CP949). UTF-8로 읽을 수 없는 오래된 파일
    };

    Encoding encoding = Encoding::Utf8;
    bool bom = false;
    bool crlf = false; // 줄 끝이 CRLF (Windows)

    bool operator==(const TextFileFormat &) const = default;
};

namespace TextFile {

// 인코딩을 알아내서 읽는다: BOM → UTF-8 → OS 코드 페이지 순. 줄 끝은 '\n'으로 바꾼다.
bool read(const QString &path, QString *text, TextFileFormat *format, QString *error = nullptr);

// format대로 쓴다 (임시 파일에 쓴 뒤 바꿔치기). OS 코드 페이지로 쓸 수 없는 글자가 있으면
// 쓰지 않고 *unencodable를 true로 한다 (호출한 쪽에서 UTF-8로 바꿔 다시 쓸지 묻는다).
bool write(const QString &path, const QString &text, const TextFileFormat &format, QString *error = nullptr,
           bool *unencodable = nullptr);

// 상태 표시줄용 이름 (UTF-8, UTF-8 BOM, UTF-16, CP949 ...)
QString encodingName(const TextFileFormat &format);

// 작업 공간 파일에 남길 때 쓰는 이름과 그 반대
QString toKey(TextFileFormat::Encoding encoding);
TextFileFormat::Encoding fromKey(const QString &key);

} // namespace TextFile
