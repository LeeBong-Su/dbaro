package dbaro.engine

import dbaro.engine.driver.DriverRegistry
import dbaro.engine.ipc.Server
import java.io.FileDescriptor
import java.io.FileOutputStream
import java.nio.file.Path
import java.nio.file.Paths

/**
 * DBaro 엔진(드라이버 호스트) 진입점.
 *
 * Qt 앱이 이 프로세스를 띄우고 표준 입출력으로 통신한다 (protocol/README.md).
 * 표준 출력은 프로토콜 전용이다. 드라이버 등이 System.out에 쓰더라도 프로토콜이 깨지지 않게
 * 실제 표준 출력은 따로 잡아 두고 System.out은 표준 오류로 돌린다.
 */
fun main(args: Array<String>) {
    val protocolOut = FileOutputStream(FileDescriptor.out)
    System.setOut(System.err)

    val driverDir = argument(args, "--driver-dir")?.let { Paths.get(it) } ?: defaultDriverDir()
    Log.info("DBaro engine $ENGINE_VERSION (Java ${System.getProperty("java.version")}), drivers: $driverDir")

    Server(System.`in`, protocolOut, DriverRegistry(driverDir)).run()
}

private fun argument(args: Array<String>, name: String): String? {
    val index = args.indexOf(name)
    return if (index >= 0 && index + 1 < args.size) args[index + 1] else null
}

private fun defaultDriverDir(): Path = Paths.get(System.getProperty("user.home"), ".dbaro", "drivers")
