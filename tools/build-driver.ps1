param([string]$WatcomRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$savedEnvironment=@{WATCOM=$env:WATCOM; INCLUDE=$env:INCLUDE; PATH=$env:PATH}
Push-Location $root
try {
    if(!$WatcomRoot){$WatcomRoot=Join-Path $root 'tools/ow'}
    $env:WATCOM=(Resolve-Path $WatcomRoot).Path
    $env:INCLUDE="$env:WATCOM/h/win;$env:WATCOM/h"
    $env:PATH="$env:WATCOM/binnt64;$env:WATCOM/binnt;$env:PATH"
    New-Item -ItemType Directory -Force build/driver16 | Out-Null
    & "$env:WATCOM/binnt64/wcc.exe" -q -bt=windows -ml -zc -zu -s -os '-fo=build/driver16/vsc55drv.obj' src/driver16/vsc55drv.c
    if($LASTEXITCODE) { throw 'Win16 driver compilation failed' }
    $link=@"
system windows dll
option quiet
option map=build/driver16/vsc55drv.map
name build/driver16/VSC55.DRV
file build/driver16/vsc55drv.obj
file '$env:WATCOM/lib286/win/libentry.obj'
libpath '$env:WATCOM/lib286/win'
libpath '$env:WATCOM/lib286'
library windows,mmsystem,clibl
option heapsize=1024
option description 'VSC-55 MIDI output'
export WEP.1 resident
export DRIVERPROC.2=DriverProc resident
export MODMESSAGE.3 resident
export ___EXPORTEDSTUB.4=__exportedstub resident
export MIDMESSAGE.5 resident
"@
    $link | Set-Content -Encoding ASCII build/driver16/driver.lnk
    & "$env:WATCOM/binnt64/wlink.exe" '@build/driver16/driver.lnk'
    if($LASTEXITCODE) { throw 'Win16 driver link failed' }
} finally {
 $env:WATCOM=$savedEnvironment.WATCOM;$env:INCLUDE=$savedEnvironment.INCLUDE;$env:PATH=$savedEnvironment.PATH
 Pop-Location
}
