package dbaro.engine.ipc

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException

/** 메시지 하나 = 4바이트 길이(빅엔디언) + CBOR 본문. */
object Framing {
    private const val MAX_FRAME = 256 * 1024 * 1024

    /** 다음 메시지를 읽는다. 입력이 끝났으면 null. */
    fun read(input: DataInputStream): ByteArray? {
        val length = try {
            input.readInt()
        } catch (e: EOFException) {
            return null
        }
        require(length in 0..MAX_FRAME) { "Invalid frame length: $length" }
        val data = ByteArray(length)
        input.readFully(data)
        return data
    }

    fun write(output: DataOutputStream, data: ByteArray) {
        output.writeInt(data.size)
        output.write(data)
        output.flush()
    }
}
