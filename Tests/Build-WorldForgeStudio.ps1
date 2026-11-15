param(
    [string] $StudioRoot = "$env:LOCALAPPDATA/WorldForge/StudioSource",
    [string] $VisualStudio = 'C:/Program Files/Microsoft Visual Studio/2022/Community',
    [string] $WixRoot,
    [switch] $AssetDocuments,
    [ValidateRange(1, 64)] [int] $Jobs = 6
)

$ErrorActionPreference = 'Stop'
$StudioRoot = [IO.Path]::GetFullPath($StudioRoot)
$source = Join-Path $StudioRoot 'worldforge-studio'
$dependencies = Join-Path $StudioRoot 'windows-x64-deps'
$build = Join-Path $StudioRoot 'build-studio-x64'
$install = Join-Path $build 'bin'
$fixture = Join-Path $StudioRoot 'fixture-studio-x64'
if ($AssetDocuments) {
    $source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../StudioSource/blender'))
    $build = Join-Path $StudioRoot 'build-studio-assets-x64'
    $install = Join-Path $build 'bin'
    $fixture = Join-Path $StudioRoot 'fixture-studio-assets-x64'
}
$phase = if ($AssetDocuments) { 'phase2' } else { 'phase1' }

function Invoke-Logged {
    param([string] $Program, [string[]] $Arguments, [string] $Log)
    Write-Output "Running $Program. Log is $Log"
    & $Program @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) {
        $code = $LASTEXITCODE
        Get-Content -LiteralPath $Log -Tail 30 | Write-Host
        throw "$Program exited with code $code. See $Log"
    }
}

$base = & git -C $source merge-base HEAD e13b728c6880d3084d9d57c30031ed5df8b39dee
if ($LASTEXITCODE -ne 0 -or $base -ne 'e13b728c6880d3084d9d57c30031ed5df8b39dee') {
    throw "The Studio checkout must descend from Blender v5.2.3."
}
$head = & git -C $source rev-parse --short=12 HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not identify the Studio revision."
}
$depHead = & git -C $dependencies rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $depHead -ne '60d6e96b917568278d400a4024c98da0fb777338') {
    throw "The Studio build needs the Windows dependencies pinned by Blender v5.2.3."
}
foreach ($checkout in $source, $dependencies) {
    $inventory = & git -C $checkout lfs ls-files --json
    if ($LASTEXITCODE -ne 0) {
        throw "Could not verify large-file checkout at $checkout."
    }
    $missing = ($inventory | Out-String | ConvertFrom-Json).files | Where-Object {
        !$_.checkout -and $(if ($checkout -eq $dependencies) {
            $_.name -notmatch '(^|/)debug/|_d\.(dll|lib|pdb)$|\.pdb$'
        } else {
            $_.name -notlike 'tests/*'
        })
    }
    if ($missing) {
        throw "$($missing.Count) required large files are missing from $checkout."
    }
}

& "$VisualStudio/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$tools = Join-Path $VisualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $tools 'CMake/bin/cmake.exe'
$ninja = Join-Path $tools 'Ninja/ninja.exe'
$cpack = Join-Path $tools 'CMake/bin/cpack.exe'
Invoke-Logged $cmake @('-S', $source, '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$install", "-DLIBDIR=$dependencies",
    "-DCPACK_OVERRIDE_PACKAGENAME=WorldForgeStudio-$phase") "$StudioRoot/$phase-configure.log"
Invoke-Logged $cmake @('--build', $build, '--parallel', "$Jobs", '--target', 'install') "$StudioRoot/$phase-build.log"

$editor = Join-Path $install 'WorldForgeStudio.exe'
$launcher = Join-Path $install 'WorldForgeStudioLauncher.exe'
foreach ($binary in $editor, $launcher) {
    $file = Get-Item -LiteralPath $binary
    if ($file.VersionInfo.ProductName -ne 'WorldForge Studio') {
        throw "Missing Studio product identity in $binary."
    }
}
$testScript = Join-Path $PSScriptRoot 'Verify-StudioBaseline.py'
Invoke-Logged $launcher @('--background', '--version') "$StudioRoot/$phase-launcher.log"
Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
    '--python-exit-code', '1', '--python', $testScript, '--', '--output', $fixture,
    '--build-hash', $head, '--studio') "$StudioRoot/$phase-fixture.log"

$result = Get-Content -LiteralPath "$fixture/baseline-result.json" -Raw | ConvertFrom-Json
if ($result.status -ne 'PASS' -or [IO.Path]::GetFullPath($result.binary) -ne $editor) {
    throw "The Studio fixture did not pass using the expected executable."
}
if ($AssetDocuments) {
    # An incremental install can still have files installed by the older fork.
    foreach ($folder in @('brushes', 'nodes')) {
        $builtIns = Join-Path $install "5.2/datafiles/assets/$folder"
        foreach ($library in Get-ChildItem -LiteralPath (Join-Path $source "assets/$folder") -Filter '*.blend' -File) {
            $legacy = Join-Path $builtIns ($library.BaseName + '.wflib')
            if (Test-Path -LiteralPath $legacy -PathType Leaf) {
                Remove-Item -LiteralPath $legacy
            }
        }
    }
    $libraryTest = Join-Path $PSScriptRoot 'Verify-StudioLibraries.py'
    $assetTest = Join-Path $PSScriptRoot 'Verify-StudioAsset.py'
    $assetModules = Join-Path $install '5.2/scripts/startup/bl_operators'
    $env:BLENDER_USER_CONFIG = Join-Path $fixture 'config'
    New-Item -ItemType Directory -Path $env:BLENDER_USER_CONFIG -Force | Out-Null
    Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
        '--python-exit-code', '1', '--python', $libraryTest) "$StudioRoot/phase2-library-fixture.log"
    $assetFixture = Join-Path $fixture 'portable-fixture.wfasset'
    Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
        '--python-exit-code', '1', '--python', $assetTest, '--', $assetModules,
        $assetFixture) "$StudioRoot/phase2-asset-fixture.log"
    Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
        '--python-exit-code', '1', $assetFixture, '--python-expr',
        "import bpy; from bl_operators.worldforge_documents import active_path; assert active_path().suffix == '.wfasset' and bpy.data.objects['Cube'].modifiers['Editable bevel'].segments == 3") "$StudioRoot/phase2-open-asset.log"
    $publishTest = Join-Path $PSScriptRoot 'Verify-StudioPublish.py'
    $bridge = Join-Path (Split-Path $PSScriptRoot) 'Workspaces/StudioPublish.py'
    Invoke-Logged $editor @('--background', '--factory-startup', '--disable-autoexec',
        '--python-exit-code', '1', '--python', $publishTest, '--', '--bridge', $bridge,
        '--output', "$StudioRoot/phase3-two-material") "$StudioRoot/phase3-two-material.log"
}
$package = Join-Path $StudioRoot "$phase-package"
Invoke-Logged $cpack @('--config', "$build/CPackConfig.cmake", '-G', 'ZIP',
    '-C', 'Release', '-B', $package) "$StudioRoot/$phase-package.log"
if (!$WixRoot -and (Test-Path -LiteralPath "$StudioRoot/wix-3.14.1/candle.exe")) {
    $WixRoot = Join-Path $StudioRoot 'wix-3.14.1'
}
if ($WixRoot) {
    $env:WIX = [IO.Path]::GetFullPath($WixRoot)
    $env:PATH = "$env:WIX;$env:PATH"
    # WiX 3 cannot read some deeply nested bundled Python files from the normal CPack path.
    $msiStage = Join-Path $env:USERPROFILE ('wfp-' + [Guid]::NewGuid().ToString('N').Substring(0,6))
    Invoke-Logged $cpack @('--config', "$build/CPackConfig.cmake", '-G', 'WIX',
        '-C', 'Release', '-B', $msiStage) "$StudioRoot/$phase-msi.log"
    Get-ChildItem -LiteralPath $msiStage -Filter '*.msi' -File |
        Copy-Item -Destination $package -Force
}
Write-Output "Studio build and fixture passed. Package directory is $package"
