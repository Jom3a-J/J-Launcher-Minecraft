<#
.SYNOPSIS
Creates and verifies an exact tracked-source archive for a Phase 9 release tag.

.DESCRIPTION
The archive contains every tracked file from the root repository and every
initialized submodule at the revisions recorded by an annotated version tag.
It exports bytes directly from the recorded Git objects so Windows line-ending
conversion cannot alter the source, preserves Git symlinks and executable
metadata, writes an external per-file manifest, and never creates, moves, or
deletes a Git tag or release.
#>

[CmdletBinding()]
param(
    [string] $Repository = (Join-Path $PSScriptRoot '..'),

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^jlauncher-\d+\.\d+\.\d+$')]
    [string] $ExpectedTag,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]] $ArgumentList
    )

    $result = @(& git -C $repository @ArgumentList)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($ArgumentList -join ' ') exited with code $LASTEXITCODE."
    }
    return $result
}

function Read-AsciiLine {
    param(
        [Parameter(Mandatory = $true)]
        [IO.Stream] $Stream
    )

    $bytes = [Collections.Generic.List[byte]]::new()
    while ($true) {
        $value = $Stream.ReadByte()
        if ($value -lt 0) {
            throw 'Unexpected end of Git batch output.'
        }
        if ($value -eq 10) {
            break
        }
        $bytes.Add([byte] $value)
    }
    return [Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

function Export-GitIndex {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RepositoryPath,

        [string] $ArchivePrefix,

        [Parameter(Mandatory = $true)]
        [string] $StagingRoot
    )

    $indexLines = @(& git -C $RepositoryPath -c core.quotePath=false ls-files --stage)
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect Git index: $RepositoryPath"
    }

    $entries = [Collections.Generic.List[object]]::new()
    foreach ($line in $indexLines) {
        if ($line -notmatch '^(\d{6}) ([0-9a-f]{40,64}) ([0-3])\t(.+)$') {
            throw "Unexpected Git index entry in $RepositoryPath`: $line"
        }
        $mode = $Matches[1]
        $objectId = $Matches[2]
        $stage = $Matches[3]
        $relativePath = $Matches[4]
        if ($stage -ne '0') {
            throw "Unmerged Git index entry in $RepositoryPath`: $relativePath"
        }
        if ($mode -eq '160000') {
            continue
        }
        if ($mode -notin @('100644', '100755', '120000')) {
            throw "Unsupported tracked-file mode $mode`: $relativePath"
        }
        if ([IO.Path]::IsPathRooted($relativePath) -or
            $relativePath -match '(^|/)\.\.(/|$)') {
            throw "Unsafe tracked-file path: $relativePath"
        }

        $archivePath = if ($ArchivePrefix) {
            "$($ArchivePrefix.TrimEnd('/'))/$relativePath"
        }
        else {
            $relativePath
        }
        $entries.Add([pscustomobject]@{
            Mode = $mode
            ObjectId = $objectId
            RepositoryPath = $RepositoryPath
            RepositoryRelativePath = $relativePath
            ArchivePath = $archivePath
        })
    }
    if ($entries.Count -eq 0) {
        return @()
    }

    $gitPath = (Get-Command git.exe -ErrorAction Stop).Source
    if ($RepositoryPath.Contains('"')) {
        throw "Repository path contains an unsupported quote: $RepositoryPath"
    }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $gitPath
    $startInfo.Arguments = "-C `"$RepositoryPath`" cat-file --batch"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Unable to start git cat-file for $RepositoryPath."
    }

    try {
        $outputStream = $process.StandardOutput.BaseStream
        $buffer = New-Object byte[] 65536
        $stagingPrefix = [IO.Path]::GetFullPath($StagingRoot).TrimEnd('\') + '\'
        foreach ($entry in $entries) {
            $process.StandardInput.WriteLine($entry.ObjectId)
            $process.StandardInput.Flush()

            $header = Read-AsciiLine -Stream $outputStream
            if ($header -notmatch '^([0-9a-f]{40,64}) blob (\d+)$' -or
                $Matches[1] -ne $entry.ObjectId) {
                throw "Unexpected Git blob header for $($entry.ArchivePath): $header"
            }
            [long] $remaining = $Matches[2]

            $destinationPath = [IO.Path]::GetFullPath(
                (Join-Path $StagingRoot $entry.ArchivePath)
            )
            if (-not $destinationPath.StartsWith(
                    $stagingPrefix,
                    [StringComparison]::OrdinalIgnoreCase
                )) {
                throw "Tracked source escaped the staging root: $($entry.ArchivePath)"
            }
            $destinationParent = Split-Path -Parent $destinationPath
            if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
                $null = New-Item -ItemType Directory -Path $destinationParent
            }

            $destinationStream = [IO.File]::Open(
                $destinationPath,
                [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write,
                [IO.FileShare]::None
            )
            try {
                while ($remaining -gt 0) {
                    $requested = [int] [Math]::Min([long] $buffer.Length, $remaining)
                    $read = $outputStream.Read($buffer, 0, $requested)
                    if ($read -le 0) {
                        throw "Git blob ended early: $($entry.ArchivePath)"
                    }
                    $destinationStream.Write($buffer, 0, $read)
                    $remaining -= $read
                }
            }
            finally {
                $destinationStream.Dispose()
            }

            if ($outputStream.ReadByte() -ne 10) {
                throw "Git blob delimiter is missing: $($entry.ArchivePath)"
            }
        }

        $process.StandardInput.Close()
        $process.WaitForExit()
        $standardError = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0) {
            throw "git cat-file failed for $RepositoryPath`: $standardError"
        }
    }
    finally {
        if (-not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        $process.Dispose()
    }

    return @($entries)
}

function Assert-SafeArchiveLink {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ArchivePath,

        [Parameter(Mandatory = $true)]
        [string] $LinkTarget
    )

    if ([string]::IsNullOrWhiteSpace($LinkTarget) -or
        $LinkTarget.StartsWith('/') -or
        $LinkTarget.StartsWith('\') -or
        $LinkTarget -match '^[A-Za-z]:' -or
        $LinkTarget.Contains('\') -or
        $LinkTarget.Contains("`r") -or
        $LinkTarget.Contains("`n")) {
        throw "Unsafe symbolic-link target in source archive: $ArchivePath"
    }

    $components = [Collections.Generic.List[string]]::new()
    $archiveComponents = $ArchivePath.Split('/')
    foreach ($component in $archiveComponents[0..($archiveComponents.Count - 2)]) {
        $components.Add($component)
    }
    foreach ($component in $LinkTarget.Split('/')) {
        if (-not $component -or $component -eq '.') {
            continue
        }
        if ($component -eq '..') {
            if ($components.Count -le 1) {
                throw "Symbolic link escapes the source archive root: $ArchivePath"
            }
            $components.RemoveAt($components.Count - 1)
            continue
        }
        $components.Add($component)
    }
}

function New-ExactSourceArchive {
    param(
        [Parameter(Mandatory = $true)]
        [Collections.Generic.List[object]] $ExportedEntries,

        [Parameter(Mandatory = $true)]
        [string] $RootName,

        [Parameter(Mandatory = $true)]
        [string] $Destination,

        [Parameter(Mandatory = $true)]
        [string] $StagingRoot,

        [Parameter(Mandatory = $true)]
        [DateTimeOffset] $ArchiveTimestamp
    )

    if (-not ('System.Formats.Tar.GnuTarEntry' -as [type]) -or
        -not ('System.Formats.Tar.TarWriter' -as [type])) {
        throw 'This source exporter requires PowerShell on .NET 7 or newer.'
    }

    $destinationStream = [IO.File]::Open(
        $Destination,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None
    )
    try {
        $gzipStream = [IO.Compression.GZipStream]::new(
            $destinationStream,
            [IO.Compression.CompressionLevel]::Optimal,
            $true
        )
        try {
            $writer = [System.Formats.Tar.TarWriter]::new(
                $gzipStream,
                [System.Formats.Tar.TarEntryFormat]::Gnu,
                $true
            )
            try {
                foreach ($sourceEntry in $ExportedEntries) {
                    $archiveName = "$RootName/$($sourceEntry.ArchivePath)"
                    $stagedPath = Join-Path $StagingRoot $sourceEntry.ArchivePath
                    if ($sourceEntry.Mode -eq '120000') {
                        $linkBytes = [IO.File]::ReadAllBytes($stagedPath)
                        $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
                        try {
                            $linkTarget = $strictUtf8.GetString($linkBytes)
                        }
                        catch {
                            throw "Git symbolic-link target is not valid UTF-8: $($sourceEntry.ArchivePath)"
                        }
                        Assert-SafeArchiveLink `
                            -ArchivePath $archiveName `
                            -LinkTarget $linkTarget
                        $archiveEntry = [System.Formats.Tar.GnuTarEntry]::new(
                            [System.Formats.Tar.TarEntryType]::SymbolicLink,
                            $archiveName
                        )
                        $archiveEntry.LinkName = $linkTarget
                        $archiveEntry.Mode = [IO.UnixFileMode] 511
                    }
                    else {
                        $archiveEntry = [System.Formats.Tar.GnuTarEntry]::new(
                            [System.Formats.Tar.TarEntryType]::RegularFile,
                            $archiveName
                        )
                        $archiveEntry.Mode = if ($sourceEntry.Mode -eq '100755') {
                            [IO.UnixFileMode] 493
                        }
                        else {
                            [IO.UnixFileMode] 420
                        }
                        $sourceStream = [IO.File]::OpenRead($stagedPath)
                        try {
                            $archiveEntry.DataStream = $sourceStream
                            $archiveEntry.ModificationTime = $ArchiveTimestamp
                            $archiveEntry.AccessTime = $ArchiveTimestamp
                            $archiveEntry.ChangeTime = $ArchiveTimestamp
                            $archiveEntry.Uid = 0
                            $archiveEntry.Gid = 0
                            $archiveEntry.UserName = 'root'
                            $archiveEntry.GroupName = 'root'
                            $writer.WriteEntry($archiveEntry)
                        }
                        finally {
                            $sourceStream.Dispose()
                        }
                        continue
                    }

                    $archiveEntry.ModificationTime = $ArchiveTimestamp
                    $archiveEntry.AccessTime = $ArchiveTimestamp
                    $archiveEntry.ChangeTime = $ArchiveTimestamp
                    $archiveEntry.Uid = 0
                    $archiveEntry.Gid = 0
                    $archiveEntry.UserName = 'root'
                    $archiveEntry.GroupName = 'root'
                    $writer.WriteEntry($archiveEntry)
                }
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $gzipStream.Dispose()
        }
    }
    finally {
        $destinationStream.Dispose()
    }
}

function Test-ExactSourceArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ArchivePath,

        [Parameter(Mandatory = $true)]
        [string] $RootName,

        [Parameter(Mandatory = $true)]
        [Collections.Generic.List[object]] $ExportedEntries,

        [Parameter(Mandatory = $true)]
        [Collections.Generic.Dictionary[string, string]] $ExpectedHashes
    )

    $expectedEntries = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::Ordinal
    )
    foreach ($entry in $ExportedEntries) {
        $archiveName = "$RootName/$($entry.ArchivePath)"
        if (-not $expectedEntries.TryAdd($archiveName, $entry)) {
            throw "Duplicate exported source path: $archiveName"
        }
    }

    $seenEntries = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $archiveStream = [IO.File]::OpenRead($ArchivePath)
    try {
        $gzipStream = [IO.Compression.GZipStream]::new(
            $archiveStream,
            [IO.Compression.CompressionMode]::Decompress,
            $true
        )
        try {
            $reader = [System.Formats.Tar.TarReader]::new($gzipStream, $false)
            try {
                while (($archiveEntry = $reader.GetNextEntry($false)) -ne $null) {
                    if ($archiveEntry.EntryType -eq [System.Formats.Tar.TarEntryType]::Directory) {
                        continue
                    }

                    $expectedEntry = $null
                    if (-not $expectedEntries.TryGetValue($archiveEntry.Name, [ref] $expectedEntry)) {
                        throw "Source archive contains an untracked entry: $($archiveEntry.Name)"
                    }
                    if (-not $seenEntries.Add($archiveEntry.Name)) {
                        throw "Source archive contains a duplicate entry: $($archiveEntry.Name)"
                    }

                    if ($expectedEntry.Mode -eq '120000') {
                        if ($archiveEntry.EntryType -ne
                            [System.Formats.Tar.TarEntryType]::SymbolicLink) {
                            throw "Git symbolic link became a regular archive file: $($archiveEntry.Name)"
                        }
                        Assert-SafeArchiveLink `
                            -ArchivePath $archiveEntry.Name `
                            -LinkTarget $archiveEntry.LinkName
                        $contentBytes = [Text.Encoding]::UTF8.GetBytes($archiveEntry.LinkName)
                        $actualHash = [Convert]::ToHexString(
                            [Security.Cryptography.SHA256]::HashData($contentBytes)
                        )
                    }
                    else {
                        if ($archiveEntry.EntryType -notin @(
                                [System.Formats.Tar.TarEntryType]::RegularFile,
                                [System.Formats.Tar.TarEntryType]::V7RegularFile
                            )) {
                            throw "Tracked file has the wrong archive type: $($archiveEntry.Name)"
                        }
                        $executeMask = [int](
                            [IO.UnixFileMode]::UserExecute -bor
                            [IO.UnixFileMode]::GroupExecute -bor
                            [IO.UnixFileMode]::OtherExecute
                        )
                        $archiveExecuteBits = ([int] $archiveEntry.Mode) -band $executeMask
                        if ($expectedEntry.Mode -eq '100755' -and
                            $archiveExecuteBits -ne $executeMask) {
                            throw "Executable Git file lost its archive mode: $($archiveEntry.Name)"
                        }
                        if ($expectedEntry.Mode -eq '100644' -and
                            $archiveExecuteBits -ne 0) {
                            throw "Non-executable Git file gained execute bits: $($archiveEntry.Name)"
                        }
                        $actualHash = [Convert]::ToHexString(
                            [Security.Cryptography.SHA256]::HashData($archiveEntry.DataStream)
                        )
                    }

                    $relativePath = $archiveEntry.Name.Substring($RootName.Length + 1)
                    $expectedHash = $null
                    if (-not $ExpectedHashes.TryGetValue($relativePath, [ref] $expectedHash) -or
                        $actualHash -ne $expectedHash) {
                        throw "Archived tracked source hash mismatch: $relativePath"
                    }
                }
            }
            finally {
                $reader.Dispose()
            }
        }
        finally {
            $gzipStream.Dispose()
        }
    }
    finally {
        $archiveStream.Dispose()
    }

    if ($seenEntries.Count -ne $expectedEntries.Count) {
        throw "Source archive contains $($seenEntries.Count) tracked entries; expected $($expectedEntries.Count)."
    }
}

$repository = (Resolve-Path -LiteralPath $Repository).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $output -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $output
}

$head = (@(Invoke-Git -ArgumentList @('rev-parse', 'HEAD')))[-1].Trim()
$commitTimestampText = (@(Invoke-Git -ArgumentList @(
    'show', '-s', '--format=%cI', $head
)))[-1].Trim()
$archiveTimestamp = [DateTimeOffset]::Parse(
    $commitTimestampText,
    [Globalization.CultureInfo]::InvariantCulture,
    [Globalization.DateTimeStyles]::RoundtripKind
)
$tagReference = "refs/tags/$ExpectedTag"
& git -C $repository show-ref --verify --quiet $tagReference
if ($LASTEXITCODE -ne 0) {
    throw "Expected tag does not exist: $ExpectedTag"
}
$tagType = (@(Invoke-Git -ArgumentList @('cat-file', '-t', $tagReference)))[-1].Trim()
if ($tagType -ne 'tag') {
    throw "Expected tag is not annotated: $ExpectedTag"
}

$tagCommit = (@(Invoke-Git -ArgumentList @('rev-list', '-n', '1', $tagReference)))[-1].Trim()
if ($tagCommit -ne $head) {
    throw "Tag $ExpectedTag resolves to $tagCommit, not checked-out commit $head."
}

$statusLines = @(Invoke-Git -ArgumentList @(
    'status', '--short', '--untracked-files=all', '--ignore-submodules=none'
))
if ($statusLines.Count -gt 0) {
    throw 'The source worktree is dirty; refusing to create an exact-source archive.'
}

$submoduleStatus = @(Invoke-Git -ArgumentList @('submodule', 'status', '--recursive'))
$invalidSubmodules = @($submoduleStatus | Where-Object { $_ -notmatch '^ ' })
if ($invalidSubmodules.Count -gt 0) {
    throw "A submodule is uninitialized or does not match the tag: $($invalidSubmodules -join '; ')"
}

$dirtySubmodules = @(& git -C $repository submodule foreach --quiet --recursive `
    'if test -n "$(git status --porcelain --untracked-files=all)"; then echo "$displaypath"; fi')
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect submodule worktrees.'
}
if ($dirtySubmodules.Count -gt 0) {
    throw "A submodule worktree is dirty: $($dirtySubmodules -join ', ')"
}

$trackedPaths = @(Invoke-Git -ArgumentList @(
    '-c', 'core.quotePath=false', 'ls-files', '--recurse-submodules'
))
if ($trackedPaths.Count -eq 0) {
    throw 'The tracked-source file list is empty.'
}
if (@($trackedPaths | Group-Object | Where-Object Count -gt 1).Count -gt 0) {
    throw 'The recursive tracked-source file list contains duplicate paths.'
}

$version = $ExpectedTag -replace '^jlauncher-', ''
$rootName = "JLauncher-$version-Source"
$archiveName = "$rootName.tar.gz"
$archivePath = Join-Path $output $archiveName
$archiveChecksumPath = "$archivePath.sha256"
$fileManifestPath = Join-Path $output "$rootName-Files.sha256"
$provenancePath = Join-Path $output "$rootName-Provenance.json"

foreach ($path in @($archivePath, $archiveChecksumPath, $fileManifestPath, $provenancePath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to replace an existing source artifact: $path"
    }
}

$stagingParent = Join-Path $output ('.source-staging-' + [Guid]::NewGuid().ToString('N'))
$stagingRoot = Join-Path $stagingParent $rootName
$outputPrefix = $output.TrimEnd('\') + '\'
$stagingFullPath = [IO.Path]::GetFullPath($stagingParent)
if (-not $stagingFullPath.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Source staging directory escaped the requested output directory.'
}

$null = New-Item -ItemType Directory -Path $stagingRoot
try {
    $repositories = [Collections.Generic.List[object]]::new()
    $repositories.Add([pscustomobject]@{
        RepositoryPath = $repository
        ArchivePrefix = ''
    })
    foreach ($line in $submoduleStatus) {
        if ($line -notmatch '^ [0-9a-f]{40,64} (.+?)(?: \(.+\))?$') {
            throw "Unexpected submodule status line: $line"
        }
        $submodulePath = $Matches[1]
        $repositories.Add([pscustomobject]@{
            RepositoryPath = Join-Path $repository $submodulePath
            ArchivePrefix = $submodulePath.Replace('\', '/')
        })
    }

    $exportedEntries = [Collections.Generic.List[object]]::new()
    foreach ($sourceRepository in $repositories) {
        $entries = @(Export-GitIndex `
            -RepositoryPath $sourceRepository.RepositoryPath `
            -ArchivePrefix $sourceRepository.ArchivePrefix `
            -StagingRoot $stagingRoot)
        foreach ($entry in $entries) {
            $exportedEntries.Add($entry)
        }
    }

    $exportedPaths = @($exportedEntries | Select-Object -ExpandProperty ArchivePath)
    $exportDifference = @(Compare-Object -ReferenceObject $trackedPaths `
        -DifferenceObject $exportedPaths -CaseSensitive)
    if ($exportDifference.Count -gt 0) {
        throw 'Exported Git blobs do not exactly match the recursive tracked-file list.'
    }

    $manifestLines = [Collections.Generic.List[string]]::new()
    $expectedHashes = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::Ordinal
    )
    foreach ($relativePath in $trackedPaths) {
        $destinationPath = Join-Path $stagingRoot $relativePath
        if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
            throw "Exported tracked source file is missing: $relativePath"
        }
        $sourceHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
        $manifestPath = $relativePath.Replace('\', '/')
        $manifestLines.Add("$sourceHash *$manifestPath")
        if (-not $expectedHashes.TryAdd($manifestPath, $sourceHash)) {
            throw "Duplicate source manifest path: $manifestPath"
        }
    }

    $stagedFiles = @(Get-ChildItem -LiteralPath $stagingRoot -Recurse -File)
    if ($stagedFiles.Count -ne $trackedPaths.Count) {
        throw "Staged source count $($stagedFiles.Count) does not match tracked count $($trackedPaths.Count)."
    }

    $manifestLines | Set-Content -LiteralPath $fileManifestPath -Encoding ascii
    $fileManifestHash = (Get-FileHash -LiteralPath $fileManifestPath `
        -Algorithm SHA256).Hash

    New-ExactSourceArchive `
        -ExportedEntries $exportedEntries `
        -RootName $rootName `
        -Destination $archivePath `
        -StagingRoot $stagingRoot `
        -ArchiveTimestamp $archiveTimestamp
    Test-ExactSourceArchive `
        -ArchivePath $archivePath `
        -RootName $rootName `
        -ExportedEntries $exportedEntries `
        -ExpectedHashes $expectedHashes

    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    "$archiveHash *$archiveName" |
        Set-Content -LiteralPath $archiveChecksumPath -Encoding ascii

    $provenance = [ordered]@{
        generatedAt = (Get-Date).ToUniversalTime().ToString('o')
        repository = $env:GITHUB_REPOSITORY
        tag = $ExpectedTag
        productVersion = $version
        tagType = $tagType
        sourceCommit = $head
        sourceFileCount = $trackedPaths.Count
        sourceBytesExportedFromGitObjects = $true
        sourceArchivePreservesGitLinksAndExecutability = $true
        sourceArchiveDeterministic = $true
        sourceArchive = $archiveName
        sourceArchiveSha256 = $archiveHash
        sourceFileManifest = Split-Path -Leaf $fileManifestPath
        sourceFileManifestSha256 = $fileManifestHash
        submodules = @($submoduleStatus)
        workflowRun = if ($env:GITHUB_ACTIONS -eq 'true') {
            "$($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)"
        }
        else {
            $null
        }
    }
    $provenance | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $provenancePath -Encoding utf8

    $provenance | ConvertTo-Json -Depth 5
}
finally {
    if (Test-Path -LiteralPath $stagingParent -PathType Container) {
        $validatedStagingPath = [IO.Path]::GetFullPath($stagingParent)
        if ($validatedStagingPath.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) -and
            $validatedStagingPath -ne $output) {
            Remove-Item -LiteralPath $validatedStagingPath -Recurse -Force
        }
    }
}
