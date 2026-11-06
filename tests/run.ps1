param(
    [string]$Compiler64 = 'D:\mingw64\bin\gcc.exe',
    [string]$Compiler32 = 'D:\msys64\mingw32\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
& python (Join-Path $PSScriptRoot 'smoke.py')
if ($LASTEXITCODE) { throw 'CLI smoke checks failed' }
$oldTmp = $env:TMP
$oldTemp = $env:TEMP
try {
    $scratch = Join-Path $root '.build\tmp'
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    $env:TMP = $scratch
    $env:TEMP = $scratch
    foreach ($arch in @('x64','x86')) {
        $gcc = if ($arch -eq 'x64') { $Compiler64 } else { $Compiler32 }
        $testDir = Join-Path $root ".build\$arch"
        $probe = Join-Path $testDir 'probe.exe'
        $test = Join-Path $testDir 'gui_integration.exe'
        & $gcc -std=c11 -O2 -Wall -Wextra -Werror -static -s (Join-Path $PSScriptRoot 'probe.c') -o $probe -lshell32
        if ($LASTEXITCODE) { throw 'Probe compile failed' }
        & $gcc -std=c11 -O2 -Wall -Wextra -Werror -static -s -municode -mconsole -D__USE_MINGW_ANSI_STDIO=1 (Join-Path $PSScriptRoot 'gui_integration.c') (Join-Path $root 'src\update.c') (Join-Path $root 'src\update_core.c') (Join-Path $testDir 'tinfl.o') (Join-Path $testDir 'app.o') -o $test -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lole32 -lgdi32 -luuid -lwinhttp -lbcrypt -lversion -ladvapi32 -luxtheme
        if ($LASTEXITCODE) { throw 'GUI integration test compile failed' }
        & $test (Join-Path $root "dist\win-$arch") (Join-Path $root '.build\smoke\中文 路径 & fixture') $probe
        if ($LASTEXITCODE) { throw "GUI integration failed: $arch" }
        $ota = Join-Path $testDir 'ota_test.exe'
        & $gcc -std=c11 -O2 -Wall -Wextra -Werror -static -s -municode -mconsole -D__USE_MINGW_ANSI_STDIO=1 (Join-Path $PSScriptRoot 'ota.c') (Join-Path $root 'src\update_core.c') (Join-Path $testDir 'tinfl.o') -o $ota -lshell32 -lbcrypt -lversion
        if ($LASTEXITCODE) { throw 'OTA integration test compile failed' }
        $fixture = (& python (Join-Path $PSScriptRoot 'ota_fixtures.py') $arch) | ConvertFrom-Json
        if ($LASTEXITCODE) { throw 'OTA fixture generation failed' }
        & $ota $fixture
        if ($LASTEXITCODE) { throw "OTA integration failed: $arch" }
        $sessions = Join-Path $testDir 'update_sessions.exe'
        & $gcc -std=c11 -O2 -Wall -Wextra -Werror -static -s -municode -mconsole -D__USE_MINGW_ANSI_STDIO=1 (Join-Path $PSScriptRoot 'update_sessions.c') (Join-Path $root 'src\update_core.c') (Join-Path $testDir 'tinfl.o') -o $sessions -lcomctl32 -lwinhttp -lshell32 -lbcrypt -lversion -ladvapi32 -luxtheme -lgdi32
        if ($LASTEXITCODE) { throw 'Update session test compile failed' }
        & $sessions $fixture
        if ($LASTEXITCODE) { throw "Update session tests failed: $arch" }
    }
} finally {
    $env:TMP = $oldTmp
    $env:TEMP = $oldTemp
}
