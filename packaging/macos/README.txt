DBaro (디비바로) - 크로스플랫폼 DB 접속 툴 (macOS, Apple Silicon)

설치
  DBaro.app 을 Applications 폴더로 끌어다 놓는다.
  Java나 Qt를 따로 설치할 필요가 없다 (앱 안에 Java 런타임이 들어 있다).

처음 열 때 (Apple 서명이 없는 앱)
  "DBaro를 열 수 없습니다" / "확인되지 않은 개발자" 창이 뜨면:
    시스템 설정 > 개인정보 보호 및 보안 > 아래쪽 "DBaro" 옆의 [그래도 열기] 를 누른 뒤 다시 연다.
  또는 터미널에서 한 번 실행한다:
    xattr -dr com.apple.quarantine /Applications/DBaro.app

명령줄
  dbaro-cli 는 앱 안에 들어 있다. 터미널에서 쓰려면 한 번 연결해 둔다:
    sudo ln -sf /Applications/DBaro.app/Contents/MacOS/dbaro-cli /usr/local/bin/dbaro-cli
  dbaro-cli 는 DBaro에 저장한 연결로 SQL을 실행한다 (dbaro-cli --help).
    dbaro-cli -l                              저장된 연결 목록
    dbaro-cli 연결이름 "select * from emp"     실행하고 끝
    dbaro-cli 연결이름                         대화형 (; 로 끝, \q 로 나가기)

Claude Code에서 쓰기 (API 키 없이 자기 Claude 로그인으로)
  DBaro의 Database > Use with Claude Code... 에서 등록 명령을 복사하거나 바로 등록한다.
    claude mcp add --transport stdio --scope user dbaro -- /Applications/DBaro.app/Contents/MacOS/dbaro-cli mcp
  Claude가 저장된 연결의 구조를 읽고 읽기 전용 조회·실행 계획을 보고, 데이터를 바꾸는 SQL은
  실행하지 않고 DBaro의 새 쿼리 탭에 넣는다. 비밀번호를 저장한 연결만 쓸 수 있고,
  운영 연결은 조회하지 않는다 (--allow-production 으로 연다).
  dbaro-cli 가 처음 키체인의 비밀번호를 읽을 때 macOS가 물으면 [항상 허용]을 누른다.
  Database > Open Claude Code (⌘⌥C) 는 지금 연결로 터미널 앱에서 Claude Code를 연다.

처음 연결할 때
  Oracle, MariaDB, MySQL, PostgreSQL JDBC 드라이버는 처음 연결할 때 Maven Central에서 내려받는다.
  인터넷이 막힌 환경이면 연결 설정의 Advanced 탭에서 드라이버 jar 파일을 직접 지정한다.
  Tibero는 Tibero 클라이언트의 client/lib/jar/tibero7-jdbc.jar 를 지정해야 한다.

비밀번호
  로그인 키체인에 "DBaro/connection/..." 이름으로 저장한다.
  저장한 비밀번호를 읽을 때 macOS가 키체인 접근을 물으면 [항상 허용]을 누른다
  (새 버전을 설치하면 다시 물을 수 있다).

사용자 데이터 위치 (앱을 지워도 남는다)
  연결 정보   ~/Library/Application Support/DBaro/DBaro/connections.json
  드라이버    ~/Library/Application Support/DBaro/DBaro/drivers/
  창 배치·테마 ~/Library/Preferences 의 DBaro 설정 파일 (.plist)

제거
  Applications 폴더의 DBaro.app 을 휴지통으로 옮긴다. 사용자 데이터는 남는다.

라이선스
  함께 들어 있는 구성 요소의 라이선스는 DBaro.app/Contents/Resources/THIRD_PARTY_NOTICES.md 에 있다.
  빌드에 쓴 버전은 DBaro.app/Contents/Resources/BUILD-INFO.txt 에 있다.
