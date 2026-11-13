param(
    [string]$Dotnet = 'dotnet',
    [string]$Zig = 'zig',
    [switch]$SelfContained,
    [string]$OAuthClients
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
$name = 'WindowCloud-win-x64'
if ($SelfContained) { $name += '-self-contained' }
$dotnetCommand = (Get-Command $Dotnet).Source
$zigCommand = (Get-Command $Zig).Source
$mode = $SelfContained.IsPresent.ToString().ToLowerInvariant()

if ($OAuthClients) {
    $oauthFile = Get-Item -LiteralPath $OAuthClients
    $config = Get-Content -LiteralPath $oauthFile.FullName -Raw | ConvertFrom-Json
    $providerNames = @('GoogleDrive', 'OneDrive', 'Dropbox')
    $clients = [ordered]@{}
    foreach ($provider in $providerNames) {
        $registration = $config.$provider
        if ($null -eq $registration) { continue }
        if ($registration.clientId -isnot [string] -or [string]::IsNullOrWhiteSpace($registration.clientId)) {
            throw "Missing OAuth clientId for $provider."
        }
        $client = [ordered]@{ clientId = $registration.clientId.Trim() }
        if ($provider -eq 'GoogleDrive' -and $registration.desktopSecret) {
            if ($registration.desktopSecret -isnot [string]) { throw 'Invalid Google desktopSecret.' }
            $client.desktopSecret = $registration.desktopSecret.Trim()
        }
        $clients[$provider] = $client
    }
    if ($clients.Count -eq 0) { throw 'The OAuth file does not contain a supported provider.' }
}

$stagingName = '.publish-' + [Guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$artifacts = (Resolve-Path -LiteralPath $artifacts).Path
$output = Join-Path $artifacts $stagingName
New-Item -ItemType Directory -Path $output | Out-Null
$archive = Join-Path $artifacts "$name.zip"
try {
    & $dotnetCommand publish (Join-Path $root 'src/WindowCloud') -c Release -r win-x64 --self-contained $mode -o $output -p:DebugType=None
    if ($LASTEXITCODE) { throw "dotnet publish failed ($LASTEXITCODE)." }

    $native = Join-Path $root 'src/Explorer'
    $sources = @('Folder.cpp', 'Menu.cpp', 'Pipe.cpp', 'Transfer.cpp', 'Explorer.def') |
        ForEach-Object { Join-Path $native $_ }
    & $zigCommand c++ -target x86_64-windows-gnu -std=c++20 -O2 -shared -s -DUNICODE -D_UNICODE @sources `
        -lole32 -loleaut32 -lshell32 -lshlwapi -luuid -ladvapi32 -luser32 `
        "-Wl,--out-implib,$(Join-Path $output 'WindowCloud.Explorer.lib')" -o (Join-Path $output 'WindowCloud.Explorer.dll')
    if ($LASTEXITCODE) { throw "Explorer DLL build failed ($LASTEXITCODE)." }
    Remove-Item -LiteralPath (Join-Path $output 'WindowCloud.Explorer.lib') -ErrorAction SilentlyContinue

    if ($OAuthClients) {
        $clients | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'oauth-clients.json') -Encoding utf8
    }
    if ($SelfContained) {
        foreach ($notice in @('LICENSE.txt', 'ThirdPartyNotices.txt')) {
            Copy-Item -LiteralPath (Join-Path (Split-Path $dotnetCommand) $notice) -Destination $output
        }
    }
    Copy-Item -LiteralPath (Join-Path (Split-Path $zigCommand) 'LICENSE') -Destination (Join-Path $output 'Zig-LICENSE.txt')

    Compress-Archive -Path (Join-Path $output '*') -DestinationPath $archive -Force
    Get-FileHash -Algorithm SHA256 -LiteralPath $archive
}
finally {
    $staging = Get-Item -LiteralPath $output
    if ($staging.FullName -eq $output -and $staging.Parent.FullName -eq $artifacts -and
        -not ($staging.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $staging.FullName -Recurse -Force
    }
}
