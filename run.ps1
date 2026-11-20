$ErrorActionPreference = 'Stop'
$application = Join-Path $PSScriptRoot 'build/PingLedger.exe'
if (-not (Test-Path -LiteralPath $application)) { & (Join-Path $PSScriptRoot 'build.ps1') }
& $application
