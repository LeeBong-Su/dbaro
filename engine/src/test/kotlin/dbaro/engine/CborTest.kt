package dbaro.engine

import dbaro.engine.ipc.Cbor
import java.math.BigDecimal
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals

class CborTest {
    @Test
    fun roundTripsProtocolValues() {
        val value = mapOf(
            "small" to 5L,
            "byte" to 200L,
            "short" to 60_000L,
            "int" to 4_000_000_000L,
            "long" to Long.MAX_VALUE,
            "negative" to -1L,
            "minLong" to Long.MIN_VALUE,
            "double" to 1.25,
            "text" to "한글 SELECT 'a;b'",
            "empty" to "",
            "null" to null,
            "true" to true,
            "false" to false,
            "list" to listOf(1L, "two", null, listOf(3L)),
            "nested" to mapOf("k" to "v"),
        )
        assertEquals(value, Cbor.decode(Cbor.encode(value)))
    }

    @Test
    fun encodesBytesAndBigDecimal() {
        val decoded = Cbor.decode(Cbor.encode(mapOf("bytes" to byteArrayOf(0, 1, -1), "decimal" to BigDecimal("12.30")))) as Map<*, *>
        assertContentEquals(byteArrayOf(0, 1, -1), decoded["bytes"] as ByteArray)
        assertEquals("12.30", decoded["decimal"])
    }

    @Test
    fun decodesHalfAndSingleFloats() {
        assertEquals(1.5, Cbor.decode(byteArrayOf(0xf9.toByte(), 0x3e, 0x00)))
        assertEquals(0.5, Cbor.decode(byteArrayOf(0xfa.toByte(), 0x3f, 0x00, 0x00, 0x00)))
    }
}
