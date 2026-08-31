<#
.SYNOPSIS
Builds, tests, stages, and audits a local Windows x64 release preflight package.

.DESCRIPTION
This script creates local qualification evidence only. It deliberately does
not tag, sign, publish, upload, or claim that an artifact is ready. A release
still requires a clean tagged source tree, hosted provenance,
post-reboot cold-start evidence, fresh-machine testing, accurate disclosure of
its unsigned status, and explicit maintainer approval.
#>

[CmdletBinding()]
param(
    [string] $BuildDirectory = (Join-Path $PSScriptRoot '..\build-migration-build2'),

    [string] $OutputDirectory,

    [string] $QtRoot,

    [string] $CMakeToolchainFile,

    [ValidateSet('Ninja Multi-Config', 'Visual Studio 17 2022')]
    [string] $Generator = 'Ninja Multi-Config',

    [ValidatePattern('^jlauncher-\d+\.\d+\.\d+(?:-beta\.\d+)?$')]
    [string] $ExpectedTag,

    [ValidateSet('beta', 'stable')]
    [string] $ReleaseStage = 'stable',

    [string] $CurseForgeApiKey = $env:CURSEFORGE_API_KEY,

    [ValidateRange(1, 16)]
    [int] $ParallelJobs = 2,

    [switch] $SkipBuild,

    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [Parameter(Mandatory = $true)]
        [string[]] $ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE."
    }
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string] $CachePath,

        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $escapedName = [Regex]::Escape($Name)
    $line = Get-Content -LiteralPath $CachePath |
        Where-Object { $_ -match "^${escapedName}(?::[^=]+)?=" } |
        Select-Object -Last 1
    if (-not $line) {
        return $null
    }
    return ($line -replace '^[^=]+=', '')
}

function Get-RelativeFileName {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Root,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $prefix = $Root.TrimEnd('\') + '\'
    if (-not $Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the expected root: $Path"
    }
    return $Path.Substring($prefix.Length)
}

function Get-PeMachine {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a Windows PE file: $Path"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Invalid PE signature: $Path"
        }
        return ('0x{0:X4}' -f $reader.ReadUInt16())
    }
    finally {
        $stream.Dispose()
    }
}

function Enable-MsvcEnvironment {
    if ($env:VSCMD_VER -and $env:INCLUDE -and $env:LIB) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio locator (vswhere.exe) was not found.'
    }

    $installationPath = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
        throw 'A Visual Studio installation with the x64 C++ tools was not found.'
    }

    $developerShell = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $developerShell -PathType Leaf)) {
        throw "Visual Studio developer shell was not found: $developerShell"
    }

    $environmentLines = & $env:ComSpec '/s' '/c' `
        "`"$developerShell`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw 'Visual Studio x64 developer environment initialization failed.'
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }

    if (-not $env:VSCMD_VER -or -not $env:INCLUDE -or -not $env:LIB) {
        throw 'Visual Studio initialized without the required x64 compiler paths.'
    }
}

$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$build = [IO.Path]::GetFullPath($BuildDirectory)
$buildExists = Test-Path -LiteralPath $build -PathType Container
if (-not $buildExists) {
    if ($SkipBuild) {
        throw "Build directory does not exist and -SkipBuild was requested: $build"
    }
    $null = New-Item -ItemType Directory -Path $build
}

$cachePath = Join-Path $build 'CMakeCache.txt'
if ($SkipBuild -and -not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache not found: $cachePath"
}

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $build "phase9-preflight-$stamp"
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$buildPrefix = $build.TrimEnd('\') + '\'
if (-not $output.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be a new directory inside the selected build directory.'
}
if (Test-Path -LiteralPath $output) {
    throw "Output directory already exists: $output"
}

$null = New-Item -ItemType Directory -Path $output
$portable = Join-Path $output 'portable'

$head = (& git -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to determine the source commit.'
}
$branchLines = @(& git -C $repository branch --show-current)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to determine the source branch.'
}
$branch = if ($branchLines.Count -gt 0) {
    $branchLines[-1].Trim()
}
else {
    $null
}
$statusLines = @(& git -C $repository status --short --untracked-files=all `
    --ignore-submodules=none)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect the source worktree.'
}
$sourceDirty = $statusLines.Count -gt 0

$exactTag = $null
$tagAnnotated = $false
$tagsAtHead = @(& git -C $repository tag --points-at HEAD)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect tags at the source commit.'
}
if ($tagsAtHead.Count -gt 0) {
    $exactTag = $tagsAtHead[0].Trim()
    $tagType = @(& git -C $repository cat-file -t "refs/tags/$exactTag")
    $tagAnnotated = $LASTEXITCODE -eq 0 -and $tagType.Count -gt 0 -and
        $tagType[-1].Trim() -eq 'tag'
}

$exactTagVerified = $false
$expectedProductVersion = $null
$expectedVersionChannel = $ReleaseStage
$tagReleaseVersion = $null
if ($ExpectedTag) {
    $expectedTagPattern = if ($ReleaseStage -eq 'beta') {
        '^jlauncher-(\d+\.\d+\.\d+)-beta\.(\d+)$'
    }
    else {
        '^jlauncher-(\d+\.\d+\.\d+)$'
    }
    if ($ExpectedTag -notmatch $expectedTagPattern) {
        throw "Expected tag $ExpectedTag does not match release stage $ReleaseStage."
    }
    $expectedProductVersion = $Matches[1]
    if ($ReleaseStage -eq 'beta') {
        $expectedVersionChannel = "beta.$($Matches[2])"
    }
    $tagReleaseVersion = $ExpectedTag -replace '^jlauncher-', ''

    & git -C $repository show-ref --verify --quiet "refs/tags/$ExpectedTag"
    if ($LASTEXITCODE -ne 0) {
        throw "Expected tag does not exist: $ExpectedTag"
    }
    $expectedTagType = @(& git -C $repository cat-file -t "refs/tags/$ExpectedTag")
    if ($LASTEXITCODE -ne 0 -or $expectedTagType.Count -eq 0 -or
        $expectedTagType[-1].Trim() -ne 'tag') {
        throw "Expected tag is missing or is not annotated: $ExpectedTag"
    }

    $tagCommit = (& git -C $repository rev-list -n 1 "refs/tags/$ExpectedTag").Trim()
    if ($LASTEXITCODE -ne 0 -or $tagCommit -ne $head) {
        throw "Expected tag $ExpectedTag does not resolve to checked-out commit $head."
    }
    if ($tagsAtHead -notcontains $ExpectedTag) {
        throw "Checked-out commit is not exactly at expected tag $ExpectedTag."
    }
    $exactTag = $ExpectedTag
    $tagAnnotated = $true
    $exactTagVerified = $true
}

Enable-MsvcEnvironment

if (-not $SkipBuild) {
    if ([string]::IsNullOrWhiteSpace($CurseForgeApiKey)) {
        throw 'A J Launcher-owned CurseForge API key is required for release builds. Set CURSEFORGE_API_KEY or pass -CurseForgeApiKey.'
    }
    $configureArguments = @(
        '-S', $repository,
        '-B', $build,
        '-G', $Generator,
        '-DBUILD_TESTING=ON',
        "-DLauncher_RELEASE_STAGE=$ReleaseStage",
        "-DLauncher_VERSION_CHANNEL=$expectedVersionChannel",
        '-DLauncher_BUILD_PLATFORM=official',
        '-DLauncher_BUILD_ARTIFACT=Windows-MSVC-x64',
        '-DLauncher_ENABLE_JAVA_DOWNLOADER=ON',
        "-DLauncher_CURSEFORGE_API_KEY=$CurseForgeApiKey",
        '-DLauncher_UPDATER_GITHUB_REPO=',
        '-DENABLE_LTO=ON'
    )
    if ($QtRoot) {
        $resolvedQtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
        $configureArguments += "-DCMAKE_PREFIX_PATH=$resolvedQtRoot"
    }
    if ($CMakeToolchainFile) {
        $resolvedToolchain = (Resolve-Path -LiteralPath $CMakeToolchainFile).Path
        $configureArguments += "-DCMAKE_TOOLCHAIN_FILE=$resolvedToolchain"
        $configureArguments += '-DVCPKG_TARGET_TRIPLET=x64-windows'
    }

    Invoke-Checked -FilePath 'cmake' -ArgumentList $configureArguments
    Invoke-Checked -FilePath 'cmake' -ArgumentList @(
        '--build', $build,
        '--config', 'Release',
        '--parallel', "$ParallelJobs"
    )
}

if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache not found after configuration: $cachePath"
}

$releaseStage = Get-CMakeCacheValue -CachePath $cachePath -Name 'Launcher_RELEASE_STAGE'
$versionChannel = Get-CMakeCacheValue -CachePath $cachePath -Name 'Launcher_VERSION_CHANNEL'
$buildPlatform = Get-CMakeCacheValue -CachePath $cachePath -Name 'Launcher_BUILD_PLATFORM'
$ltoEnabled = Get-CMakeCacheValue -CachePath $cachePath -Name 'ENABLE_LTO'
$configuredCurseForgeApiKey = Get-CMakeCacheValue -CachePath $cachePath -Name 'Launcher_CURSEFORGE_API_KEY'
if ($releaseStage -ne $ReleaseStage) {
    throw "The build cache release stage is $releaseStage; expected $ReleaseStage."
}
if ($versionChannel -ne $expectedVersionChannel) {
    throw "The build cache version channel is $versionChannel; expected $expectedVersionChannel."
}
if ($buildPlatform -ne 'official') {
    throw "The build cache is not configured for the official platform: $buildPlatform"
}
if ($ltoEnabled -ne 'ON') {
    throw "The build cache does not have Release LTO enabled: $ltoEnabled"
}
if ([string]::IsNullOrWhiteSpace($configuredCurseForgeApiKey)) {
    throw 'The release build cache does not contain the required CurseForge API key.'
}

$qtRoot = Get-CMakeCacheValue -CachePath $cachePath -Name 'CMAKE_PREFIX_PATH'
if ($qtRoot) {
    $qtBin = Join-Path $qtRoot 'bin'
    if (Test-Path -LiteralPath $qtBin -PathType Container) {
        $env:Path = "$qtBin;$env:Path"
    }
}

if (-not $SkipTests) {
    Invoke-Checked -FilePath 'ctest' -ArgumentList @(
        '--test-dir', $build,
        '-C', 'Release',
        '--output-on-failure'
    )
}

Invoke-Checked -FilePath 'cmake' -ArgumentList @(
    '--install', $build,
    '--config', 'Release',
    '--prefix', $portable
)
Invoke-Checked -FilePath 'cmake' -ArgumentList @(
    '--install', $build,
    '--config', 'Release',
    '--prefix', $portable,
    '--component', 'portable'
)

$requiredFiles = @(
    'jlauncher.exe',
    'jlauncher_filelink.exe',
    'portable.txt',
    'qt.conf',
    'Qt6Core.dll',
    'Qt6Gui.dll',
    'Qt6Network.dll',
    'Qt6NetworkAuth.dll',
    'Qt6Widgets.dll',
    'platforms\qwindows.dll',
    'jars\JavaCheck.jar',
    'jars\NewLaunch.jar',
    'jars\NewLaunchLegacy.jar',
    'LICENSE',
    'COPYING.md',
    'vc_redist.x64.exe'
)
$missingFiles = @($requiredFiles | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $portable $_) -PathType Leaf)
})
if ($missingFiles.Count -gt 0) {
    throw "Portable package is missing required files: $($missingFiles -join ', ')"
}
$redistributablePath = Join-Path $portable 'vc_redist.x64.exe'
$redistributableSignature = [string](
    Get-AuthenticodeSignature -LiteralPath $redistributablePath
).Status
if ($redistributableSignature -ne 'Valid') {
    throw "Microsoft Visual C++ redistributable signature is not valid: $redistributableSignature"
}

$forbiddenNames = @(
    'accounts.json',
    'jlauncher.cfg',
    'prismlauncher.cfg',
    'servers.json'
)
$leakedProfileFiles = @(
    Get-ChildItem -LiteralPath $portable -Recurse -File |
        Where-Object { $forbiddenNames -contains $_.Name } |
        ForEach-Object { Get-RelativeFileName -Root $portable -Path $_.FullName }
)
if ($leakedProfileFiles.Count -gt 0) {
    throw "Package contains profile/private state: $($leakedProfileFiles -join ', ')"
}

$debugRuntimeFiles = @(
    Get-ChildItem -LiteralPath $portable -Recurse -File |
        Where-Object { $_.Name -match '^(Qt6.*d|.*140d)\.dll$' } |
        Select-Object -ExpandProperty Name
)
if ($debugRuntimeFiles.Count -gt 0) {
    throw "Release package contains debug runtimes: $($debugRuntimeFiles -join ', ')"
}

$launcherPath = Join-Path $portable 'jlauncher.exe'
$launcherInfo = (Get-Item -LiteralPath $launcherPath).VersionInfo
if ($launcherInfo.ProductName -ne 'J Launcher' -or $launcherInfo.FileDescription -ne 'J Launcher') {
    throw 'Launcher version resources do not identify J Launcher.'
}
$machine = Get-PeMachine -Path $launcherPath
if ($machine -ne '0x8664') {
    throw "Launcher is not an x64 PE executable: $machine"
}

$manifestPath = Join-Path $portable 'manifest.txt'
$manifest = @(
    Get-ChildItem -LiteralPath $portable -Recurse -File |
        Where-Object FullName -NE $manifestPath |
        ForEach-Object { Get-RelativeFileName -Root $portable -Path $_.FullName } |
        Sort-Object
)
$manifest | Set-Content -LiteralPath $manifestPath -Encoding utf8

$version = $launcherInfo.ProductVersion -replace '\.0$', ''
if (-not $version) {
    throw 'Launcher product version is empty.'
}
if ($ExpectedTag) {
    if ($version -ne $expectedProductVersion) {
        throw "Launcher product version $version does not match expected tag $ExpectedTag."
    }
}
$releaseVersion = if ($tagReleaseVersion) { $tagReleaseVersion } else { $version }
$portableZipName = "JLauncher-Windows-x64-Portable-$releaseVersion.zip"
$portableZip = Join-Path $output $portableZipName
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $portableZip -CompressionLevel Optimal

$portableHash = (Get-FileHash -LiteralPath $portableZip -Algorithm SHA256).Hash
"$portableHash *$portableZipName" |
    Set-Content -LiteralPath "$portableZip.sha256" -Encoding ascii

$installerPath = $null
$installerHash = $null
$installerSignature = 'NotProduced'
$installerPayloadAudited = $false
$makensisCommand = Get-Command 'makensis.exe' -ErrorAction SilentlyContinue
$makensisPath = $null
if ($makensisCommand) {
    $makensisPath = $makensisCommand.Source
}
if (-not $makensisPath) {
    $standardNsis = 'C:\Program Files (x86)\NSIS\makensis.exe'
    if (Test-Path -LiteralPath $standardNsis -PathType Leaf) {
        $makensisPath = $standardNsis
    }
}
if ($makensisPath) {
    $nsiPath = Join-Path $build 'program_info\win_install.nsi'
    $installerSupport = Join-Path $output 'program_info'
    $null = New-Item -ItemType Directory -Path $installerSupport
    Copy-Item -LiteralPath (Join-Path $repository 'program_info\jlauncher.ico') `
        -Destination (Join-Path $installerSupport 'jlauncher.ico')
    Push-Location $portable
    try {
        Invoke-Checked -FilePath $makensisPath -ArgumentList @('-NOCD', $nsiPath)
    }
    finally {
        Pop-Location
    }

    $generatedInstaller = Join-Path $output 'JLauncher-Setup.exe'
    if (-not (Test-Path -LiteralPath $generatedInstaller -PathType Leaf)) {
        throw 'NSIS completed but did not produce JLauncher-Setup.exe.'
    }
    $installerName = "JLauncher-Windows-x64-Setup-$releaseVersion.exe"
    $installerPath = Join-Path $output $installerName
    Move-Item -LiteralPath $generatedInstaller -Destination $installerPath
    $installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
    "$installerHash *$installerName" |
        Set-Content -LiteralPath "$installerPath.sha256" -Encoding ascii

    $installerInfo = (Get-Item -LiteralPath $installerPath).VersionInfo
    if ($installerInfo.ProductName -ne 'J Launcher' -or
        $installerInfo.FileDescription -ne 'J Launcher Installer') {
        throw 'Installer version resources do not identify J Launcher.'
    }

    $sevenZipCommand = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    $sevenZipPath = $null
    if ($sevenZipCommand) {
        $sevenZipPath = $sevenZipCommand.Source
    }
    if (-not $sevenZipPath) {
        $standardSevenZip = 'C:\Program Files\7-Zip\7z.exe'
        if (Test-Path -LiteralPath $standardSevenZip -PathType Leaf) {
            $sevenZipPath = $standardSevenZip
        }
    }
    if (-not $sevenZipPath) {
        throw '7-Zip is required to audit the generated NSIS payload.'
    }

    $installerListing = @(& $sevenZipPath 'l' '-slt' '--' $installerPath)
    if ($LASTEXITCODE -ne 0) {
        throw '7-Zip could not inspect the generated NSIS installer.'
    }
    $installerPaths = @(
        $installerListing |
            Where-Object { $_ -match '^Path = (.+)$' } |
            ForEach-Object { ($_ -replace '^Path = ', '').Replace('/', '\') }
    )
    $requiredInstallerFiles = @(
        'jlauncher.exe',
        'jlauncher_filelink.exe',
        'qt.conf',
        'qtlogging.ini',
        'LICENSE',
        'COPYING.md',
        'Qt6Core.dll',
        'Qt6Gui.dll',
        'Qt6Network.dll',
        'Qt6NetworkAuth.dll',
        'Qt6Widgets.dll',
        'platforms\qwindows.dll',
        'jars\JavaCheck.jar',
        'jars\NewLaunch.jar',
        'jars\NewLaunchLegacy.jar',
        'uninstall.exe',
        'vc_redist.x64.exe'
    )
    $missingInstallerFiles = @($requiredInstallerFiles | Where-Object {
        $installerPaths -notcontains $_
    })
    if ($missingInstallerFiles.Count -gt 0) {
        throw "Installer payload is missing required files: $($missingInstallerFiles -join ', ')"
    }
    $forbiddenInstallerFiles = @($installerPaths | Where-Object {
        $forbiddenNames -contains (Split-Path -Leaf $_) -or
        (Split-Path -Leaf $_) -match '^(Qt6.*d|.*140d)\.dll$'
    })
    if ($forbiddenInstallerFiles.Count -gt 0) {
        throw "Installer contains forbidden profile/debug files: $($forbiddenInstallerFiles -join ', ')"
    }
    $installerPayloadAudited = $true
    $installerSignature = [string](Get-AuthenticodeSignature -LiteralPath $installerPath).Status
    Remove-Item -LiteralPath (Join-Path $installerSupport 'jlauncher.ico')
    Remove-Item -LiteralPath $installerSupport
}

$launcherSignature = [string](Get-AuthenticodeSignature -LiteralPath $launcherPath).Status
$signatureStateMatchesPolicy = $launcherSignature -eq 'NotSigned' -and
    $installerSignature -eq 'NotSigned'
$hostedBuild = $env:GITHUB_ACTIONS -eq 'true' -and
    -not [string]::IsNullOrWhiteSpace($env:GITHUB_REPOSITORY) -and
    -not [string]::IsNullOrWhiteSpace($env:GITHUB_RUN_ID)
$hostedProvenanceUrl = if ($hostedBuild) {
    "$($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)"
}
else {
    $null
}
$readyForExternalGates = (
    -not $sourceDirty -and
    $exactTagVerified -and
    $installerPath -and
    $signatureStateMatchesPolicy
)

$openExternalGates = [Collections.Generic.List[string]]::new()
if (-not $exactTagVerified) {
    $openExternalGates.Add('exact clean annotated tag')
}
if (-not $hostedBuild) {
    $openExternalGates.Add('hosted build provenance')
}
$openExternalGates.Add('certified post-reboot cold-start sample')
$openExternalGates.Add('genuinely fresh Windows x64 standard-user smoke')
$openExternalGates.Add('explicit maintainer publication approval')

$report = [ordered]@{
    status = 'passed-local-preflight'
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    repository = $repository
    sourceCommit = $head
    sourceBranch = $branch
    sourceDirty = $sourceDirty
    sourceChangeCount = $statusLines.Count
    exactTag = $exactTag
    expectedTag = $ExpectedTag
    tagAnnotated = $tagAnnotated
    exactTagVerified = $exactTagVerified
    hostedBuild = $hostedBuild
    hostedProvenanceUrl = $hostedProvenanceUrl
    runnerImage = $env:ImageOS
    runnerImageVersion = $env:ImageVersion
    buildDirectory = $build
    releaseStage = $releaseStage
    versionChannel = $versionChannel
    buildPlatform = $buildPlatform
    ltoEnabled = $ltoEnabled
    parallelJobs = $ParallelJobs
    buildExecuted = (-not $SkipBuild)
    testsExecuted = (-not $SkipTests)
    productVersion = $version
    releaseVersion = $releaseVersion
    peMachine = $machine
    portableFileCount = $manifest.Count + 1
    portableZip = $portableZipName
    portableSha256 = $portableHash
    installer = $(if ($installerPath) { Split-Path -Leaf $installerPath } else { $null })
    installerSha256 = $installerHash
    launcherSignatureStatus = $launcherSignature
    installerSignatureStatus = $installerSignature
    msvcRedistributableSignatureStatus = $redistributableSignature
    expectedReleaseSignatureStatus = 'NotSigned'
    unsignedStableReleaseAccepted = ($ReleaseStage -eq 'stable')
    unsignedBetaReleaseAccepted = ($ReleaseStage -eq 'beta')
    signatureStateMatchesPolicy = $signatureStateMatchesPolicy
    installerPayloadAudited = $installerPayloadAudited
    profileStateAbsent = $true
    debugRuntimesAbsent = $true
    requiredFilesPresent = $true
    readyForExternalGates = [bool]$readyForExternalGates
    publicationReady = $false
    openExternalGates = @($openExternalGates)
}
$reportPath = Join-Path $output 'phase9-preflight-report.json'
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json -Depth 5

Write-Warning "Local $ReleaseStage preflight passed, but the external Phase 9 gates remain open. This output is not a published release."
