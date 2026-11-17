package dbaro.engine

/**
 * DBaro 엔진(드라이버 호스트) 진입점.
 *
 * Qt 앱이 이 프로세스를 띄우고 표준 입출력으로 통신한다.
 * 표준 출력은 프로토콜 전용이므로 로그는 반드시 표준 오류로 보낸다.
 */
fun main() {
    System.err.println("DBaro engine 0.1.0")
    // TODO: 표준 입력에서 요청을 읽고 처리한다 (protocol/README.md 참고).
}
