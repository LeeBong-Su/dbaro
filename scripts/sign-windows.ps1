<#
.SYNOPSIS
    DBaro Windows 실행 파일과 MSI에 Authenticode 서명을 한다 (signtool).

.DESCRIPTION
    package-windows.ps1 과 build-msi-windows.ps1 이 부른다. 서명 설정은 저장소에 두지 않고 환경 변수로 받는다.

      DBARO_SIGN_ARGS       signtool에 넘길 인증서 선택 인자. 비어 있으면 서명하지 않고 경고만 한다.
                              인증서 저장소(하드웨어 토큰 포함) : /sha1 <인증서 지문>
                              Azure Artifact Signing           : /dlib <경로>\Azure.CodeSigning.Dlib.dll /dmdf <경로>\metadata.json
      DBARO_SIGN_TIMESTAMP  RFC 3161 타임스탬프 서버. 기본은 http://timestamp.digicert.com
                            (Artifact Signing은 http://timestamp.acs.microsoft.com)

    스마트 앱 제어가 켜진 Windows는 Microsoft 신뢰 루트 프로그램에 속한 인증 기관의 인증서로 서명하지 않은
    exe·dll을 실행하지 못하게 막는다. 자체 서명 인증서는 통하지 않는다.
    배포본에서 서명이 없는 파일은 dbaro.exe, dbaro-cli.exe 뿐이다 (Qt, Java 런타임, MSVC 런타임은 이미 서명돼 있다).

.PARAMETER Files
    서명할 파일. 서명한 뒤 서명이 유효한지(신뢰할 수 있는 인증서인지) 확인한다.

.PARAMETER AuditFolder
    폴더 안의 exe·dll 중 유효한 서명이 없는 파일을 찾는다. 서명을 설정했는데 남아 있으면 실패하고,
    설정하지 않았으면 경고만 한다.

.EXAMPLE
    $env:DBARO_SIGN_ARGS = '/sha1 0123456789ABCDEF0123456789ABCDEF01234567'
    .\scripts\build-msi-windows.ps1
#>
[CmdletBinding()]
param(
    [Parameter(ParameterSetName = 'Sign', Mandatory)] [string[]]$Files,
    [Parameter(ParameterSetName = 'Audit', Mandatory)] [string]$AuditFolder
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$signArgs = $env:DBARO_SIGN_ARGS
$configured = -not [string]::IsNullOrWhiteSpace($signArgs)
$timestamp = if ($env:DBARO_SIGN_TIMESTAMP) { $env:DBARO_SIGN_TIMESTAMP } else { 'http://timestamp.digicert.com' }

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    # Windows SDK(Visual Studio C++ 워크로드에 들어 있다)의 가장 새 x64 signtool
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $tool = Get-ChildItem $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' -and $_.Directory.Parent.Name -match '^\d+(\.\d+)+$' } |
        Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
        Select-Object -First 1
    if ($tool) { return $tool.FullName }
    throw 'signtool.exe was not found. Install the Windows SDK (Visual Studio "Desktop development with C++").'
}

if ($PSCmdlet.ParameterSetName -eq 'Audit') {
    $root = (Resolve-Path $AuditFolder).Path.TrimEnd('\')
    $unsigned = @(Get-ChildItem $root -Recurse -Include *.exe, *.dll |
        Where-Object { (Get-AuthenticodeSignature $_.FullName).Status -ne 'Valid' })
    if ($unsigned.Count -eq 0) {
        Write-Host 'Every exe/dll in the package has a valid signature.'
        return
    }
    $list = ($unsigned | ForEach-Object { '  ' + $_.FullName.Substring($root.Length + 1) }) -join [Environment]::NewLine
    if ($configured) { throw "These files have no valid signature:$([Environment]::NewLine)$list" }
    Write-Warning ("These files are not signed, so Windows Smart App Control will block them " +
        "(set DBARO_SIGN_ARGS to sign; see scripts\sign-windows.ps1):$([Environment]::NewLine)$list")
    return
}

$names = ($Files | ForEach-Object { Split-Path -Leaf $_ }) -join ', '
if (-not $configured) {
    Write-Warning "Not signing $names (DBARO_SIGN_ARGS is not set)."
    return
}

$signTool = Find-SignTool
# 따옴표로 묶은 경로(공백 포함)를 한 인자로 본다.
$selection = @([regex]::Matches($signArgs, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
$arguments = @('sign') + $selection + @(
    '/fd', 'SHA256', '/tr', $timestamp, '/td', 'SHA256',
    '/d', 'DBaro', '/du', 'https://github.com/LeeBong-Su/dbaro') + $Files
& $signTool @arguments
if ($LASTEXITCODE -ne 0) { throw "signtool failed (exit code $LASTEXITCODE)" }

foreach ($file in $Files) {
    $signature = Get-AuthenticodeSignature $file
    if ($signature.Status -ne 'Valid') {
        throw "$(Split-Path -Leaf $file): the signature is $($signature.Status). $($signature.StatusMessage)"
    }
    if (-not $signature.TimeStamperCertificate) { throw "$(Split-Path -Leaf $file): the signature has no timestamp." }
    Write-Host "Signed $(Split-Path -Leaf $file)"
}
