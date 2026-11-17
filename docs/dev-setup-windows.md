# Windows 개발 환경 설치

관리자 PowerShell에서 진행한다.

## 1. winget으로 설치

```powershell
winget install -e --id EclipseAdoptium.Temurin.21.JDK
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

- VS Build Tools는 C++ 워크로드(MSVC, Windows SDK)만 설치한다. 끝나면 PC를 다시 시작한다.
- Temurin JDK 설치 프로그램이 시스템 `JAVA_HOME`을 JDK 21로 설정한다. Gradle 8.14는 JDK 25 이상에서 실행되지 않으므로 이 설정을 유지한다.
- Gradle은 설치하지 않는다. `engine/gradlew`가 받아 쓴다.

## 2. Qt (aqtinstall)

Qt 계정 없이 Qt 공식 서버에서 받는다.

```powershell
python -m venv aqt-venv
aqt-venv\Scripts\python -m pip install aqtinstall truststore
aqt-venv\Scripts\aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 -O C:\Qt
```

- aqtinstall 3.3.0은 Qt 6.11부터 바뀐 저장소 구조를 읽지 못한다. 6.10.x까지 설치할 수 있다.
- Python 3.15에는 aqtinstall 의존 패키지의 미리 빌드된 wheel이 없어서 MSVC로 직접 빌드된다. VS Build Tools를 먼저 설치한다.

### 회사 네트워크에서 SSL 오류가 날 때

회사 프록시가 HTTPS를 검사하면서 인증서를 회사 인증서로 다시 서명하는 경우가 있다.
Windows는 이 인증서를 신뢰하지만 Python은 자체 인증서 목록을 써서 `CERTIFICATE_VERIFY_FAILED`가 난다.
검증을 끄지 말고, `truststore`로 Python이 Windows 인증서 저장소를 쓰게 한다.
aqt는 여러 작업 프로세스로 파일을 받으므로, 실행 스크립트가 아니라 가상환경의 `sitecustomize.py`에 넣어야 모든 프로세스에 적용된다.

```python
# aqt-venv\Lib\site-packages\sitecustomize.py
import truststore

truststore.inject_into_ssl()
```

그다음 위의 `aqt install-qt` 명령을 그대로 실행한다.

## 3. 테스트용 DB

Docker 호환 도구는 아직 설치하지 않았다. Docker Desktop은 직원 250명 이상 또는 매출 1천만 달러 이상인 회사에서 업무용으로 쓰면 유료 구독이 필요하다. 무료 대안은 Rancher Desktop이다.
