# app — Qt 6 데스크톱 앱 (C++)

사용자가 보는 화면 전체와, 키 입력마다 반응해야 하는 기능을 맡는다.
DB 연결은 직접 하지 않고 엔진 프로세스(`../engine`)에 맡긴다.

| 폴더 | 역할 |
|---|---|
| `src/ui/` | 메인 창, SQL 에디터, 결과 그리드, 객체 탐색기, 연결 관리 화면, 테마(`Theme`), 아이콘(`Icons`) |
| `src/core/` | 연결 정보 저장(`ConnectionStore`), 비밀번호 보안 저장(`SecretStore`), 방언별 문장 분리(`SqlSplitter`). 이후 자동완성, 메타데이터 캐시 |
| `src/ipc/` | 엔진 프로세스 실행과 통신 |
| `resources/icons/` | 자체 아이콘 (SVG, `#ICON` 색은 테마에 맞춰 바뀜). 이후 번역(한국어·영어) |
| `tests/` | 단위 테스트 |

## SQL 에디터 단축키

| 키 | 동작 |
|---|---|
| Ctrl+Enter | 커서가 있는 문장 실행 (선택 영역이 있으면 선택 영역 안의 문장들) |
| F5 | 스크립트 전체 실행 |
| Alt+드래그, Alt+Shift+방향키 | 세로 커서·블록 선택 |
| Ctrl+Alt+위/아래 | 위/아래 줄에 커서 추가 |
| Esc | 세로 커서 끝내기 |
| Tab / Shift+Tab | 들여쓰기 / 내어쓰기 (여러 줄 선택 시 줄 단위) |
| Ctrl+/ | 줄 주석 켜기·끄기 |

## 빌드

Qt 6.8 이상과 CMake 3.21 이상이 필요하다. 저장소 루트에서:

```bash
cmake -S app -B build/app -DCMAKE_PREFIX_PATH=<Qt 설치 경로>
cmake --build build/app --config Release
```

Windows 개발 PC 예시 (Qt 6.10.3, MSVC 2022):

```powershell
cmake -S app -B build/app -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build/app --config Release
```

실행하려면 Qt DLL이 필요하다. `C:\Qt\6.10.3\msvc2022_64\bin`을 PATH에 넣거나, 배포용으로는 `windeployqt`로 DLL을 exe 옆에 복사한다.
