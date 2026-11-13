param(
    [Parameter(Mandatory)]
    [string]$Archive
)

$ErrorActionPreference = 'Stop'
$tempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path
$testRoot = Join-Path $tempRoot ('WindowCloud smoke test ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$process = $null
try {
    Expand-Archive -LiteralPath $Archive -DestinationPath $testRoot
    $required = @('WindowCloud.exe', 'WindowCloud.Explorer.dll', 'hostfxr.dll', 'hostpolicy.dll',
        'coreclr.dll', 'System.Windows.Forms.dll', 'LICENSE.txt', 'ThirdPartyNotices.txt', 'Zig-LICENSE.txt')
    foreach ($file in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $testRoot $file) -PathType Leaf)) { throw "Missing $file." }
    }
    $runtime = (Get-Content -LiteralPath (Join-Path $testRoot 'WindowCloud.runtimeconfig.json') -Raw | ConvertFrom-Json).runtimeOptions
    if ($runtime.framework -or $runtime.frameworks -or 'Microsoft.WindowsDesktop.App' -notin $runtime.includedFrameworks.name) {
        throw 'Expected a self-contained Windows Desktop runtime.'
    }
    $assemblyBytes = [IO.File]::ReadAllBytes((Join-Path $testRoot 'WindowCloud.dll'))
    $assembly = [Reflection.Assembly]::Load($assemblyBytes)
    $icon = $assembly.GetManifestResourceStream('WindowCloud.TrayIcon')
    if (!$icon) { throw 'The WindowCloud tray icon was not embedded.' }
    try {
        $reader = [IO.BinaryReader]::new($icon)
        if ($reader.ReadUInt16() -ne 0 -or $reader.ReadUInt16() -ne 1 -or $reader.ReadUInt16() -lt 2) {
            throw 'Expected a Windows icon with multiple resolutions.'
        }
    }
    finally { $icon.Dispose() }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PortableNativeSmokeTest
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
    $dll = Join-Path $testRoot 'WindowCloud.Explorer.dll'
    $module = [PortableNativeSmokeTest]::LoadLibraryExW($dll, [IntPtr]::Zero, 0x900)
    if ($module -eq [IntPtr]::Zero) { throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
    try {
        foreach ($export in @('DllGetClassObject', 'DllCanUnloadNow')) {
            if ([PortableNativeSmokeTest]::GetProcAddress($module, $export) -eq [IntPtr]::Zero) {
                throw "Explorer extension has no $export export."
            }
        }
    }
    finally { [void][PortableNativeSmokeTest]::FreeLibrary($module) }

    $launch = [Diagnostics.ProcessStartInfo]::new((Join-Path $testRoot 'WindowCloud.exe'))
    $launch.WorkingDirectory = $testRoot
    $launch.UseShellExecute = $false
    $launch.CreateNoWindow = $true
    $launch.Environment['DOTNET_ROOT'] = Join-Path $testRoot 'missing-dotnet'
    $launch.Environment['DOTNET_ROOT_X64'] = $launch.Environment['DOTNET_ROOT']
    $launch.Environment['PATH'] = [Environment]::SystemDirectory
    $process = [Diagnostics.Process]::Start($launch)
    if (!$process.WaitForInputIdle(15000)) { throw 'WindowCloud did not start a Windows message loop.' }
    $process.Refresh()
    if ($process.HasExited -or $process.MainWindowHandle -ne [IntPtr]::Zero) {
        throw 'Expected WindowCloud to run in the background.'
    }
    $loadedCoreClr = $process.Modules | Where-Object ModuleName -eq 'coreclr.dll'
    if ($loadedCoreClr.FileName -ne (Join-Path $testRoot 'coreclr.dll')) {
        throw 'WindowCloud did not load CoreCLR from the extracted ZIP.'
    }
    Write-Output "Verified the Explorer DLL and background launch with CoreCLR loaded from the extracted ZIP."
}
finally {
    if ($process) {
        if (!$process.HasExited) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
        }
        $process.Dispose()
    }
    $directory = Get-Item -LiteralPath $testRoot
    if ($directory.FullName -eq $testRoot -and $directory.Parent.FullName -eq $tempRoot -and
        -not ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force
    }
}
