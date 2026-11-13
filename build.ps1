param(
    [string]$Dotnet = 'dotnet',
    [string]$Zig = 'zig',
    [switch]$SelfContained
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
$name = 'WindowCloud-win-x64'
if ($SelfContained) { $name += '-self-contained' }
$output = Join-Path $artifacts $name
New-Item -ItemType Directory -Path $output -Force | Out-Null

$dotnetCommand = (Get-Command $Dotnet).Source
$zigCommand = (Get-Command $Zig).Source
$mode = $SelfContained.IsPresent.ToString().ToLowerInvariant()
& $dotnetCommand publish (Join-Path $root 'src/WindowCloud') -c Release -r win-x64 --self-contained $mode -o $output -p:DebugType=None
if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)." }

$native = Join-Path $root 'src/Explorer'
$sources = @('Folder.cpp', 'Menu.cpp', 'Pipe.cpp', 'Transfer.cpp', 'Explorer.def') |
    ForEach-Object { Join-Path $native $_ }
& $zigCommand c++ -target x86_64-windows-gnu -std=c++20 -O2 -shared -s -DUNICODE -D_UNICODE @sources `
    -lole32 -loleaut32 -lshell32 -lshlwapi -luuid -ladvapi32 -luser32 `
    "-Wl,--out-implib,$(Join-Path $artifacts 'WindowCloud.Explorer.lib')" -o (Join-Path $output 'WindowCloud.Explorer.dll')
if ($LASTEXITCODE) { throw "Explorer DLL build failed ($LASTEXITCODE)." }

$archive = Join-Path $artifacts "$name.zip"
$files = Get-ChildItem -LiteralPath $output -File |
    Where-Object { $_.Extension -notin '.lib', '.pdb' -and $_.Name -ne 'oauth-clients.json' } |
    Select-Object -ExpandProperty FullName
Compress-Archive -Path $files -DestinationPath $archive -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $archive
