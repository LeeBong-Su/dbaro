package dbaro.engine.driver

import dbaro.engine.Log
import java.io.IOException
import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.sql.Driver
import java.util.concurrent.ConcurrentHashMap

/**
 * JDBC 드라이버를 찾아 불러온다.
 * 1. 연결 설정에 jar 경로가 있으면 그 jar (Tibero는 벤더 독점 드라이버라 항상 이 방식). ';'로 여러 개,
 *    Oracle이면 같은 폴더의 orai18n*.jar도 함께
 * 2. 엔진 클래스패스에 이미 있으면 그대로 (테스트)
 * 3. 없으면 Maven Central에서 내려받아 driverDir에 저장하고 SHA-1을 확인한다 (Oracle은 orai18n도 함께, 받지 못하면 빼고)
 * 드라이버는 jar 묶음마다 별도 클래스 로더로 불러온다. DriverManager를 거치지 않고 Driver.connect를 직접 부른다.
 */
class DriverRegistry(
    private val driverDir: Path,
    private val repositoryUrl: String = MAVEN_CENTRAL,
) {
    data class Artifact(val group: String, val name: String, val version: String) {
        val fileName: String get() = "$name-$version.jar"
        val path: String get() = "${group.replace('.', '/')}/$name/$version/$fileName"
    }

    /**
     * [companions]: 드라이버와 같은 클래스 로더에 함께 넣는 jar. 받지 못해도 접속은 해 본다 (폐쇄망).
     * Oracle orai18n: KO16MSWIN949·KO16KSC5601(EUC-KR)·JA16SJIS 같은 문자셋의 DB에 붙을 때 thin 드라이버가 쓴다.
     */
    data class Spec(val driverClass: String, val artifact: Artifact?, val companions: List<Artifact> = emptyList())

    companion object {
        const val MAVEN_CENTRAL = "https://repo1.maven.org/maven2"
        private const val ORACLE_VERSION = "23.4.0.24.05"

        // 버전을 바꾸면 packaging/windows/README.txt의 폐쇄망 jar 목록도 고친다.
        val BUILT_IN: Map<String, Spec> = mapOf(
            "oracle" to Spec(
                "oracle.jdbc.OracleDriver",
                Artifact("com.oracle.database.jdbc", "ojdbc11", ORACLE_VERSION),
                listOf(Artifact("com.oracle.database.nls", "orai18n", ORACLE_VERSION)),
            ),
            "tibero" to Spec("com.tmax.tibero.jdbc.TbDriver", null),
            "mariadb" to Spec("org.mariadb.jdbc.Driver", Artifact("org.mariadb.jdbc", "mariadb-java-client", "3.4.1")),
            "mysql" to Spec("com.mysql.cj.jdbc.Driver", Artifact("com.mysql", "mysql-connector-j", "8.4.0")),
            "postgres" to Spec("org.postgresql.Driver", Artifact("org.postgresql", "postgresql", "42.7.4")),
        )
    }

    private val loaders = ConcurrentHashMap<List<Path>, ClassLoader>()
    private val downloadLock = Any()
    private val unavailable = ConcurrentHashMap.newKeySet<String>() // 이번 실행에서 받지 못한 함께 쓰는 jar (다시 시도하지 않는다)

    fun driver(type: String, driverPath: String?, driverClass: String?): Driver {
        val spec = BUILT_IN[type]
        val className = driverClass ?: spec?.driverClass
            ?: throw IllegalArgumentException("Unknown database type: $type")

        if (driverPath != null) return instantiate(loaderFor(userJars(type, driverPath)), className)

        loadFromClasspath(className)?.let { return it }

        if (spec?.artifact == null) throw IllegalArgumentException(
            if (type == "tibero") "Select the Tibero JDBC driver (.jar) in the connection settings."
            else "No JDBC driver is configured for $type."
        )
        return instantiate(loaderFor(builtInJars(spec)), className)
    }

    /** 기본 드라이버 jar와 함께 쓰는 jar. 함께 쓰는 jar는 받지 못하면 빼고 로그만 남긴다. */
    internal fun builtInJars(spec: Spec): List<Path> {
        val main = download(spec.artifact ?: throw IllegalArgumentException("No JDBC driver artifact"))
        val companions = spec.companions.mapNotNull { companion ->
            if (companion.fileName in unavailable) return@mapNotNull null
            runCatching { download(companion) }.onFailure {
                unavailable.add(companion.fileName)
                Log.info("Continuing without ${companion.fileName}: ${it.message}")
            }.getOrNull()
        }
        return listOf(main) + companions
    }

    /**
     * 연결 설정에서 지정한 jar. ';'로 여러 개를 적을 수 있다.
     * Oracle이면 같은 폴더의 orai18n*.jar도 함께 넣는다 (한글 문자셋 DB).
     */
    internal fun userJars(type: String, driverPath: String): List<Path> {
        val jars = driverPath.split(';').map { it.trim() }.filter { it.isNotEmpty() }.map { Paths.get(it).toAbsolutePath().normalize() }
        if (jars.isEmpty()) throw IOException("No JDBC driver file is set.")
        for (jar in jars) if (!Files.isRegularFile(jar)) throw IOException("JDBC driver file not found: $jar")
        val companions = if (type == "oracle") {
            jars.mapNotNull { it.parent }.distinct().flatMap { dir ->
                runCatching {
                    Files.list(dir).use { files ->
                        files.filter { file ->
                            val name = file.fileName.toString().lowercase()
                            name.startsWith("orai18n") && name.endsWith(".jar") && Files.isRegularFile(file)
                        }.toList()
                    }
                }.getOrDefault(emptyList())
            }
        } else emptyList()
        return (jars + companions.map { it.toAbsolutePath().normalize() }).distinct()
    }

    /** Maven Central에서 드라이버 jar를 내려받는다. 이미 있으면 그 파일을 쓴다. */
    fun download(artifact: Artifact): Path {
        synchronized(downloadLock) { return downloadLocked(artifact) }
    }

    private fun downloadLocked(artifact: Artifact): Path {
        val target = driverDir.resolve(artifact.fileName)
        if (Files.isRegularFile(target)) return target

        Files.createDirectories(driverDir)
        val url = "$repositoryUrl/${artifact.path}"
        Log.info("Downloading JDBC driver $url")
        val bytes = Http.get(url)
        val expected = String(Http.get("$url.sha1"), Charsets.US_ASCII).trim().take(40).lowercase()
        val actual = sha1(bytes)
        if (expected != actual) throw IOException("Checksum mismatch for ${artifact.fileName} (expected $expected, got $actual)")

        val temp = Files.createTempFile(driverDir, artifact.name, ".part")
        Files.write(temp, bytes)
        Files.move(temp, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        Log.info("Saved ${artifact.fileName} (${bytes.size / 1024} KB)")
        return target
    }

    private fun loadFromClasspath(className: String): Driver? = try {
        instantiate(DriverRegistry::class.java.classLoader, className)
    } catch (e: ClassNotFoundException) {
        null
    }

    private fun loaderFor(jars: List<Path>): ClassLoader =
        loaders.computeIfAbsent(jars.map { it.toAbsolutePath().normalize() }) { paths ->
            URLClassLoader(paths.map { it.toUri().toURL() }.toTypedArray(), DriverRegistry::class.java.classLoader)
        }

    private fun instantiate(loader: ClassLoader, className: String): Driver =
        Class.forName(className, true, loader).getDeclaredConstructor().newInstance() as Driver

    private fun sha1(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
}
