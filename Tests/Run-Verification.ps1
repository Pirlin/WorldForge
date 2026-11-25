<#
.SYNOPSIS
Builds build/audit and runs the WorldForge checks one at a time, printing a short summary per check.

.DESCRIPTION
This is the verification loop described in Docs/WorldForge-Development-Playbook.md. Run it through PowerShell (not
powershell.exe from Bash: this machine's execution policy blocks .ps1 files started that way).

Each audit mode writes build/audit/<mode>-<Tag>/controls.csv. The summary prints the PASS/FAIL/INFO counts, every
failing row, the frame times of timing audits, and the realism score. Audits run strictly one after another with a
visible window: two at once, or a hidden window, make the rendering checks fail ("No Vulkan frame").

.PARAMETER Modes
Audit modes: rendering, street, realism, r3-performance, district, full (the default full editor audit), or any
other mode WorldForgeEditorAudit accepts. Order matters only for timing: run timing modes together.

.PARAMETER Tag
Suffix for the output folders, so runs do not overwrite each other.

.PARAMETER NoBuild
Skip the build (for example while comparing timings of one build).

.PARAMETER CTest
Also run ctest (all tests except the full editor audit, which the "full" mode runs).

.PARAMETER Repeat
Run the timing modes (street, r3-performance, district) this many times, to see how much timings vary.

.EXAMPLE
& .\Tests\Run-Verification.ps1 -Modes rendering,street -Tag fog
& .\Tests\Run-Verification.ps1 -Modes rendering,street,realism,r3-performance,district,full -Tag release -CTest
#>
param(
    [string[]]$Modes = @("rendering","street"),
    [string]$Tag = "run",
    [switch]$NoBuild,
    [switch]$CTest,
    [int]$Repeat = 1
)

$repo = Split-Path -Parent $PSScriptRoot
$devShell = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
if (-not (Test-Path $devShell)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $installer) {
        $vs = & $installer -latest -property installationPath
        $devShell = Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1"
    }
}
# The dev shell prints "'vswhere.exe' is not recognized"; that line is harmless.
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation 2>&1 | Out-Null
$qtRoot = if ($env:WORLDFORGE_QT_ROOT) { $env:WORLDFORGE_QT_ROOT } else { "$env:LOCALAPPDATA\WorldForge\Qt\6.8.3\msvc2022_64" }
$env:PATH = "$qtRoot\bin;$env:PATH"
# The street audit's Forge checks need the embedded Studio.
$env:WORLDFORGE_STUDIO_TEST_FORGE = "1"
Set-Location $repo

if (-not $NoBuild) {
    $build = cmake --build build/audit --parallel 6 2>&1
    $buildExit = $LASTEXITCODE
    $build | Select-String -Pattern "error|warning C|FAILED" | Select-Object -First 20
    "build: $($build | Select-Object -Last 1)"
    if ($buildExit -ne 0) {
        "Build failed; nothing was run."
        exit 1
    }
}

$failed = $false
if ($CTest) {
    Push-Location build\audit
    "== ctest"
    ctest -E EditorAudit --output-on-failure 2>&1 | Select-Object -Last 8
    $failed = $LASTEXITCODE -ne 0
    Pop-Location
}

$timing = @("street","r3-performance","street-profile","district","district-profile","district-large","district-textured","district-compare",
    "district-occlusion","district-streaming","district-meshlets","district-meshlets-indexed")
foreach ($mode in $Modes) {
    $runs = if ($timing -contains $mode) { [Math]::Max($Repeat,1) } else { 1 }
    for ($run = 1; $run -le $runs; ++$run) {
        $suffix = if ($runs -gt 1) { "$Tag-$run" } else { $Tag }
        $out = "$repo\build\audit\$mode-$suffix"
        if (Test-Path -LiteralPath $out) {
            $out += "-" + [Guid]::NewGuid().ToString("N").Substring(0,8)
        }
        $arguments = @($out,$repo)
        if ($mode -ne "full") { $arguments += "--$mode" }
        New-Item -ItemType Directory -Path $out | Out-Null
        $process = Start-Process "$repo\build\audit\WorldForgeEditorAudit.exe" -ArgumentList $arguments -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log"
        if (-not (Test-Path "$out\controls.csv")) {
            "== $mode : no controls.csv (the audit did not start or crashed)"
            $failed = $true
            continue
        }
        $csv = Import-Csv "$out\controls.csv"
        if ($process.ExitCode -ne 0 -or $csv.Result -contains "FAIL") { $failed = $true }
        $groups = ($csv | Group-Object Result | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ", "
        "== $mode$(if ($runs -gt 1) { " (run $run)" }) : $groups"
        $csv | Where-Object { $_.Result -eq "FAIL" } | ForEach-Object { "FAIL: $($_.Control) :: $($_.Notes)" }
        $times = [regex]::Matches((Get-Content "$out\controls.csv" -Raw),'frame avg [0-9.]+ ms p95 [0-9.]+ ms') |
            ForEach-Object { $_.Value } | Select-Object -Unique
        if ($times) { "times: $($times -join ' | ')" }
        if ($mode -eq "realism") { $csv | Where-Object { $_.Control -match "Realism" } | ForEach-Object { "$($_.Control): $($_.Notes)" } }
        if ($mode -eq "aerial-perspective") { $csv | ForEach-Object { "$($_.Result) $($_.Control): $($_.Notes)" } }
        if ($mode -like "district*" -or $mode -eq "street-profile") { $csv | Where-Object { $_.Result -eq "INFO" -or $_.Kind -eq "profile" } | ForEach-Object { "$($_.Control): $($_.Notes)" } }
    }
}
if ($failed) { exit 1 }
