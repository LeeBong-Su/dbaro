<#
.SYNOPSIS
    DBaro Windows 설치 프로그램(MSI)을 만든다.

.DESCRIPTION
    1. package-windows.ps1로 배포 폴더를 만든다 (-SkipPackage면 이미 있는 폴더를 쓴다)
    2. WiX v5로 배포 폴더를 통째로 담은 MSI를 만든다 (packaging\windows\DBaro.wxs)

    결과: dist\DBaro-<버전>-windows-x64.msi

    WiX v5가 필요하다 (v6 이상은 유지보수 비용·EULA 조건이 있어 쓰지 않는다):
        winget install -e --id Microsoft.DotNet.SDK.8
        dotnet tool install --global wix --version 5.0.2

.PARAMETER SkipPackage
    배포 폴더를 다시 만들지 않고 dist 안의 기존 폴더로 MSI만 만든다.

.EXAMPLE
    .\scripts\build-msi-windows.ps1
    .\scripts\build-msi-windows.ps1 -SkipPackage
#>
[CmdletBinding()]
param(
    [switch]$SkipPackage,
    [string]$QtDir = $env:QTDIR,
    [string]$JdkHome = $env:JAVA_HOME,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo 'dist'

function Write-Step([string]$message) {
    Write-Host ""
    Write-Host "==> $message" -ForegroundColor Cyan
}

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

function Find-Wix {
    $command = Get-Command wix -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $tool = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'
    if (Test-Path $tool) { return $tool }
    throw 'WiX was not found. Install it with: dotnet tool install --global wix --version 5.0.2'
}

$cmakeLists = Get-Content (Join-Path $repo 'app\CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(dbaro VERSION ([0-9.]+)') { throw 'Could not read the version from app\CMakeLists.txt' }
$version = $Matches[1]
$name = "DBaro-$version-windows-x64"
$stage = Join-Path $outDir $name
$msi = Join-Path $outDir "$name.msi"

$wix = Find-Wix
$wixVersion = (& $wix --version) -replace '\+.*$', ''
if (-not $wixVersion.StartsWith('5.')) {
    throw "WiX $wixVersion was found, but this script uses WiX v5 (v6+ has maintenance-fee/EULA terms)."
}

if (-not $SkipPackage) {
    Write-Step 'Creating the package folder'
    $packageArgs = @{}
    if ($QtDir) { $packageArgs.QtDir = $QtDir }
    if ($JdkHome) { $packageArgs.JdkHome = $JdkHome }
    if ($RunTests) { $packageArgs.RunTests = $true }
    & (Join-Path $PSScriptRoot 'package-windows.ps1') @packageArgs
}
if (-not (Test-Path (Join-Path $stage 'dbaro.exe'))) {
    throw "The package folder was not found: $stage (run without -SkipPackage)"
}

Write-Step "Building $msi (WiX $wixVersion)"
if (Test-Path $msi) { Remove-Item $msi -Force }
Invoke-Native $wix @('build', (Join-Path $repo 'packaging\windows\DBaro.wxs'),
    '-arch', 'x64',
    '-d', "Version=$version",
    '-d', "IconFile=$(Join-Path $repo 'app\resources\dbaro.ico')",
    '-bindpath', "Stage=$stage",
    '-o', $msi)

# 빌드 부산물(.wixpdb)은 배포에 쓰지 않는다.
Remove-Item (Join-Path $outDir "$name.wixpdb") -Force -ErrorAction SilentlyContinue

# Windows Installer 규칙 검사(ICE). 설계상 예상되는 두 항목만 제외한다.
#  - ICE57: 사용자별/전체 사용자 겸용 패키지에서 바로 가기 키 경로를 HKMU로 둘 때 나오는 경고
#  - ICE61: 같은 버전 재설치를 허용(AllowSameVersionUpgrades)해서 나오는 경고
Write-Step 'Validating the MSI (ICE)'
Invoke-Native $wix @('msi', 'validate', '-sice', 'ICE57', '-sice', 'ICE61', $msi)

Write-Step 'Done'
Write-Host ("MSI : {0} ({1} MB)" -f $msi, [math]::Round((Get-Item $msi).Length / 1MB, 1))
Write-Host 'Install for the current user : double-click, or  msiexec /i <msi>'
Write-Host 'Install for all users (admin): msiexec /i <msi> ALLUSERS=1'
