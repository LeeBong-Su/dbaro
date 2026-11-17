# DBaro (디비바로)

DB를 바로 다루는 크로스플랫폼 DB 접속 툴.
Oracle, MariaDB, MySQL, PostgreSQL, Tibero를 지원한다.

## 폴더 구조

```
dbaro/
├─ app/        Qt 6 (C++) 데스크톱 앱: 에디터, 결과 그리드, 객체 탐색기, 파서·자동완성
├─ engine/     Kotlin/JVM 드라이버 호스트: JDBC 연결, 실행·취소, 메타데이터 조회
├─ protocol/   앱과 엔진 사이 통신 규약
├─ dev/        개발·테스트용 DB (Docker Compose)
├─ bench/      성능 기준과 측정
└─ docs/       설계 문서
```

## 개발 환경

| 도구 | 버전 | 용도 |
|---|---|---|
| Qt | 6.8 이상 (개발 PC는 6.10.3) | 앱 |
| CMake | 3.21 이상 | 앱 빌드 |
| C++ 컴파일러 | C++20 지원 (MSVC 2022, Clang, GCC) | 앱 빌드 |
| JDK | 21 | 엔진 (Gradle은 Wrapper가 받아 쓴다) |
| Docker 호환 도구 | Compose v2 | 테스트용 DB |

Windows 설치 방법은 [docs/dev-setup-windows.md](docs/dev-setup-windows.md)에 있다.
빌드 방법은 `app/README.md`, `engine/README.md`에 있다.

## 문서

- [아키텍처](docs/architecture.md)
- [통신 규약](protocol/README.md)
- [성능 기준](bench/README.md)
