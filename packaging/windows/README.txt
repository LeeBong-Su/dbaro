DBaro (디비바로) - 크로스플랫폼 DB 접속 툴

실행
  dbaro.exe 를 실행한다. Java나 Qt를 따로 설치할 필요가 없다 (runtime\ 폴더에 Java 런타임이 들어 있다).
  MSI로 설치했다면 바탕 화면이나 시작 메뉴의 DBaro 바로 가기로 실행한다.
  서명되지 않은 배포본은 Windows 11 스마트 앱 제어가 켜져 있으면 실행이 막힌다
  (Windows 보안 > 앱 및 브라우저 컨트롤 > 스마트 앱 제어 설정).

명령줄
  dbaro-cli.exe 는 DBaro에 저장한 연결로 SQL을 실행한다 (dbaro-cli --help).
    dbaro-cli -l                              저장된 연결 목록
    dbaro-cli 연결이름 "select * from emp"     실행하고 끝
    dbaro-cli 연결이름                         대화형 (; 로 끝, \q 로 나가기)
    @파일명                                    스크립트 폴더의 SQL 파일 실행 (\scripts 로 폴더 정하기)

Claude Code에서 쓰기 (API 키 없이 자기 Claude 로그인으로)
  DBaro의 Database > Use with Claude Code... 에서 등록 명령을 복사하거나 바로 등록한다.
    claude mcp add --transport stdio --scope user dbaro -- "<DBaro 폴더>\dbaro-cli.exe" mcp
  Claude가 저장된 연결의 구조를 읽고 읽기 전용 조회·실행 계획을 보고, 데이터를 바꾸는 SQL은
  실행하지 않고 DBaro의 새 쿼리 탭에 넣는다. 비밀번호를 저장한 연결만 쓸 수 있고,
  운영 연결은 조회하지 않는다 (--allow-production 으로 연다).
  Database > Open Claude Code (Ctrl+Alt+C) 는 지금 연결로 Claude Code를 새 터미널 창에서 연다.

처음 연결할 때
  Oracle, MariaDB, MySQL, PostgreSQL JDBC 드라이버는 처음 연결할 때 Maven Central에서 내려받아
  %APPDATA%\DBaro\DBaro\drivers\ 에 저장한다. 다음부터는 저장한 파일을 쓴다.
  Oracle은 orai18n도 함께 받는다. KO16MSWIN949, KO16KSC5601(EUC-KR) 같은 문자셋의 DB에 붙을 때 필요하다.
  Tibero는 Tibero 클라이언트의 client\lib\jar\tibero7-jdbc.jar 를 연결 설정의 Advanced 탭에서 지정해야 한다.

인터넷이 막힌 환경 (폐쇄망)
  아래 jar를 %APPDATA%\DBaro\DBaro\drivers\ 에 파일 이름 그대로 복사해 두면 내려받지 않고 바로 쓴다.
  연결 설정은 바꾸지 않아도 되고, dbaro-cli도 같은 폴더를 쓴다.
  인터넷이 되는 PC에서 DBaro로 한 번 연결해 봤다면 그 PC의 같은 폴더에 이미 있다.

    Oracle       ojdbc11-23.4.0.24.05.jar
                 orai18n-23.4.0.24.05.jar   (한글 KO16MSWIN949·KO16KSC5601 등 문자셋 DB. 없으면 이런 DB에 접속하지 못한다)
    PostgreSQL   postgresql-42.7.4.jar
    MySQL        mysql-connector-j-8.4.0.jar
    MariaDB      mariadb-java-client-3.4.1.jar

  받는 곳 (Maven Central)
    https://repo1.maven.org/maven2/com/oracle/database/jdbc/ojdbc11/23.4.0.24.05/ojdbc11-23.4.0.24.05.jar
    https://repo1.maven.org/maven2/com/oracle/database/nls/orai18n/23.4.0.24.05/orai18n-23.4.0.24.05.jar
    https://repo1.maven.org/maven2/org/postgresql/postgresql/42.7.4/postgresql-42.7.4.jar
    https://repo1.maven.org/maven2/com/mysql/mysql-connector-j/8.4.0/mysql-connector-j-8.4.0.jar
    https://repo1.maven.org/maven2/org/mariadb/jdbc/mariadb-java-client/3.4.1/mariadb-java-client-3.4.1.jar

  파일 이름이 한 글자라도 다르면 찾지 못하고 내려받으려다 실패한다.
  다른 버전을 쓰려면 연결 설정의 Advanced 탭에서 jar 파일을 직접 지정한다. 여러 jar는 ; 로 잇는다.
  Oracle은 지정한 jar와 같은 폴더의 orai18n*.jar 도 함께 불러온다.
  Tibero jar는 Tibero 서버나 클라이언트의 client\lib\jar\ 에서 복사해 와서 같은 방법으로 지정한다.

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
