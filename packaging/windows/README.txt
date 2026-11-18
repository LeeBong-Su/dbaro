DBaro (디비바로) - 크로스플랫폼 DB 접속 툴

실행
  dbaro.exe 를 실행한다. Java나 Qt를 따로 설치할 필요가 없다 (runtime\ 폴더에 Java 런타임이 들어 있다).

처음 연결할 때
  Oracle, MariaDB, MySQL, PostgreSQL JDBC 드라이버는 처음 연결할 때 Maven Central에서 내려받는다.
  인터넷이 막힌 환경이면 연결 설정의 Advanced 탭에서 드라이버 jar 파일을 직접 지정한다.
  Tibero는 Tibero 클라이언트의 client\lib\jar\tibero7-jdbc.jar 를 지정해야 한다.

사용자 데이터 위치 (프로그램 폴더를 지워도 남는다)
  연결 정보   %APPDATA%\DBaro\DBaro\connections.json
  드라이버    %APPDATA%\DBaro\DBaro\drivers\
  비밀번호    Windows 자격 증명 관리자 (DBaro/connection/...)
  창 배치·테마 레지스트리 HKCU\Software\DBaro\DBaro

제거
  MSI로 설치했다면 Windows 설정 > 앱 > 설치된 앱에서 DBaro를 제거한다.
  ZIP으로 받았다면 이 폴더를 지운다.
  어느 경우든 사용자 데이터는 남는다. 모두 지우려면 위 위치도 지운다.

라이선스
  함께 들어 있는 구성 요소의 라이선스는 THIRD_PARTY_NOTICES.md 에 있다.
  빌드에 쓴 버전은 BUILD-INFO.txt 에 있다.
