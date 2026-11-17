package dbaro.engine.driver

import dbaro.engine.Log
import java.io.IOException
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.security.KeyStore
import java.time.Duration
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory

/** 드라이버 다운로드용 HTTP. Windows에서는 OS 인증서 저장소를 써서 회사 HTTPS 검사 프록시 환경에서도 동작한다. */
object Http {
    private val client: HttpClient by lazy {
        val builder = HttpClient.newBuilder()
            .followRedirects(HttpClient.Redirect.NORMAL)
            .connectTimeout(Duration.ofSeconds(20))
        windowsSslContext()?.let { builder.sslContext(it) }
        builder.build()
    }

    fun get(url: String): ByteArray {
        val request = HttpRequest.newBuilder(URI.create(url)).timeout(Duration.ofMinutes(3)).GET().build()
        val response = client.send(request, HttpResponse.BodyHandlers.ofByteArray())
        if (response.statusCode() != 200) throw IOException("HTTP ${response.statusCode()} for $url")
        return response.body()
    }

    private fun windowsSslContext(): SSLContext? {
        if (!System.getProperty("os.name").startsWith("Windows")) return null
        return try {
            val store = KeyStore.getInstance("Windows-ROOT")
            store.load(null, null)
            val trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
            trust.init(store)
            SSLContext.getInstance("TLS").apply { init(null, trust.trustManagers, null) }
        } catch (e: Exception) {
            Log.info("Windows certificate store is not available, using the JDK default: ${e.message}")
            null
        }
    }
}
