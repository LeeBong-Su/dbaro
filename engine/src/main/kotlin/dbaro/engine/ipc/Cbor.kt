package dbaro.engine.ipc

import java.io.ByteArrayOutputStream
import java.math.BigDecimal
import java.math.BigInteger

/**
 * 앱과 주고받는 메시지 형식 (CBOR, RFC 8949).
 * 프로토콜에 필요한 부분만 구현한다: 정수, 실수, 문자열, 바이트열, 배열, 문자열 키 맵, true/false/null.
 * 길이가 정해진 항목만 다룬다 (Qt의 QCborValue::toCbor도 그렇게 쓴다).
 */
object Cbor {

    fun encode(value: Any?): ByteArray {
        val out = ByteArrayOutputStream()
        write(out, value)
        return out.toByteArray()
    }

    fun decode(bytes: ByteArray): Any? = Reader(bytes).read()

    private fun write(out: ByteArrayOutputStream, value: Any?) {
        when (value) {
            null -> out.write(0xf6)
            is Boolean -> out.write(if (value) 0xf5 else 0xf4)
            is Byte, is Short, is Int, is Long -> writeInteger(out, (value as Number).toLong())
            is Float, is Double -> {
                out.write(0xfb)
                val bits = (value as Number).toDouble().toRawBits()
                for (shift in 56 downTo 0 step 8) out.write((bits ushr shift).toInt() and 0xff)
            }
            is BigInteger -> if (value.bitLength() < 64) writeInteger(out, value.toLong()) else writeText(out, value.toString())
            is BigDecimal -> writeText(out, value.toPlainString())
            is String -> writeText(out, value)
            is ByteArray -> {
                writeHead(out, 2, value.size.toLong())
                out.write(value)
            }
            is Map<*, *> -> {
                writeHead(out, 5, value.size.toLong())
                for ((key, item) in value) {
                    writeText(out, key.toString())
                    write(out, item)
                }
            }
            is Collection<*> -> {
                writeHead(out, 4, value.size.toLong())
                for (item in value) write(out, item)
            }
            is Array<*> -> {
                writeHead(out, 4, value.size.toLong())
                for (item in value) write(out, item)
            }
            else -> writeText(out, value.toString())
        }
    }

    private fun writeInteger(out: ByteArrayOutputStream, value: Long) {
        if (value >= 0) writeHead(out, 0, value) else writeHead(out, 1, -1 - value)
    }

    private fun writeText(out: ByteArrayOutputStream, text: String) {
        val bytes = text.toByteArray(Charsets.UTF_8)
        writeHead(out, 3, bytes.size.toLong())
        out.write(bytes)
    }

    private fun writeHead(out: ByteArrayOutputStream, major: Int, length: Long) {
        val type = major shl 5
        when {
            length < 24 -> out.write(type or length.toInt())
            length < 0x100 -> {
                out.write(type or 24)
                out.write(length.toInt())
            }
            length < 0x10000 -> {
                out.write(type or 25)
                for (shift in 8 downTo 0 step 8) out.write((length ushr shift).toInt() and 0xff)
            }
            length < 0x100000000L -> {
                out.write(type or 26)
                for (shift in 24 downTo 0 step 8) out.write((length ushr shift).toInt() and 0xff)
            }
            else -> {
                out.write(type or 27)
                for (shift in 56 downTo 0 step 8) out.write((length ushr shift).toInt() and 0xff)
            }
        }
    }

    private class Reader(private val bytes: ByteArray) {
        private var pos = 0

        fun read(): Any? {
            val initial = byte()
            val major = initial ushr 5
            val info = initial and 0x1f
            return when (major) {
                0 -> argument(info)
                1 -> -1L - argument(info)
                2 -> take(length(info))
                3 -> String(take(length(info)), Charsets.UTF_8)
                4 -> {
                    val count = length(info)
                    List(count) { read() }
                }
                5 -> {
                    val count = length(info)
                    val map = LinkedHashMap<String, Any?>(count * 2)
                    repeat(count) {
                        val key = read().toString()
                        map[key] = read()
                    }
                    map
                }
                6 -> {
                    argument(info) // 태그 번호는 무시하고 내용만 읽는다
                    read()
                }
                else -> when (info) {
                    20 -> false
                    21 -> true
                    22, 23 -> null
                    25 -> halfToDouble(argument(25).toInt())
                    26 -> Float.fromBits(argument(26).toInt()).toDouble()
                    27 -> Double.fromBits(argument(27))
                    else -> throw IllegalArgumentException("Unsupported CBOR simple value: $info")
                }
            }
        }

        private fun byte(): Int {
            if (pos >= bytes.size) throw IllegalArgumentException("Truncated CBOR data")
            return bytes[pos++].toInt() and 0xff
        }

        private fun argument(info: Int): Long = when {
            info < 24 -> info.toLong()
            info == 24 -> byte().toLong()
            info == 25 -> (byte().toLong() shl 8) or byte().toLong()
            info == 26 -> {
                var value = 0L
                repeat(4) { value = (value shl 8) or byte().toLong() }
                value
            }
            info == 27 -> {
                var value = 0L
                repeat(8) { value = (value shl 8) or byte().toLong() }
                value
            }
            else -> throw IllegalArgumentException("Unsupported CBOR length encoding: $info")
        }

        private fun length(info: Int): Int {
            val value = argument(info)
            if (value < 0 || value > Int.MAX_VALUE) throw IllegalArgumentException("Invalid CBOR length")
            return value.toInt()
        }

        private fun take(count: Int): ByteArray {
            if (count < 0 || pos + count > bytes.size) throw IllegalArgumentException("Truncated CBOR data")
            val result = bytes.copyOfRange(pos, pos + count)
            pos += count
            return result
        }

        private fun halfToDouble(half: Int): Double {
            val sign = if (half and 0x8000 != 0) -1.0 else 1.0
            val exponent = (half ushr 10) and 0x1f
            val fraction = half and 0x3ff
            return when (exponent) {
                0 -> sign * Math.scalb(fraction.toDouble(), -24)
                31 -> if (fraction == 0) sign * Double.POSITIVE_INFINITY else Double.NaN
                else -> sign * Math.scalb((fraction + 1024).toDouble(), exponent - 25)
            }
        }
    }
}
