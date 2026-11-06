$ErrorActionPreference = 'Stop'
$release = Join-Path $PSScriptRoot 'release'
New-Item -ItemType Directory -Force -Path $release | Out-Null
foreach ($arch in @('x64','x86')) {
    $package = Join-Path $PSScriptRoot "dist\win-$arch"
    $items = @('PayloadDumperGUI.exe','bin','licenses','README.md') | ForEach-Object { Join-Path $package $_ }
    foreach ($item in $items) {
        if (-not (Test-Path -LiteralPath $item)) { throw "Missing build output: $item" }
    }
    Compress-Archive -LiteralPath $items -DestinationPath (Join-Path $release "PayloadDumperGUI-win-$arch.zip") -Force
}
$sourceItems = @('src','assets','tools','tests','third_party','updates','.github','.gitignore','.gitattributes','README.md','VALIDATION.md','build.ps1','package.ps1') |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
# Include dotfiles/workflows explicitly; Compress-Archive skips hidden items.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sourcePath = Join-Path $release 'PayloadDumperGUI-source.zip'
$stream = [IO.File]::Open($sourcePath,[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite)
$archive = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($item in $sourceItems) {
        $files = if (Test-Path -LiteralPath $item -PathType Container) { Get-ChildItem -LiteralPath $item -Recurse -File -Force } else { Get-Item -LiteralPath $item -Force }
        foreach ($file in $files) {
            if ($file.FullName -match '[\\/]__pycache__[\\/]') { continue }
            $relative = $file.FullName.Substring($PSScriptRoot.Length + 1).Replace('\','/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
} finally {
    $archive.Dispose()
    $stream.Dispose()
}
$hashes = Get-ChildItem -LiteralPath $release -Filter '*.zip' | Sort-Object Name | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    "{0}  {1}" -f $hash.Hash.ToLowerInvariant(), $_.Name
}
[IO.File]::WriteAllLines((Join-Path $release 'SHA256SUMS.txt'), [string[]]$hashes, [Text.UTF8Encoding]::new($false))
Get-ChildItem -LiteralPath $release | Select-Object Name,Length
