package dbaro.engine.exec

import java.sql.ResultSet
import java.sql.SQLException
import java.sql.Types
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

/**
 * JDBC 값을 앱으로 보낼 값으로 바꾼다.
 * - 정수 → Long, 실수 → Double, 불리언 → Boolean
 * - NUMERIC/DECIMAL → 문자열 (Oracle NUMBER 같은 큰 수와 소수 자릿수를 그대로 지키려고)
 * - 날짜·시간 → "yyyy-MM-dd HH:mm:ss[.fffffffff]" 형식 문자열
 * - 바이너리 → 바이트열 (최대 64KB), 긴 문자열·CLOB → 최대 64K 글자
 */
object Values {
    private const val MAX_TEXT = 65_536
    private const val MAX_BYTES = 65_536
    private val SECONDS = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")

    private val NUMERIC_TYPES = setOf(
        Types.TINYINT, Types.SMALLINT, Types.INTEGER, Types.BIGINT,
        Types.REAL, Types.FLOAT, Types.DOUBLE, Types.NUMERIC, Types.DECIMAL,
    )

    fun isNumeric(type: Int): Boolean = type in NUMERIC_TYPES

    fun read(resultSet: ResultSet, index: Int, type: Int): Any? = try {
        readValue(resultSet, index, type)
    } catch (e: SQLException) {
        "<${e.message}>"
    }

    private fun readValue(rs: ResultSet, i: Int, type: Int): Any? = when (type) {
        Types.BOOLEAN -> rs.getBoolean(i).takeUnless { rs.wasNull() }
        // MySQL·MariaDB BIT(n>1)은 바이트로 온다 → 0과 1의 글 (BIT(1)과 PostgreSQL boolean은 불리언)
        Types.BIT -> when (val value = rs.getObject(i)) {
            null -> null
            is Boolean -> value
            is ByteArray -> value.joinToString("") { Integer.toBinaryString(it.toInt() and 0xFF).padStart(8, '0') }
            else -> value.toString()
        }
        Types.TINYINT, Types.SMALLINT, Types.INTEGER -> rs.getLong(i).takeUnless { rs.wasNull() }
        // MySQL·MariaDB BIGINT UNSIGNED는 Long을 넘을 수 있다 → 그때는 문자열
        Types.BIGINT -> try {
            rs.getLong(i).takeUnless { rs.wasNull() }
        } catch (e: SQLException) {
            rs.getString(i)
        }
        Types.REAL, Types.FLOAT, Types.DOUBLE -> rs.getDouble(i).takeUnless { rs.wasNull() }
        Types.NUMERIC, Types.DECIMAL -> rs.getBigDecimal(i)?.toPlainString()
        Types.DATE -> rs.getDate(i)?.toLocalDate()?.toString()
        Types.TIME -> rs.getTime(i)?.toLocalTime()?.toString()
        Types.TIMESTAMP -> rs.getTimestamp(i)?.let { format(it.toLocalDateTime()) }
        Types.BINARY, Types.VARBINARY, Types.LONGVARBINARY -> rs.getBytes(i)?.let { if (it.size > MAX_BYTES) it.copyOf(MAX_BYTES) else it }
        Types.BLOB -> rs.getBlob(i)?.let { blob -> blob.getBytes(1, minOf(blob.length(), MAX_BYTES.toLong()).toInt()) }
        Types.CLOB, Types.NCLOB -> rs.getClob(i)?.let { clob -> clob.getSubString(1, minOf(clob.length(), MAX_TEXT.toLong()).toInt()) }
        else -> rs.getString(i)?.let { if (it.length > MAX_TEXT) it.substring(0, MAX_TEXT) else it }
    }

    private fun format(value: LocalDateTime): String {
        val seconds = value.format(SECONDS)
        if (value.nano == 0) return seconds
        return seconds + "." + "%09d".format(value.nano).trimEnd('0')
    }
}
