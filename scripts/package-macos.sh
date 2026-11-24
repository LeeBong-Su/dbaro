#!/usr/bin/env bash
# DBaro macOS 배포본(DBaro.app + .dmg)을 만든다.
#
#   1. 앱 Release 빌드 (CMake + Apple clang)
#   2. macdeployqt로 Qt 프레임워크·플러그인을 앱 번들에 넣는다 (dbaro-cli 포함)
#   3. 엔진 installDist (Gradle Wrapper) 후 jar를 Contents/Resources/engine/lib 에
#   4. jlink로 Java 런타임을 Contents/Resources/runtime 에 (사용자는 Java를 설치하지 않아도 된다)
#   5. 라이선스 고지, 빌드 정보
#   6. 임시 서명 (ad-hoc. Apple 개발자 서명이 아니어서 처음 열 때 허락이 필요하다)
#   7. 배포본의 Java로 엔진을 띄워 hello 요청에 응답하는지, dbaro-cli가 도는지 확인
#   8. DMG 생성 (DBaro.app, Applications 바로 가기, README.txt)
#
# 결과: dist/DBaro-<버전>-macos-<arm64|x86_64>.dmg
#
# 사용법:
#   scripts/package-macos.sh [--qt-dir DIR] [--jdk-home DIR] [--run-tests]
#     --qt-dir    Qt 폴더 (예: ~/Qt/6.10.3/macos). 비우면 QT_DIR, QT_ROOT_DIR 환경 변수
#     --jdk-home  JDK 21 폴더. 비우면 JAVA_HOME. Gradle 8.14는 JDK 25 이상에서 실행되지 않는다
#     --run-tests 엔진 단위 테스트도 함께 실행한다
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
qt_dir="${QT_DIR:-${QT_ROOT_DIR:-}}"
jdk_home="${JAVA_HOME:-}"
run_tests=0
while [ $# -gt 0 ]; do
    case "$1" in
        --qt-dir) qt_dir="$2"; shift 2 ;;
        --jdk-home) jdk_home="$2"; shift 2 ;;
        --run-tests) run_tests=1; shift ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

step() { printf '\n==> %s\n' "$1"; }
fail() { echo "error: $1" >&2; exit 1; }

# 엔진이 쓰는 모듈(jdeps: java.base, java.net.http, java.sql)에 JDBC 드라이버, TLS, 한국어 문자셋에 필요한 모듈을 더한다.
# Windows와 같고, Windows 인증서 저장소(jdk.crypto.mscapi)만 뺀다 (macOS 키체인 인증서는 java.base의 KeychainStore).
java_modules=(
    java.base java.sql java.net.http
    java.naming java.management java.logging java.xml java.transaction.xa java.desktop
    java.security.jgss java.security.sasl
    jdk.crypto.ec       # TLS의 ECDHE 암호 (DB SSL 연결, 드라이버 다운로드)
    jdk.charsets        # MS949, EUC-KR 등 한국어 문자셋
    jdk.localedata      # 한국어 로케일 (en, ko만 포함)
    jdk.naming.dns jdk.unsupported
)

# ---------------------------------------------------------------------------

step 'Checking tools'
[ -n "$qt_dir" ] && [ -x "$qt_dir/bin/macdeployqt" ] || fail 'Qt was not found. Pass --qt-dir ~/Qt/<version>/macos'
[ -n "$jdk_home" ] && [ -x "$jdk_home/bin/jlink" ] || fail 'A JDK with jlink was not found. Pass --jdk-home or set JAVA_HOME to JDK 21.'
command -v cmake >/dev/null || fail 'CMake was not found (brew install cmake)'

jdk_version="$(sed -n 's/^JAVA_VERSION="\(.*\)"$/\1/p' "$jdk_home/release")"
if [ -n "$jdk_version" ] && [ "${jdk_version%%.*}" -gt 24 ]; then
    fail "JDK $jdk_version is too new for Gradle 8.14. Use JDK 21."
fi
version="$(sed -n 's/^project(dbaro VERSION \([0-9.]*\).*/\1/p' "$repo/app/CMakeLists.txt")"
[ -n "$version" ] || fail 'Could not read the version from app/CMakeLists.txt'
arch="$(uname -m)"
commit="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)"
qt_version="$(basename "$(dirname "$qt_dir")")"

echo "Version : $version ($commit)"
echo "Arch    : $arch"
echo "Qt      : $qt_dir"
echo "JDK     : $jdk_home ($jdk_version)"

name="DBaro-$version-macos-$arch"
out_dir="$repo/dist"
stage="$out_dir/$name"
app="$stage/DBaro.app"
resources="$app/Contents/Resources"
build_dir="$repo/build/package-macos"

step 'Building the app (Release)'
cmake -S "$repo/app" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_dir" \
    -DCMAKE_OSX_ARCHITECTURES="$arch"
cmake --build "$build_dir" --target dbaro dbaro-cli --parallel

step 'Building the engine'
export JAVA_HOME="$jdk_home"
gradle_tasks=(installDist)
if [ "$run_tests" = 1 ]; then gradle_tasks+=(test); fi
"$repo/engine/gradlew" -p "$repo/engine" --console=plain "${gradle_tasks[@]}"

step "Staging $app"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$build_dir/dbaro.app" "$app"
cp "$build_dir/dbaro-cli" "$app/Contents/MacOS/dbaro-cli"
"$qt_dir/bin/macdeployqt" "$app" -executable="$app/Contents/MacOS/dbaro-cli" -always-overwrite

mkdir -p "$resources/engine/lib"
cp "$repo/engine/build/install/dbaro-engine/lib/"*.jar "$resources/engine/lib/"

step 'Creating the Java runtime (jlink)'
modules="$(IFS=,; echo "${java_modules[*]}")"
"$jdk_home/bin/jlink" --add-modules "$modules" --include-locales=en,ko \
    --strip-debug --no-header-files --no-man-pages --compress=zip-6 --output "$resources/runtime"

step 'Adding notices and build info'
cp "$repo/packaging/THIRD_PARTY_NOTICES.md" "$resources/"
{
    echo "DBaro $version"
    echo "Commit    : $commit"
    echo "Built     : $(date '+%Y-%m-%d %H:%M')"
    echo "Qt        : $qt_version (macos)"
    echo "Java      : $jdk_version (jlink runtime)"
    echo "Arch      : $arch"
    echo "macOS SDK : $(xcrun --show-sdk-version 2>/dev/null || echo unknown)"
    echo "Engine jars:"
    ls "$resources/engine/lib" | sed 's/^/  - /'
} > "$resources/BUILD-INFO.txt"

step 'Signing (ad-hoc)'
# Apple Silicon은 서명이 없는 코드를 실행하지 않는다. macdeployqt가 라이브러리 경로를 고치면서 서명이 깨지므로
# Java 런타임의 실행 파일·라이브러리, dbaro-cli, 앱 번들 순서로 다시 서명한다.
find "$resources/runtime" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 | xargs -0 codesign --force --sign -
codesign --force --sign - "$app/Contents/MacOS/dbaro-cli"
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

step 'Checking the packaged engine and CLI'
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# { "id": 1, "method": "hello", "params": {} } (CBOR 26바이트) 앞에 4바이트 길이를 붙인 메시지.
printf '\000\000\000\032\243\142id\001\146method\145hello\146params\240' > "$work/request.bin"
# 입력이 끝나면 엔진은 받은 요청에 답한 뒤 스스로 끝난다.
"$resources/runtime/bin/java" -cp "$resources/engine/lib/*" dbaro.engine.MainKt --driver-dir "$work/drivers" \
    < "$work/request.bin" > "$work/response.bin" 2> "$work/engine.log" &
engine_pid=$!
( sleep 30; kill "$engine_pid" 2>/dev/null ) &
watchdog_pid=$!
wait "$engine_pid" || true
kill "$watchdog_pid" 2>/dev/null || true
if ! grep -a -q "$version" "$work/response.bin"; then
    cat "$work/engine.log" >&2
    fail 'The packaged engine did not answer the hello request.'
fi
echo 'The engine started with the bundled Java runtime and answered.'
"$app/Contents/MacOS/dbaro-cli" --version

step 'Creating the DMG'
dmg="$out_dir/$name.dmg"
dmg_root="$work/dmg"
mkdir -p "$dmg_root"
ditto "$app" "$dmg_root/DBaro.app"
ln -s /Applications "$dmg_root/Applications"
cp "$repo/packaging/macos/README.txt" "$dmg_root/README.txt"
rm -f "$dmg"
# hdiutil은 가끔 "Resource busy"로 실패해서 몇 번 다시 해 본다.
for attempt in 1 2 3; do
    if hdiutil create -volname "DBaro $version" -srcfolder "$dmg_root" -ov -format UDZO "$dmg"; then
        break
    fi
    [ "$attempt" = 3 ] && fail 'hdiutil could not create the DMG.'
    sleep 5
done

step 'Done'
echo "App : $app ($(du -sh "$app" | cut -f1))"
echo "DMG : $dmg ($(du -sh "$dmg" | cut -f1))"
