param(
    [string] $StudioRoot = "$env:LOCALAPPDATA/WorldForge/StudioSource",
    [string] $VisualStudio = 'C:/Program Files/Microsoft Visual Studio/2022/Community',
    [ValidateRange(1, 64)] [int] $Jobs = 6
)

$ErrorActionPreference = 'Stop'
$StudioRoot = [IO.Path]::GetFullPath($StudioRoot)
$source = Join-Path $StudioRoot 'blender'
$dependencies = Join-Path $StudioRoot 'windows-x64-deps'
$build = Join-Path $StudioRoot 'build-v5.2.3-x64'
$fixture = Join-Path $StudioRoot 'fixture-v5.2.3-x64'
$sourceCommit = 'e13b728c6880d3084d9d57c30031ed5df8b39dee'
$dependencyCommit = '60d6e96b917568278d400a4024c98da0fb777338'

function Invoke-Logged {
    param([string] $Program, [string[]] $Arguments, [string] $Log)
    Write-Output "Running $Program. Log is $Log"
    & $Program @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $Log -Tail 30 | Write-Host
        throw "$Program exited with code $LASTEXITCODE. See $Log"
    }
}

foreach ($checkout in @(
    @{ Path = $source; Commit = $sourceCommit },
    @{ Path = $dependencies; Commit = $dependencyCommit }
)) {
    $head = & git -C $checkout.Path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -ne $checkout.Commit) {
        throw "Expected commit $($checkout.Commit) at $($checkout.Path)."
    }
    & git -C $checkout.Path diff --quiet HEAD --
    if ($LASTEXITCODE -ne 0) {
        throw "Expected a clean checkout at $($checkout.Path)."
    }
    $inventory = & git -C $checkout.Path lfs ls-files --json
    if ($LASTEXITCODE -ne 0) {
        throw "Could not verify large-file checkout at $($checkout.Path)."
    }
    $missing = ($inventory | Out-String | ConvertFrom-Json).files | Where-Object {
        !$_.checkout -and $(if ($checkout.Path -eq $dependencies) {
            $_.name -notmatch '(^|/)debug/|_d\.(dll|lib|pdb)$|\.pdb$'
        } else {
            $_.name -notlike 'tests/*'
        })
    }
    if ($missing) {
        throw "$($missing.Count) required large files are missing from $($checkout.Path)."
    }
}

& "$VisualStudio/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$tools = Join-Path $VisualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $tools 'CMake/bin/cmake.exe'
$ninja = Join-Path $tools 'Ninja/ninja.exe'
$install = Join-Path $build 'bin'

Invoke-Logged $cmake @('-S', $source, '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$install", "-DLIBDIR=$dependencies") "$StudioRoot/phase0-configure.log"
Invoke-Logged $cmake @('--build', $build, '--parallel', "$Jobs", '--target', 'install') "$StudioRoot/phase0-build.log"

$editor = Join-Path $install 'blender.exe'
$testScript = Join-Path $PSScriptRoot 'Verify-StudioBaseline.py'
Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
    '--python-exit-code', '1', '--python', $testScript, '--', '--output', $fixture) "$StudioRoot/phase0-fixture.log"

$resultPath = Join-Path $fixture 'baseline-result.json'
$result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
if ($result.status -ne 'PASS' -or [IO.Path]::GetFullPath($result.binary) -ne $editor) {
    throw "The fixture did not pass using the expected executable. See $resultPath"
}
$evidence = [ordered] @{
    source_commit = $sourceCommit
    dependency_commit = $dependencyCommit
    msvc = $env:VCToolsVersion
    windows_sdk = $env:WindowsSDKVersion
    cmake = (& $cmake --version | Select-Object -First 1)
    ninja = (& $ninja --version)
    binary_sha256 = (Get-FileHash -LiteralPath $editor).Hash
    fixture = $result
}
$evidence | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$StudioRoot/phase0-build-evidence.json" -Encoding utf8
Write-Output "Baseline build and fixture passed. Evidence is $StudioRoot/phase0-build-evidence.json"
