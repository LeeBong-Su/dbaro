plugins {
    kotlin("jvm") version "2.1.0"
    application
}

group = "dbaro"
version = "0.1.0"

repositories {
    mavenCentral()
}

dependencies {
    // JDBC 드라이버는 여기에 넣지 않는다.
    // 실행 중에 jar를 내려받거나 사용자가 지정한 jar를 불러온다 (driver 패키지).
    testImplementation(kotlin("test"))
}

kotlin {
    jvmToolchain(21)
}

application {
    mainClass.set("dbaro.engine.MainKt")
}

tasks.test {
    useJUnitPlatform()
}
