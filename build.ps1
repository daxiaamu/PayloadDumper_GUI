param(
    [ValidateSet('x64','x86')][string[]]$Architectures = @('x64','x86'),
    [string]$Compiler64 = 'D:\mingw64\bin\gcc.exe',
    [string]$Compiler32 = 'D:\msys64\mingw32\bin\gcc.exe',
    [string]$PayloadProject = (Join-Path $PSScriptRoot 'third_party\payload_dumper_C')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    # Some MinGW assemblers cannot create temporary objects under a Chinese
    # Windows profile path. Scope temporary files to the project during builds.
    $oldTmp = $env:TMP
    $oldTemp = $env:TEMP
    $scratch = Join-Path $PSScriptRoot '.build\tmp'
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    try {
        $env:TMP = $scratch
        $env:TEMP = $scratch
        & $Exe @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE): $Exe" }
    } finally {
        $env:TMP = $oldTmp
        $env:TEMP = $oldTemp
    }
}
$sources = @('main.c','payload.c','ziputil.c','decompress.c','dumper.c','threadpool.c','http_reader.c','sha256.c')
foreach ($arch in $Architectures) {
    $compiler = if ($arch -eq 'x64') { $Compiler64 } else { $Compiler32 }
    if (-not (Test-Path -LiteralPath $compiler)) { throw "Compiler not found: $compiler" }
    $windres = Join-Path (Split-Path $compiler) 'windres.exe'
    $build = Join-Path $PSScriptRoot ".build\$arch"
    $package = Join-Path $PSScriptRoot "dist\win-$arch"
    $bin = Join-Path $package 'bin'
    New-Item -ItemType Directory -Force -Path $build,$bin | Out-Null
    Push-Location (Join-Path $PSScriptRoot 'src')
    try { Invoke-Checked $windres @('-I.','-i','app.rc','-o',(Join-Path $build 'app.o')) }
    finally { Pop-Location }
    Invoke-Checked $compiler @('-std=c11','-O2','-Wall','-Wextra','-c',
        (Join-Path $PSScriptRoot 'third_party\miniz\miniz_tinfl.c'),'-o',(Join-Path $build 'tinfl.o'))
    Invoke-Checked $compiler @('-std=c11','-O2','-Wall','-Wextra','-Werror','-static','-s','-municode','-mwindows',
        '-finput-charset=UTF-8','-fexec-charset=UTF-8','-D__USE_MINGW_ANSI_STDIO=1',
        (Join-Path $PSScriptRoot 'src\main.c'),(Join-Path $PSScriptRoot 'src\update.c'),
        (Join-Path $PSScriptRoot 'src\update_core.c'),(Join-Path $build 'tinfl.o'),(Join-Path $build 'app.o'),
        '-o',(Join-Path $package 'PayloadDumperGUI.exe'),
        '-lcomctl32','-lcomdlg32','-lshell32','-lshlwapi','-lole32','-lgdi32','-luuid','-lwinhttp','-lbcrypt','-lversion','-ladvapi32','-luxtheme')
    Invoke-Checked $compiler @('-std=c11','-O2','-Wall','-Wextra','-Werror','-static','-s','-municode','-mwindows',
        '-D__USE_MINGW_ANSI_STDIO=1',(Join-Path $PSScriptRoot 'src\updater.c'),
        (Join-Path $PSScriptRoot 'src\update_core.c'),(Join-Path $build 'tinfl.o'),(Join-Path $build 'app.o'),
        '-o',(Join-Path $bin 'updater.exe'),'-lshell32','-lbcrypt','-lversion')
    Invoke-Checked $compiler @('-std=c11','-O2','-Wall','-Wextra','-static','-s',
        (Join-Path $PSScriptRoot 'third_party\arbextract\windows_utf8.c'),
        '-o',(Join-Path $bin 'arbextract.exe'),'-lshell32')
    $libs = if ($arch -eq 'x64') { 'lib' } else { 'lib32' }
    Push-Location $PayloadProject
    try {
        $arguments = @('-std=c11','-O2','-Wall','-Wextra','-static','-s','-mconsole','-D__USE_MINGW_ANSI_STDIO=1',
            "-I$libs/include","-L$libs/lib",'-o',(Join-Path $bin 'payload_dumper.exe')) + $sources +
            @('-lzstd','-lbrotlidec','-lbrotlicommon','-lbz2','-llzma','-lbcrypt','-lwinhttp','-lshlwapi','-lshell32')
        Invoke-Checked $compiler $arguments
    } finally { Pop-Location }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $package
    $licenses = Join-Path $package 'licenses'
    New-Item -ItemType Directory -Force -Path $licenses | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\arbextract\LICENSE') -Destination (Join-Path $licenses 'arbextract-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $PayloadProject 'LICENSE') -Destination (Join-Path $licenses 'payload_dumper-GPL-3.0.txt')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\miniz\LICENSE') -Destination (Join-Path $licenses 'miniz-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\jsmn\LICENSE') -Destination (Join-Path $licenses 'jsmn-MIT.txt')
    Write-Host "Built $package"
}
# Default launch location: x64 when available; use x86 for a 32-bit-only build.
$defaultArch = if ($Architectures -contains 'x64') { 'x64' } else { 'x86' }
$default = Join-Path $PSScriptRoot "dist\win-$defaultArch"
Copy-Item -LiteralPath (Join-Path $default 'PayloadDumperGUI.exe') -Destination $PSScriptRoot
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'bin') | Out-Null
Copy-Item -Path (Join-Path $default 'bin\*.exe') -Destination (Join-Path $PSScriptRoot 'bin')
Write-Host "Default $defaultArch application copied to project root."
