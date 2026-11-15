param(
    [string] $PackageRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
$editor = Join-Path $package 'bin/WorldForgeEditor.exe'
$player = Join-Path $package 'bin/WorldForgePlayer.exe'
$fixture = Join-Path $package 'qa/equipment-case.wfplay'
foreach ($file in @($editor, $player, $fixture)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required package file is missing: $file"
    }
}

Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber | Format-List
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion | Format-Table -AutoSize
Get-FileHash -LiteralPath $editor, $player | Format-List

$previousPath = $env:PATH
$previousQtPluginPath = $env:QT_PLUGIN_PATH
$previousQtPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $null
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $null
    $app = Start-Process -FilePath $editor -WindowStyle Hidden -PassThru
    try {
        if (!$app.WaitForInputIdle(15000) -or $app.HasExited) {
            throw 'The packaged editor did not start. Check the graphics driver and the bundled VC++ redistributable.'
        }
        $qtModules = @($app.Modules | Where-Object ModuleName -in 'Qt6Core.dll', 'qwindows.dll')
        if ($qtModules.Count -ne 2 -or @($qtModules | Where-Object {
            !$_.FileName.StartsWith($package + '\', [StringComparison]::OrdinalIgnoreCase)
        }).Count) {
            throw 'The editor did not load the bundled Qt runtime and platform plugin.'
        }
        Write-Output 'PASS: editor startup with bundled Qt'
    } finally {
        if (!$app.HasExited) { Stop-Process -InputObject $app }
    }

    foreach ($mode in @('--smoke-test-warp', '--smoke-test')) {
        $run = Start-Process -FilePath $player -ArgumentList @($mode, ('"' + $fixture + '"')) -WindowStyle Hidden -PassThru
        try {
            if (!$run.WaitForExit(30000)) { throw "Player timed out in $mode" }
            if ($run.ExitCode -ne 0) { throw "Player $mode exited with code $($run.ExitCode)" }
            Write-Output "PASS: Player $mode"
        } finally {
            if (!$run.HasExited) { Stop-Process -InputObject $run }
        }
    }
} finally {
    $env:PATH = $previousPath
    $env:QT_PLUGIN_PATH = $previousQtPluginPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $previousQtPlatformPath
}
