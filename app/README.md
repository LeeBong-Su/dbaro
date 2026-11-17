# app — Qt 6 데스크톱 앱 (C++)

사용자가 보는 화면 전체와, 키 입력마다 반응해야 하는 기능을 맡는다.
DB 연결은 직접 하지 않고 엔진 프로세스(`../engine`)에 맡긴다.

| 폴더 | 역할 |
|---|---|
| `src/ui/` | SQL 에디터, 결과 그리드, 객체 탐색기, 연결 관리 화면 |
| `src/core/` | SQL 문장 분리·파서, 자동완성, 메타데이터 캐시. 키 입력마다 엔진을 오가지 않도록 앱 안에서 처리한다 |
| `src/ipc/` | 엔진 프로세스 실행과 통신 |
| `resources/` | 아이콘, 번역(한국어·영어), 문법 정의 |
| `tests/` | 단위 테스트 |

## 빌드

Qt 6.8 이상과 CMake 3.21 이상이 필요하다. 저장소 루트에서:

```bash
cmake -S app -B build/app -DCMAKE_PREFIX_PATH=<Qt 설치 경로>
cmake --build build/app
```
