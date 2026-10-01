$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$url='https://github.com/redpanda-cpp/mingw-lite/releases/download/16.2.0%2Bemutls-r1/mingw32_686-msvcrt_win98-16.2.0%2Bemutls-r1.7z'
$expected='d51c5103f9cb4f92cfaae6fb0045512683097307f31dbd759457784d9761f4e3'
$archive=Join-Path $root 'tools/downloads/mingw-lite-win98.7z'
$destination=Join-Path $root 'tools/mingw-lite'
New-Item -ItemType Directory -Force (Split-Path $archive),$destination | Out-Null
if(!(Test-Path $archive)) {
    & curl.exe --fail --location --silent --show-error $url -o $archive
    if($LASTEXITCODE) { throw 'Toolchain download failed' }
}
if((Get-FileHash $archive).Hash.ToLowerInvariant() -ne $expected) { throw 'Toolchain archive hash mismatch' }
if(!(Test-Path (Join-Path $destination 'mingw32_686-msvcrt_win98-16+emutls/bin/g++.exe'))) {
    & tar.exe -xf $archive -C $destination
    if($LASTEXITCODE) { throw 'Toolchain extraction failed' }
}
Write-Output 'Verified MinGW Lite GCC 16.2.0, i686 MSVCRT Win98, emulated TLS.'
