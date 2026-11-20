param([switch]$Test)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$compiler = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    $candidate = 'C:\msys64\ucrt64\bin\g++.exe'
    if (-not (Test-Path -LiteralPath $candidate)) { throw 'Install MSYS2 UCRT64 GCC and add its bin folder to PATH.' }
    $compiler = Get-Item -LiteralPath $candidate
}
$compilerPath = if ($compiler.Source) { $compiler.Source } else { $compiler.FullName }
$env:PATH = (Split-Path $compilerPath) + ';' + $env:PATH
New-Item -ItemType Directory -Path build -Force | Out-Null
$common = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0601', '-Iinclude', '-static', '-static-libgcc', '-static-libstdc++')
& $compilerPath @common src/main.cpp -o build/PingLedger.exe -municode -mwindows -lcomctl32 -liphlpapi -lws2_32 -lshell32 -lole32 -luuid -lgdi32
if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }
& $compilerPath @common tests/core_tests.cpp -o build/core_tests.exe -liphlpapi -lws2_32
if ($LASTEXITCODE -ne 0) { throw 'Tests build failed.' }
if ($Test) {
    & ./build/core_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    $smoke = Start-Process -FilePath "$PSScriptRoot/build/PingLedger.exe" -ArgumentList '--smoke' -WindowStyle Hidden -Wait -PassThru
    if ($smoke.ExitCode -ne 0) { throw "GUI smoke test failed: $($smoke.ExitCode)" }
    Write-Host 'PASS: native GUI smoke test, background measurements, CSV persistence, offscreen rendering.'
}
Write-Host 'Built build/PingLedger.exe'
