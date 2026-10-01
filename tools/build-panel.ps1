param([switch]$TimingDiagnostics, [string]$CompilerRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$CompilerRoot){$CompilerRoot=Join-Path $root 'tools/mingw-lite/mingw32_686-msvcrt_win98-16+emutls/bin'}
$compiler=$CompilerRoot.Replace('\','/')
$buildDir=if($TimingDiagnostics){'build/panel-timing'}else{'build/panel'}
$timing=if($TimingDiagnostics){'ON'}else{'OFF'}
$profile=if($TimingDiagnostics){'A00-o3lto-timing'}else{'A00-o3lto'}
Push-Location $root
try {
    & cmake -S . -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DVSC55_WIN98=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DVSC55_CPU_PROFILE=base "-DVSC55_BUILD_PROFILE=$profile" "-DVSC55_TIMING_DIAGNOSTICS=$timing" '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG' '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG' "-DCMAKE_C_COMPILER=$compiler/gcc.exe" "-DCMAKE_CXX_COMPILER=$compiler/g++.exe" "-DCMAKE_RC_COMPILER=$compiler/windres.exe"
    if($LASTEXITCODE){throw 'Panel configuration failed'}
    & cmake --build $buildDir --target vsc55 vsccfg midireg -j 4
    if($LASTEXITCODE){throw 'Panel build failed'}
    Copy-Item assets/frontpanel-float/PANEL.BMP "$buildDir/PANEL.BMP" -Force
} finally {Pop-Location}
