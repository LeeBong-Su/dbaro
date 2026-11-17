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
    // 테스트용 내장 DB. 실제 DB 서버 없이 JDBC 흐름(실행, 페이지, 취소, 트랜잭션)을 확인한다.
    testImplementation("com.h2database:h2:2.3.232")
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
