package dbaro.engine

import dbaro.engine.driver.DriverRegistry
import org.junit.jupiter.api.Assumptions.assumeTrue
import java.lang.reflect.InvocationTargetException
import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Paths
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertFailsWith

/*
 * Oracle 한글 문자셋(KO16MSWIN949, KO16KSC5601 = EUC-KR) DB는 thin 드라이버가 orai18n.jar로 글자를 바꾼다.
 * 드라이버 폴더(~/.dbaro/drivers)에 두 jar가 이미 있을 때만 돈다 (내려받지 않는다: RealDbTest를 돌리면 생긴다).
 */
class OracleNlsTest {
    private val drivers = Paths.get(System.getProperty("user.home"), ".dbaro", "drivers")
    private val spec = DriverRegistry.BUILT_IN.getValue("oracle")

    private fun converter(jars: List<java.nio.file.Path>, charsetId: Int): (String) -> ByteArray {
        // 부모는 플랫폼 로더 (java.sql은 보이고, 테스트 클래스패스의 드라이버는 보이지 않게)
        val loader = URLClassLoader(jars.map { it.toUri().toURL() }.toTypedArray(), ClassLoader.getPlatformClassLoader())
        val characterSet = loader.loadClass("oracle.sql.CharacterSet")
        val charset = characterSet.getMethod("make", Int::class.javaPrimitiveType).invoke(null, charsetId)
        val convert = characterSet.getMethod("convert", String::class.java)
        return { text ->
            try {
                convert.invoke(charset, text) as ByteArray
            } catch (e: InvocationTargetException) {
                throw e.targetException
            }
        }
    }

    @Test
    fun koreanCharacterSetsNeedOrai18n() {
        val files = (listOf(spec.artifact!!) + spec.companions).map { drivers.resolve(it.fileName) }
        assumeTrue(files.all { Files.isRegularFile(it) }, "Oracle driver jars are not in $drivers")
        val ojdbcOnly = files.take(1)
        // 846 KO16MSWIN949, 840 KO16KSC5601 (EUC-KR)
        for (id in listOf(846, 840)) {
            assertFailsWith<Exception>("charset $id without orai18n") { converter(ojdbcOnly, id)("한글") }
            // orai18n을 함께 두면 바뀐다: "한글"은 두 문자셋 모두 C7 D1 B1 DB
            assertContentEquals(byteArrayOf(0xC7.toByte(), 0xD1.toByte(), 0xB1.toByte(), 0xDB.toByte()), converter(files, id)("한글"))
        }
        // DriverRegistry가 실제로 두 jar를 함께 넘기는지
        assertContentEquals(files.map { it.toAbsolutePath().normalize() }, DriverRegistry(drivers).builtInJars(spec).map { it.toAbsolutePath().normalize() })
    }
}
