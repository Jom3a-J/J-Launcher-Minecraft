[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int] $PackId,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int] $FileId,

    [Parameter(Mandatory = $true)]
    [string] $CurseForgeApiKey,

    [Parameter(Mandatory = $true)]
    [string] $ServerManagerExecutable,

    [Parameter(Mandatory = $true)]
    [string] $QtBin,

    [Parameter(Mandatory = $true)]
    [string] $VcpkgBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($CurseForgeApiKey)) {
    throw 'The protected CurseForge API key is required.'
}
if (-not (Test-Path -LiteralPath $ServerManagerExecutable -PathType Leaf)) {
    throw 'The ServerManager test executable does not exist.'
}
if (-not (Test-Path -LiteralPath $QtBin -PathType Container)) {
    throw 'The Qt runtime directory does not exist.'
}
if (-not (Test-Path -LiteralPath $VcpkgBin -PathType Container)) {
    throw 'The vcpkg runtime directory does not exist.'
}
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    throw 'RUNNER_TEMP is required so downloaded content stays outside the repository.'
}

$apiOrigin = [Uri] 'https://api.curseforge.com'
$apiBase = $apiOrigin.AbsoluteUri.TrimEnd('/') + '/v1'
$headers = @{
    'Accept' = 'application/json'
    'x-api-key' = $CurseForgeApiKey
}

function Get-CurseForgeData {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if (-not $Path.StartsWith('/')) {
        throw 'CurseForge API paths must start with a slash.'
    }
    $response = Invoke-RestMethod -Uri ($apiBase + $Path) -Headers $headers
    if ($null -eq $response -or $null -eq $response.data) {
        throw "CurseForge returned no data for $Path."
    }
    return $response.data
}

function Invoke-CurseForgeBatch {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('mods', 'mods/files')]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [hashtable] $Body
    )

    $response = Invoke-RestMethod -Uri ($apiBase + '/' + $Path) `
        -Method Post -Headers $headers -ContentType 'application/json' `
        -Body ($Body | ConvertTo-Json -Compress)
    if ($null -eq $response -or $null -eq $response.data) {
        throw "CurseForge returned no batch data for $Path."
    }
    return @($response.data)
}

function Get-SafeRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $normalized = $Path.Replace('\', '/').Trim()
    if ([string]::IsNullOrWhiteSpace($normalized) `
        -or [IO.Path]::IsPathRooted($normalized) `
        -or @($normalized.Split('/') | Where-Object { $_ -eq '..' }).Count -gt 0) {
        throw "Unsafe relative path: $Path"
    }
    return $normalized
}

function Get-VerifiedDestination {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Root,

        [Parameter(Mandatory = $true)]
        [string] $RelativePath
    )

    $safeRelativePath = Get-SafeRelativePath $RelativePath
    $resolvedRoot = [IO.Path]::GetFullPath($Root) + [IO.Path]::DirectorySeparatorChar
    $destination = [IO.Path]::GetFullPath((Join-Path $Root $safeRelativePath))
    if (-not $destination.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped its temporary root: $RelativePath"
    }
    return $destination
}

function Assert-CurseForgeHash {
    param(
        [Parameter(Mandatory = $true)]
        [object] $File,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $hash = @($File.hashes | Where-Object { $_.algo -eq 1 } | Select-Object -First 1)
    $algorithm = 'SHA1'
    if ($hash.Count -eq 0) {
        $hash = @($File.hashes | Where-Object { $_.algo -eq 2 } | Select-Object -First 1)
        $algorithm = 'MD5'
    }
    if ($hash.Count -eq 0) {
        throw "CurseForge supplied no supported SHA-1 or MD5 hash for file $($File.id)."
    }
    $expected = ([string] $hash[0].value).Trim().ToLowerInvariant()
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm $algorithm).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "CurseForge hash verification failed for file $($File.id)."
    }
}

function Get-CurseForgeDownloadUrl {
    param(
        [Parameter(Mandatory = $true)]
        [object] $File
    )

    $value = ([string] $File.downloadUrl).Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        $value = [string] (Get-CurseForgeData "/mods/$($File.modId)/files/$($File.id)/download-url")
    }
    $uri = [Uri] $value
    if (-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https') {
        throw "CurseForge supplied an insecure download URL for file $($File.id)."
    }
    return $uri
}

function Expand-VerifiedArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ArchivePath,

        [Parameter(Mandatory = $true)]
        [string] $Destination
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        if ($archive.Entries.Count -gt 100000) {
            throw 'The CurseForge pack archive contains too many entries.'
        }
        [long] $expandedBytes = 0
        foreach ($entry in $archive.Entries) {
            $relativePath = Get-SafeRelativePath $entry.FullName
            $null = Get-VerifiedDestination -Root $Destination -RelativePath $relativePath
            $expandedBytes += $entry.Length
            if ($expandedBytes -gt 2GB) {
                throw 'The CurseForge pack archive exceeds the 2 GB smoke-test limit.'
            }
        }
    }
    finally {
        $archive.Dispose()
    }
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Destination
}

function Write-MetadataList {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[string]] $Values
    )

    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    [IO.File]::WriteAllLines($Path, [string[]] $Values)
}

$runnerTemp = [IO.Path]::GetFullPath($env:RUNNER_TEMP)
$fixtureRoot = Join-Path $runnerTemp (
    'JLauncher-CurseForge-' + [guid]::NewGuid().ToString('N'))
$instanceRoot = Join-Path $fixtureRoot 'instance'
$gameRoot = Join-Path $instanceRoot 'minecraft'
$flameRoot = Join-Path $instanceRoot 'flame'
$serverPackRoot = Join-Path $instanceRoot 'server-pack'
$archiveRoot = Join-Path $fixtureRoot 'archive'
$packArchive = Join-Path $fixtureRoot 'pack.zip'

try {
    New-Item -ItemType Directory -Path $gameRoot,$flameRoot,$serverPackRoot,$archiveRoot -Force | Out-Null

    $packFile = Get-CurseForgeData "/mods/$PackId/files/$FileId"
    if ([int64] $packFile.id -ne $FileId -or [int64] $packFile.modId -ne $PackId) {
        throw 'CurseForge returned metadata for a different modpack file.'
    }
    if ($null -ne $packFile.serverPackFileId `
        -and [int64] $packFile.serverPackFileId -gt 0) {
        throw 'The selected CurseForge file has a dedicated server pack; choose a fallback-only file.'
    }
    if ([bool] $packFile.isServerPack) {
        throw 'The selected CurseForge file is itself a server pack.'
    }

    $packDownload = Get-CurseForgeDownloadUrl $packFile
    Invoke-WebRequest -Uri $packDownload -OutFile $packArchive
    Assert-CurseForgeHash -File $packFile -Path $packArchive
    Expand-VerifiedArchive -ArchivePath $packArchive -Destination $archiveRoot

    $manifestPath = Join-Path $archiveRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'The CurseForge archive has no root manifest.json.'
    }
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($manifest.manifestType -ne 'minecraftModpack') {
        throw 'The selected CurseForge file is not a Minecraft modpack.'
    }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $flameRoot 'manifest.json')

    $manifestFiles = @($manifest.files | Where-Object { [bool] $_.required })
    $projectIds = @($manifestFiles | ForEach-Object { [int] $_.projectID } | Sort-Object -Unique)
    $fileIds = @($manifestFiles | ForEach-Object { [int] $_.fileID } | Sort-Object -Unique)
    $projects = if ($projectIds.Count -gt 0) {
        Invoke-CurseForgeBatch -Path 'mods' -Body @{ modIds = $projectIds }
    } else { @() }
    $files = if ($fileIds.Count -gt 0) {
        Invoke-CurseForgeBatch -Path 'mods/files' -Body @{ fileIds = $fileIds }
    } else { @() }

    $projectsById = @{}
    foreach ($project in $projects) {
        $projectsById[[string] $project.id] = $project
    }
    $filesById = @{}
    foreach ($file in $files) {
        $filesById[[string] $file.id] = $file
    }

    $clientOnly = [System.Collections.Generic.List[string]]::new()
    $serverOnly = [System.Collections.Generic.List[string]]::new()
    $universal = [System.Collections.Generic.List[string]]::new()
    $unknown = [System.Collections.Generic.List[string]]::new()

    foreach ($entry in $manifestFiles) {
        $projectKey = [string] ([int] $entry.projectID)
        $fileKey = [string] ([int] $entry.fileID)
        if (-not $projectsById.ContainsKey($projectKey) `
            -or -not $filesById.ContainsKey($fileKey)) {
            throw "CurseForge did not return required file metadata for $fileKey."
        }
        $project = $projectsById[$projectKey]
        $file = $filesById[$fileKey]
        if ([int64] $file.modId -ne [int64] $entry.projectID) {
            throw "CurseForge returned a file from the wrong project for $fileKey."
        }

        $folder = switch ([int] $project.classId) {
            6 { 'mods' }
            12 { 'resourcepacks' }
            6552 { 'shaderpacks' }
            6945 { 'datapacks' }
            default { throw "Unsupported CurseForge project class $($project.classId) for project $projectKey." }
        }
        $safeName = [IO.Path]::GetFileName(([string] $file.fileName))
        if ([string]::IsNullOrWhiteSpace($safeName)) {
            throw "CurseForge returned an invalid filename for $fileKey."
        }
        $relativePath = Get-SafeRelativePath ($folder + '/' + $safeName)
        $destination = Get-VerifiedDestination -Root $gameRoot -RelativePath $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Invoke-WebRequest -Uri (Get-CurseForgeDownloadUrl $file) -OutFile $destination
        Assert-CurseForgeHash -File $file -Path $destination

        $gameVersions = @($file.gameVersions | ForEach-Object {
            ([string] $_).Trim().ToLowerInvariant()
        })
        $hasClient = $gameVersions -contains 'client'
        $hasServer = $gameVersions -contains 'server'
        if ($hasClient -and -not $hasServer) {
            $clientOnly.Add($relativePath)
        } elseif ($hasServer -and -not $hasClient) {
            $serverOnly.Add($relativePath)
        } elseif ($hasClient -and $hasServer) {
            $universal.Add($relativePath)
        } else {
            $unknown.Add($relativePath)
        }
    }

    $overrideName = ([string] $manifest.overrides).Trim()
    if (-not [string]::IsNullOrWhiteSpace($overrideName)) {
        $safeOverrideName = Get-SafeRelativePath $overrideName
        $overrideRoot = Get-VerifiedDestination -Root $archiveRoot -RelativePath $safeOverrideName
        if (-not (Test-Path -LiteralPath $overrideRoot -PathType Container)) {
            throw 'The CurseForge manifest declares a missing overrides directory.'
        }
        $overridePaths = [System.Collections.Generic.List[string]]::new()
        foreach ($overrideFile in Get-ChildItem -LiteralPath $overrideRoot -File -Recurse) {
            $relativePath = Get-SafeRelativePath (
                [IO.Path]::GetRelativePath($overrideRoot, $overrideFile.FullName))
            $overridePaths.Add($relativePath)
            $destination = Get-VerifiedDestination -Root $gameRoot -RelativePath $relativePath
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $overrideFile.FullName -Destination $destination
        }
        Write-MetadataList -Path (Join-Path $flameRoot 'overrides.txt') -Values $overridePaths
    }

    [IO.File]::WriteAllText((Join-Path $serverPackRoot 'provider.txt'), "curseforge`n")
    Write-MetadataList -Path (Join-Path $serverPackRoot 'client-only.txt') -Values $clientOnly
    Write-MetadataList -Path (Join-Path $serverPackRoot 'server-only.txt') -Values $serverOnly
    Write-MetadataList -Path (Join-Path $serverPackRoot 'include.txt') -Values $universal
    Write-MetadataList -Path (Join-Path $serverPackRoot 'unknown.txt') -Values $unknown

    $minecraftVersion = ([string] $manifest.minecraft.version).Trim()
    $loaders = @($manifest.minecraft.modLoaders)
    $loader = @($loaders | Where-Object { [bool] $_.primary } | Select-Object -First 1)
    if ($loader.Count -eq 0) {
        $loader = @($loaders | Select-Object -First 1)
    }
    if ($loader.Count -ne 1) {
        throw 'The CurseForge manifest declares no loader.'
    }
    $loaderId = ([string] $loader[0].id).Trim()
    $loaderType = ''
    $loaderVersion = ''
    foreach ($prefix in @('neoforge-', 'forge-', 'fabric-')) {
        if ($loaderId.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $loaderType = $prefix.TrimEnd('-')
            $loaderVersion = $loaderId.Substring($prefix.Length)
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($loaderType) `
        -or [string]::IsNullOrWhiteSpace($loaderVersion)) {
        throw "The CurseForge manifest declares an unsupported loader: $loaderId"
    }
    if ($loaderType -eq 'neoforge' `
        -and $loaderVersion.StartsWith($minecraftVersion + '-')) {
        $loaderVersion = $loaderVersion.Substring($minecraftVersion.Length + 1)
    }

    $env:JLAUNCHER_LIVE_NORMALIZED_INSTANCE = $instanceRoot
    $env:JLAUNCHER_LIVE_SERVER_MINECRAFT = $minecraftVersion
    $env:JLAUNCHER_LIVE_SERVER_FABRIC = if ($loaderType -eq 'fabric') { $loaderVersion } else { '' }
    $env:JLAUNCHER_LIVE_SERVER_FORGE = if ($loaderType -eq 'forge') { $loaderVersion } else { '' }
    $env:JLAUNCHER_LIVE_SERVER_NEOFORGE = if ($loaderType -eq 'neoforge') { $loaderVersion } else { '' }
    $env:JLAUNCHER_LIVE_SERVER_PROVIDER = 'curseforge'
    $env:Path = "$QtBin;$VcpkgBin;$env:Path"

    & $ServerManagerExecutable validatesExternalNormalizedServerProjection
    if ($LASTEXITCODE -ne 0) {
        throw "The CurseForge projection test failed with exit code $LASTEXITCODE."
    }

    Write-Output (
        "CurseForge fallback smoke passed for pack {0}, file {1}: Minecraft {2}, {3} {4}, {5} required files." `
            -f $PackId, $FileId, $minecraftVersion, $loaderType, $loaderVersion, $manifestFiles.Count)
}
finally {
    Remove-Item Env:JLAUNCHER_LIVE_NORMALIZED_INSTANCE,Env:JLAUNCHER_LIVE_SERVER_MINECRAFT, `
        Env:JLAUNCHER_LIVE_SERVER_FABRIC,Env:JLAUNCHER_LIVE_SERVER_FORGE, `
        Env:JLAUNCHER_LIVE_SERVER_NEOFORGE,Env:JLAUNCHER_LIVE_SERVER_PROVIDER `
        -ErrorAction SilentlyContinue
    $CurseForgeApiKey = $null
    $headers['x-api-key'] = ''

    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $allowedPrefix = $runnerTemp.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($resolvedFixture.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase) `
        -and [IO.Path]::GetFileName($resolvedFixture).StartsWith('JLauncher-CurseForge-')) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force -ErrorAction SilentlyContinue
    }
}
