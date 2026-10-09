# Configures the CI build. Shared by the pull request check (build.yml) and the cache seed
# (cache-seed.yml): the vcpkg packages the seed saves are only reused when both configure the
# build in exactly the same way, so neither should configure it any other way.
$ErrorActionPreference = 'Stop'

$qtPrefix = Join-Path $env:QT_ROOT "$env:QT_VERSION\msvc2022_64"
$toolchain = Join-Path $env:GITHUB_WORKSPACE 'cmake\vcpkg\scripts\buildsystems\vcpkg.cmake'
$configureArguments = @(
    '-S', $env:GITHUB_WORKSPACE,
    '-B', $env:BUILD_DIRECTORY,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DBUILD_TESTING=ON',
    '-DLauncher_USE_PCH=OFF',
    '-DENABLE_LTO=OFF',
    '-DLauncher_RELEASE_STAGE=development',
    '-DLauncher_BUILD_ARTIFACT=ci',
    '-DLauncher_BUILD_PLATFORM=Windows-MSVC-x64',
    '-DLauncher_ENABLE_JAVA_DOWNLOADER=ON',
    '-DLauncher_UPDATER_GITHUB_REPO=',
    '-DLauncher_CURSEFORGE_API_KEY=',
    "-DCMAKE_PREFIX_PATH=$qtPrefix",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
)
& cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw 'Windows x64 CMake configuration failed.'
}
