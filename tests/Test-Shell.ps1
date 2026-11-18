param(
    [Parameter(Mandatory)]
    [string]$Archive,
    [string]$Zig = 'zig'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
$temporary = Join-Path (Resolve-Path $artifacts).Path ('.shell-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    Expand-Archive -LiteralPath $Archive -DestinationPath $temporary
    $exe = Join-Path $temporary 'ShellIntegration.exe'
    $arguments = @('c++', '-target', 'x86_64-windows-gnu', '-std=c++20', '-O2', '-municode',
        '-DUNICODE', '-D_UNICODE', (Join-Path $PSScriptRoot 'ShellIntegration.cpp'),
        '-lole32', '-loleaut32', '-lshell32', '-lshlwapi', '-luuid', '-ladvapi32', '-luser32', '-o', $exe)
    & $Zig @arguments
    if ($LASTEXITCODE) { throw 'Shell integration test build failed.' }
    $launch = [Diagnostics.ProcessStartInfo]::new($exe)
    $launch.UseShellExecute = $false
    $launch.CreateNoWindow = $true
    $launch.RedirectStandardOutput = $true
    $launch.RedirectStandardError = $true
    $launch.ArgumentList.Add((Join-Path $temporary 'WindowCloud.Explorer.dll'))
    $launch.ArgumentList.Add($temporary)
    $process = [Diagnostics.Process]::Start($launch)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    try {
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw 'Shell integration test timed out.'
        }
        if ($process.ExitCode) { throw 'Shell integration test failed.' }
    }
    finally {
        Write-Output $stdout.GetAwaiter().GetResult()
        Write-Output $stderr.GetAwaiter().GetResult()
        $process.Dispose()
    }
}
finally {
    $directory = Get-Item -LiteralPath $temporary
    if ($directory.FullName -eq $temporary -and $directory.Parent.FullName -eq (Resolve-Path $artifacts).Path -and
        -not ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force
    }
}
