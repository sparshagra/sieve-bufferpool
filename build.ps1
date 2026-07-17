# Builds both binaries with -O2 and runs the tests. Requires g++ on PATH
# (MSYS2 UCRT64: C:\msys64\ucrt64\bin). No CMake needed.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force -Path build, results | Out-Null

$flags = @("-O2", "-std=c++17", "-Wall", "-Wextra", "-Iinclude")

Write-Host "building runner..." -ForegroundColor Cyan
g++ @flags (Get-Item src\*.cpp, src\policies\*.cpp -ErrorAction SilentlyContinue).FullName bench\runner.cpp -o build\runner.exe
if ($LASTEXITCODE -ne 0) { throw "runner build failed" }

Write-Host "building tests..." -ForegroundColor Cyan
g++ @flags (Get-Item src\*.cpp, src\policies\*.cpp -ErrorAction SilentlyContinue).FullName tests\run_tests.cpp -o build\run_tests.exe
if ($LASTEXITCODE -ne 0) { throw "tests build failed" }

Write-Host "running tests..." -ForegroundColor Cyan
.\build\run_tests.exe
if ($LASTEXITCODE -ne 0) { throw "TESTS FAILED" }
Write-Host "OK" -ForegroundColor Green
