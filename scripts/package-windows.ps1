<#
.SYNOPSIS
    DBaro Windows 배포본(폴더 + ZIP)을 만든다.

.DESCRIPTION
    1. 앱 Release 빌드 (CMake + MSVC)
    2. windeployqt로 Qt DLL·플러그인 수집, MSVC 런타임 DLL을 앱 옆에 복사
    3. 엔진 installDist (Gradle Wrapper) 후 jar 복사
    4. jlink로 Java 런타임 생성 (사용자는 Java를 설치하지 않아도 된다)
    5. 라이선스 고지, 안내문, 빌드 정보 복사
    6. 배포본의 Java로 엔진을 띄워 hello 요청에 응답하는지 확인
    7. ZIP 생성

    결과: dist\DBaro-<버전>-windows-x64\ 폴더와 같은 이름의 .zip

.PARAMETER QtDir
    Qt 설치 폴더 (예: C:\Qt\6.10.3\msvc2022_64). 비우면 C:\Qt 아래 최신 msvc2022_64를 찾는다.

.PARAMETER JdkHome
    JDK 21 폴더. 비우면 JAVA_HOME. Gradle 8.14는 JDK 25 이상에서 실행되지 않는다.

.PARAMETER RunTests
    엔진 단위 테스트도 함께 실행한다.

.PARAMETER NoZip
    ZIP을 만들지 않고 폴더만 만든다.

.EXAMPLE
    .\scripts\package-windows.ps1
    .\scripts\package-windows.ps1 -QtDir C:\Qt\6.10.3\msvc2022_64 -RunTests
#>
[CmdletBinding()]
param(
    [string]$QtDir = $env:QTDIR,
    [string]$JdkHome = $env:JAVA_HOME,
    [string]$OutDir,
    [switch]$RunTests,
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }

# 엔진이 쓰는 모듈(jdeps: java.base, java.net.http, java.sql)에 JDBC 드라이버, TLS, 한국어 문자셋에 필요한 모듈을 더한다.
$javaModules = @(
    'java.base', 'java.sql', 'java.net.http',
    'java.naming', 'java.management', 'java.logging', 'java.xml', 'java.transaction.xa', 'java.desktop',
    'java.security.jgss', 'java.security.sasl',
    'jdk.crypto.ec',      # TLS의 ECDHE 암호 (DB SSL 연결, 드라이버 다운로드)
    'jdk.crypto.mscapi',  # Windows 인증서 저장소 (회사 HTTPS 검사 프록시 환경)
    'jdk.charsets',       # MS949, EUC-KR 등 한국어 문자셋
    'jdk.localedata',     # 한국어 로케일 (en, ko만 포함)
    'jdk.naming.dns', 'jdk.unsupported'
)

function Write-Step([string]$message) {
    Write-Host ""
    Write-Host "==> $message" -ForegroundColor Cyan
}

# 외부 프로그램 실행. 표준 오류 출력 때문에 PowerShell이 멈추지 않게 하고, 종료 코드로 성공을 판단한다.
function Invoke-Native([string]$File, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $File @Arguments 2>&1 | ForEach-Object { "$_" }
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($LASTEXITCODE -ne 0) { throw "$File failed (exit code $LASTEXITCODE)" }
}

function Find-Qt {
    if ($QtDir) { return $QtDir }
    if (-not (Test-Path 'C:\Qt')) { return $null }
    $versions = Get-ChildItem 'C:\Qt' -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName 'msvc2022_64'
        if (Test-Path (Join-Path $candidate 'bin\windeployqt.exe')) { return $candidate }
    }
    return $null
}

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $default = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $default) { return $default }
    throw 'CMake was not found. Install it with: winget install -e --id Kitware.CMake'
}

function Get-JdkVersion([string]$jdk) {
    $release = Join-Path $jdk 'release'
    if (-not (Test-Path $release)) { return $null }
    $line = Select-String -Path $release -Pattern '^JAVA_VERSION="([^"]+)"' | Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[1].Value }
    return $null
}

function Get-FolderSizeMB([string]$path) {
    $bytes = (Get-ChildItem $path -Recurse -File | Measure-Object -Property Length -Sum).Sum
    return [math]::Round($bytes / 1MB, 1)
}

# 배포본의 Java로 엔진을 띄워 hello 요청(CBOR)을 보내고 응답에 버전이 들어 있는지 확인한다.
function Test-Engine([string]$stage, [string]$expectedVersion) {
    $java = Join-Path $stage 'runtime\bin\java.exe'
    $classPath = Join-Path $stage 'engine\lib\*'
    $work = Join-Path $env:TEMP "dbaro-package-check-$PID"
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $request = Join-Path $work 'request.bin'
    $response = Join-Path $work 'response.bin'
    $log = Join-Path $work 'engine.log'

    # { "id": 1, "method": "hello", "params": {} } 앞에 4바이트 길이를 붙인 메시지.
    # .NET Framework의 표준 입력 스트림은 UTF-8 BOM을 먼저 쓰므로, 파일을 그대로 표준 입력으로 넘긴다.
    [byte[]]$payload = [byte[]](0xA3, 0x62) + [Text.Encoding]::ASCII.GetBytes('id') + [byte[]](0x01, 0x66) +
        [Text.Encoding]::ASCII.GetBytes('method') + [byte[]](0x65) + [Text.Encoding]::ASCII.GetBytes('hello') +
        [byte[]](0x66) + [Text.Encoding]::ASCII.GetBytes('params') + [byte[]](0xA0)
    [byte[]]$frame = [byte[]](0, 0, 0, $payload.Length) + $payload
    [IO.File]::WriteAllBytes($request, $frame)

    try {
        # 입력 파일이 끝나면 엔진은 받은 요청에 답한 뒤 스스로 끝난다.
        $arguments = @('-cp', "`"$classPath`"", 'dbaro.engine.MainKt', '--driver-dir', "`"$work\drivers`"")
        $process = Start-Process -FilePath $java -ArgumentList $arguments -NoNewWindow -PassThru `
            -RedirectStandardInput $request -RedirectStandardOutput $response -RedirectStandardError $log
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw 'The engine did not finish within 30 seconds.'
        }
        $bytes = [IO.File]::ReadAllBytes($response)
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        if ($bytes.Length -lt 5 -or $text -notlike "*$expectedVersion*") {
            throw "Unexpected engine answer ($($bytes.Length) bytes). Engine log: $(Get-Content $log -Raw)"
        }
    } finally {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------

Write-Step 'Checking tools'
$QtDir = Find-Qt
if (-not $QtDir -or -not (Test-Path (Join-Path $QtDir 'bin\windeployqt.exe'))) {
    throw 'Qt was not found. Pass -QtDir C:\Qt\<version>\msvc2022_64'
}
if (-not $JdkHome -or -not (Test-Path (Join-Path $JdkHome 'bin\jlink.exe'))) {
    throw 'A JDK with jlink was not found. Pass -JdkHome or set JAVA_HOME to JDK 21.'
}
$jdkVersion = Get-JdkVersion $JdkHome
if ($jdkVersion -and [int]($jdkVersion.Split('.')[0]) -gt 24) {
    throw "JDK $jdkVersion is too new for Gradle 8.14. Use JDK 21."
}
$cmake = Find-CMake
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -products * -latest -property installationPath
$crtDir = Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crtDir) { throw 'The MSVC runtime (VC\Redist) was not found in Visual Studio.' }

$cmakeLists = Get-Content (Join-Path $repo 'app\CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(dbaro VERSION ([0-9.]+)') { throw 'Could not read the version from app\CMakeLists.txt' }
$version = $Matches[1]
$qtVersion = Split-Path -Leaf (Split-Path -Parent $QtDir)
$commit = (& git -C $repo rev-parse --short HEAD 2>$null)

Write-Host "Version : $version ($commit)"
Write-Host "Qt      : $QtDir"
Write-Host "JDK     : $JdkHome ($jdkVersion)"
Write-Host "CRT     : $($crtDir.FullName)"

$name = "DBaro-$version-windows-x64"
$stage = Join-Path $OutDir $name

Write-Step 'Building the app (Release)'
$buildDir = Join-Path $repo 'build\package'
Invoke-Native $cmake @('-S', (Join-Path $repo 'app'), '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$QtDir")
Invoke-Native $cmake @('--build', $buildDir, '--config', 'Release', '--target', 'dbaro')

Write-Step 'Building the engine'
$env:JAVA_HOME = $JdkHome
$gradleArgs = @('-p', (Join-Path $repo 'engine'), '--console=plain', 'installDist')
if ($RunTests) { $gradleArgs += 'test' }
Invoke-Native (Join-Path $repo 'engine\gradlew.bat') $gradleArgs

Write-Step "Staging $stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item (Join-Path $buildDir 'Release\dbaro.exe') $stage

Invoke-Native (Join-Path $QtDir 'bin\windeployqt.exe') @('--release', '--no-translations', '--no-system-d3d-compiler',
    '--no-opengl-sw', '--no-compiler-runtime', (Join-Path $stage 'dbaro.exe'))

# MSVC 런타임은 설치 프로그램 없이 앱 옆에 둔다 (앱 로컬 배포).
Copy-Item (Join-Path $crtDir.FullName '*.dll') $stage

$engineLib = Join-Path $stage 'engine\lib'
New-Item -ItemType Directory -Path $engineLib -Force | Out-Null
Copy-Item (Join-Path $repo 'engine\build\install\dbaro-engine\lib\*.jar') $engineLib

Write-Step 'Creating the Java runtime (jlink)'
Invoke-Native (Join-Path $JdkHome 'bin\jlink.exe') @('--add-modules', ($javaModules -join ','), '--include-locales=en,ko',
    '--strip-debug', '--no-header-files', '--no-man-pages', '--compress=zip-6', '--output', (Join-Path $stage 'runtime'))

Write-Step 'Adding notices and build info'
Copy-Item (Join-Path $repo 'packaging\THIRD_PARTY_NOTICES.md') $stage
Copy-Item (Join-Path $repo 'packaging\windows\README.txt') $stage
$jars = (Get-ChildItem $engineLib -Filter *.jar | ForEach-Object { "  - $($_.Name)" }) -join "`r`n"
$buildInfo = @"
DBaro $version
Commit    : $commit
Built     : $(Get-Date -Format 'yyyy-MM-dd HH:mm')
Qt        : $qtVersion (msvc2022_64)
Java      : $jdkVersion (jlink runtime)
MSVC CRT  : $(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $crtDir.FullName)))
Engine jars:
$jars
"@
Set-Content -Path (Join-Path $stage 'BUILD-INFO.txt') -Value $buildInfo -Encoding UTF8

Write-Step 'Checking the packaged engine'
Test-Engine $stage $version
Write-Host 'The engine started with the bundled Java runtime and answered.'

if (-not $NoZip) {
    Write-Step 'Creating the ZIP'
    $zip = "$stage.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path $tar) {
        Invoke-Native $tar @('-a', '-c', '-f', $zip, '-C', $OutDir, $name)
    } else {
        Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
    }
}

Write-Step 'Done'
Write-Host ("Folder : {0} ({1} MB)" -f $stage, (Get-FolderSizeMB $stage))
if (-not $NoZip) { Write-Host ("ZIP    : {0} ({1} MB)" -f "$stage.zip", [math]::Round((Get-Item "$stage.zip").Length / 1MB, 1)) }
