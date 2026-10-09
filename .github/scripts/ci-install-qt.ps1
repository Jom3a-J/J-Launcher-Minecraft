# Installs the pinned Qt SDK with aqtinstall. Shared by the pull request check (build.yml) and
# the cache seed (cache-seed.yml), which saves the result for pull requests to reuse.
$ErrorActionPreference = 'Stop'

python -m pip install --disable-pip-version-check aqtinstall==3.3.0
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to install aqtinstall 3.3.0.'
}
$sevenZip = (Get-Command 7z -ErrorAction Stop).Source
$qtPrefix = Join-Path $env:QT_ROOT "$env:QT_VERSION\msvc2022_64"
New-Item -ItemType Directory -Force -Path $qtPrefix | Out-Null
$aqtConfig = (Resolve-Path -LiteralPath (Join-Path $env:GITHUB_WORKSPACE '.github\aqt-settings.ini')).Path
$aqtWorkingDirectory = Join-Path $env:RUNNER_TEMP 'aqt-work'
New-Item -ItemType Directory -Force -Path $aqtWorkingDirectory | Out-Null
$aqtExitCode = 1
Push-Location $aqtWorkingDirectory
try {
    python -m aqt --config $aqtConfig `
        install-qt windows desktop $env:QT_VERSION $env:QT_ARCH `
        -m qtimageformats qtnetworkauth --outputdir $env:QT_ROOT --external $sevenZip
    $aqtExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($aqtExitCode -ne 0) {
    throw "Unable to install Qt $env:QT_VERSION."
}

if (-not (Test-Path -LiteralPath "$qtPrefix\lib\cmake\Qt6" -PathType Container)) {
    throw "Qt CMake package was not installed at $qtPrefix."
}
