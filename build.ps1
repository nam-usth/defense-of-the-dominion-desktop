# Builds build\dotd.exe with MinGW-w64 GCC + Ninja.
# Uses gcc / g++ / ninja from PATH, else the toolchain bundled with Strawberry Perl.
#   .\build.ps1          incremental build
#   .\build.ps1 -Clean   rebuild everything
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
  $strawberry = 'C:\Strawberry\c\bin'
  if (Test-Path "$strawberry\g++.exe") { $env:PATH = "$strawberry;$env:PATH" }
  else { Write-Error 'g++ not found: install MinGW-w64 (e.g. MSYS2 or Strawberry Perl) or put it on PATH.' }
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { Write-Error 'ninja not found on PATH.' }

if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build }
ninja
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built $PSScriptRoot\build\dotd.exe"
