# 제3자 구성 요소 고지 (Third-Party Notices)

DBaro 배포본에는 아래 구성 요소가 함께 들어 있다. 실제로 쓴 버전은 `BUILD-INFO.txt`에 있다.

| 구성 요소 | 라이선스 | 위치 | 출처 |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, Network, Svg와 플러그인) | LGPL-3.0-only | `Qt6*.dll`, 플러그인 폴더 | https://www.qt.io/download-open-source , 소스: https://download.qt.io/official_releases/qt/ |
| Java 런타임 (Eclipse Temurin OpenJDK, jlink로 축소) | GPL-2.0-only WITH Classpath-exception-2.0 | `runtime\` (고지 원문: `runtime\legal\`) | https://adoptium.net/ |
| Kotlin 표준 라이브러리 | Apache-2.0 | `engine\lib\kotlin-stdlib-*.jar` | https://kotlinlang.org/ |
| JetBrains Java Annotations | Apache-2.0 | `engine\lib\annotations-*.jar` | https://github.com/JetBrains/java-annotations |
| Microsoft Visual C++ 런타임 | Microsoft Software License Terms (재배포 가능 파일) | `vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll` | https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files |

## Qt (LGPL-3.0)

DBaro는 Qt를 동적 링크(DLL)로 사용한다. 사용자는 같은 버전 계열의 Qt DLL로 바꿔 실행할 수 있다.
Qt 소스 코드는 위 출처에서 받을 수 있다. DBaro는 Qt 소스를 수정하지 않았다.

> 정식 배포 전 할 일: LGPL-3.0과 GPL-3.0 전문을 `packaging\licenses\`에 넣고 배포본에 함께 복사한다.

## 함께 배포하지 않는 구성 요소

JDBC 드라이버는 배포본에 들어 있지 않다. 처음 연결할 때 Maven Central에서 내려받거나 사용자가 직접 지정하며, 각 드라이버의 라이선스를 따른다.

| 드라이버 | 라이선스 |
|---|---|
| Oracle JDBC (ojdbc11), Oracle NLS (orai18n) | Oracle Free Use Terms and Conditions |
| MariaDB Connector/J | LGPL-2.1 |
| MySQL Connector/J | GPL-2.0 with Universal FOSS Exception |
| PostgreSQL JDBC (pgJDBC) | BSD-2-Clause |
| Tibero JDBC | TmaxTibero 라이선스 (사용자가 직접 지정) |
