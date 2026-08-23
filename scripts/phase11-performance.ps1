[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [string[]] $DependencyPath = @(),

    [ValidateRange(1, 50)]
    [int] $Runs = 7,

    [ValidateRange(0, 10)]
    [int] $WarmupRuns = 1,

    [ValidateRange(1, 30)]
    [int] $IdleSampleSeconds = 3,

    [ValidateRange(5, 60)]
    [int] $TimeoutSeconds = 20,

    [ValidateSet('Empty', 'Populated')]
    [string] $ProfileScenario = 'Empty',

    [ValidateRange(1, 100)]
    [int] $InstanceCount = 24,

    [ValidateRange(1, 50)]
    [int] $ServerCount = 12,

    [string] $ProfileRoot,

    [string] $JsonOutput,

    [string] $ScreenshotDirectory,

    [switch] $EnforceBudgets,

    [ValidateRange(1, 10000)]
    [double] $MaximumSetupWindowMilliseconds = 750,

    [ValidateRange(1, 10000)]
    [double] $MaximumMainWindowMilliseconds = 1500,

    [ValidateRange(1, 10000)]
    [double] $MaximumSettingsWindowMilliseconds = 500,

    [ValidateRange(1, 10000)]
    [double] $MaximumServersWindowMilliseconds = 500,

    [ValidateRange(0, 100)]
    [double] $MaximumIdleCpuPercent = 0.5,

    [ValidateRange(1, 4096)]
    [double] $MaximumPrivateMemoryMiB = 160,

    [ValidateRange(1, 10000)]
    [double] $MaximumServerTabMilliseconds = 500
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

if (-not ('JLauncherPhase11.WindowProbe' -as [type])) {
    $windowProbeSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

namespace JLauncherPhase11
{
    public sealed class WindowInfo
    {
        public long Handle { get; set; }
        public string Title { get; set; }
    }

    public static class WindowProbe
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct Rect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        private delegate bool EnumWindowsProc(IntPtr window, IntPtr state);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr state);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr window);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr window, StringBuilder text, int maximumCount);

        [DllImport("user32.dll")]
        private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr window, out Rect rectangle);

        [DllImport("user32.dll")]
        private static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);

        public static WindowInfo[] VisibleWindows(int processId)
        {
            var windows = new List<WindowInfo>();
            EnumWindows((window, state) =>
            {
                uint ownerProcessId;
                GetWindowThreadProcessId(window, out ownerProcessId);
                if (ownerProcessId == processId && IsWindowVisible(window))
                {
                    var title = new StringBuilder(512);
                    GetWindowText(window, title, title.Capacity);
                    windows.Add(new WindowInfo { Handle = window.ToInt64(), Title = title.ToString() });
                }
                return true;
            }, IntPtr.Zero);
            return windows.ToArray();
        }

        public static void Close(long handle)
        {
            const uint WM_CLOSE = 0x0010;
            PostMessage(new IntPtr(handle), WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }

        public static void Capture(long handle, string path)
        {
            Rect rectangle;
            var window = new IntPtr(handle);
            if (!GetWindowRect(window, out rectangle))
                throw new InvalidOperationException("Unable to read the benchmark window bounds.");

            var width = rectangle.Right - rectangle.Left;
            var height = rectangle.Bottom - rectangle.Top;
            if (width <= 0 || height <= 0)
                throw new InvalidOperationException("The benchmark window has invalid bounds.");

            using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
            using (var graphics = Graphics.FromImage(bitmap))
            {
                var deviceContext = graphics.GetHdc();
                try
                {
                    const uint PW_RENDERFULLCONTENT = 2;
                    if (!PrintWindow(window, deviceContext, PW_RENDERFULLCONTENT))
                        throw new InvalidOperationException("Unable to render the benchmark window.");
                }
                finally
                {
                    graphics.ReleaseHdc(deviceContext);
                }
                bitmap.Save(path, ImageFormat.Png);
            }
        }
    }
}
'@
    $powerShellRuntime = [IO.Path]::GetDirectoryName([object].Assembly.Location)
    $referenceDirectory = Join-Path $powerShellRuntime 'ref'
    if (Test-Path -LiteralPath $referenceDirectory -PathType Container) {
        $references = @(
            (Get-ChildItem -LiteralPath $referenceDirectory -Filter '*.dll').FullName
            [System.Drawing.Bitmap].Assembly.Location
            (Join-Path $powerShellRuntime 'System.Private.Windows.Core.dll')
            (Join-Path $powerShellRuntime 'System.Private.Windows.GdiPlus.dll')
        )
        Add-Type -TypeDefinition $windowProbeSource -ReferencedAssemblies $references
    }
    else {
        Add-Type -TypeDefinition $windowProbeSource -ReferencedAssemblies System.Drawing
    }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedProfileRoot = if ([string]::IsNullOrWhiteSpace($ProfileRoot)) {
    Join-Path ([IO.Path]::GetDirectoryName($resolvedExecutable)) 'phase11-profiles'
}
else {
    [IO.Path]::GetFullPath($ProfileRoot)
}
[IO.Directory]::CreateDirectory($resolvedProfileRoot) | Out-Null
$resolvedScreenshotDirectory = if ([string]::IsNullOrWhiteSpace($ScreenshotDirectory)) {
    $null
}
else {
    [IO.Path]::GetFullPath($ScreenshotDirectory)
}
if ($null -ne $resolvedScreenshotDirectory) {
    [IO.Directory]::CreateDirectory($resolvedScreenshotDirectory) | Out-Null
}
if ($DependencyPath.Count -gt 0) {
    $resolvedDependencyPaths = $DependencyPath | ForEach-Object { (Resolve-Path -LiteralPath $_).Path }
    $env:PATH = ((@($resolvedDependencyPaths) + @($env:PATH)) -join [IO.Path]::PathSeparator)
}

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock] $Condition,

        [Parameter(Mandatory = $true)]
        [string] $FailureMessage
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)

    throw $FailureMessage
}

function Get-Descendants {
    param([Parameter(Mandatory = $true)] [IntPtr] $WindowHandle)

    $root = [System.Windows.Automation.AutomationElement]::FromHandle($WindowHandle)
    return $root.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
}

function Find-Button {
    param(
        [Parameter(Mandatory = $true)]
        [object[]] $Elements,

        [Parameter(Mandatory = $true)]
        [scriptblock] $NameMatches
    )

    foreach ($element in $Elements) {
        if ($element.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button -and
            (& $NameMatches $element.Current.Name)) {
            return $element
        }
    }
    return $null
}

function Invoke-Button {
    param([Parameter(Mandatory = $true)] [System.Windows.Automation.AutomationElement] $Button)

    $pattern = [System.Windows.Automation.InvokePattern] $Button.GetCurrentPattern(
        [System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}

function Write-BenchmarkFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $Content
    )

    $parent = [IO.Path]::GetDirectoryName($Path)
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Content)
}

function Add-PopulatedProfileData {
    param([Parameter(Mandatory = $true)] [string] $ProfileDirectory)

    $instanceRoot = Join-Path $ProfileDirectory 'instances'
    for ($instanceIndex = 1; $instanceIndex -le $InstanceCount; $instanceIndex++) {
        $instanceId = 'benchmark-instance-{0:d2}' -f $instanceIndex
        $instanceDirectory = Join-Path $instanceRoot $instanceId
        $instanceConfiguration = @(
            'InstanceType=OneSix'
            ('name=Benchmark Instance {0:d2}' -f $instanceIndex)
            'iconKey=default'
        ) -join "`r`n"
        Write-BenchmarkFile -Path (Join-Path $instanceDirectory 'instance.cfg') -Content ($instanceConfiguration + "`r`n")
        $pack = [ordered]@{
            formatVersion = 1
            components = @(
                [ordered]@{
                    uid = 'net.minecraft'
                    version = '1.21.8'
                    important = $true
                }
            )
        }
        Write-BenchmarkFile -Path (Join-Path $instanceDirectory 'mmc-pack.json') -Content ($pack | ConvertTo-Json -Depth 5)
    }

    $loaderTypes = @('vanilla', 'fabric', 'paper', 'purpur', 'forge', 'neoforge')
    $servers = [Collections.Generic.List[object]]::new()
    for ($serverIndex = 1; $serverIndex -le $ServerCount; $serverIndex++) {
        $serverId = 'benchmark-server-{0:d2}' -f $serverIndex
        $loaderType = $loaderTypes[($serverIndex - 1) % $loaderTypes.Count]
        $serverDirectory = Join-Path (Join-Path $ProfileDirectory 'servers') $serverId
        $port = 25564 + $serverIndex
        $servers.Add([ordered]@{
            id = $serverId
            name = 'Benchmark Server {0:d2}' -f $serverIndex
            version = '1.21.8'
            loaderType = $loaderType
            loaderVersion = if ($loaderType -eq 'vanilla') { '' } else { 'benchmark' }
            port = $port
            maxMemory = 4096
            minMemory = 1024
            javaPath = ''
            extraJvmArguments = '-XX:+UseG1GC'
            autoRestartOnCrash = $false
            eulaAccepted = $true
            gracefulStopTimeoutSeconds = 10
            serverDirectory = $serverDirectory
        })

        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'server.properties') -Content "motd=Benchmark Server $serverIndex`r`nserver-port=$port`r`nmax-players=20`r`n"
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'eula.txt') -Content "eula=true`r`n"
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'world\level.dat') -Content 'benchmark world'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'world\playerdata\benchmark.dat') -Content 'benchmark player'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'world_nether\DIM-1\region\r.0.0.mca') -Content 'benchmark nether region'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'config\server.toml') -Content 'benchmark=true'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'logs\latest.log') -Content "Benchmark server log`r`n"
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'whitelist.json') -Content '[]'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'ops.json') -Content '[]'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'banned-players.json') -Content '[]'
        Write-BenchmarkFile -Path (Join-Path $serverDirectory 'banned-ips.json') -Content '[]'

        $contentDirectoryName = if ($loaderType -in @('paper', 'purpur')) { 'plugins' } else { 'mods' }
        for ($contentIndex = 1; $contentIndex -le 24; $contentIndex++) {
            $contentName = 'benchmark-content-{0:d2}.jar' -f $contentIndex
            Write-BenchmarkFile -Path (Join-Path $serverDirectory "$contentDirectoryName\$contentName") -Content 'benchmark content'
        }

        for ($backupIndex = 1; $backupIndex -le 3; $backupIndex++) {
            $backupDirectory = Join-Path $serverDirectory "backups\server-benchmark-$backupIndex"
            $manifest = [ordered]@{
                format = 'JLauncherServerBackup'
                formatVersion = 1
                name = "Benchmark backup $backupIndex"
                serverId = $serverId
                serverName = 'Benchmark Server {0:d2}' -f $serverIndex
                minecraftVersion = '1.21.8'
                loaderType = $loaderType
                loaderVersion = if ($loaderType -eq 'vanilla') { '' } else { 'benchmark' }
                createdUtc = [DateTime]::UtcNow.AddDays(-$backupIndex).ToString('o')
                includedCategories = @('world-data', 'configuration')
            }
            Write-BenchmarkFile -Path (Join-Path $backupDirectory '.jlauncher-backup.json') -Content ($manifest | ConvertTo-Json -Depth 5)
            Write-BenchmarkFile -Path (Join-Path $backupDirectory 'world\level.dat') -Content 'benchmark backup world'
        }
    }

    $registry = [ordered]@{
        version = 1
        servers = $servers
    }
    Write-BenchmarkFile -Path (Join-Path $ProfileDirectory 'servers.json') -Content ($registry | ConvertTo-Json -Depth 6)
}

function New-BenchmarkProfile {
    $directory = Join-Path $resolvedProfileRoot ('jlauncher-phase11-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory | Out-Null

    $configuration = @(
        'Language=en_US'
        'ApplicationTheme=system'
        'IconTheme=pe_blue'
        'IgnoreJavaWizard=true'
        'AutomaticJavaDownload=true'
        'AutomaticJavaSwitch=true'
        'UserAskedAboutAutomaticJavaDownload=true'
        'InstanceDir=instances'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $directory 'jlauncher.cfg'), $configuration + "`r`n")
    if ($ProfileScenario -eq 'Populated') {
        Add-PopulatedProfileData -ProfileDirectory $directory
    }
    return $directory
}

function Open-NewWindow {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [System.Windows.Automation.AutomationElement] $Button,

        [Parameter(Mandatory = $true)]
        [string] $SurfaceName
    )

    $existingHandles = @([JLauncherPhase11.WindowProbe]::VisibleWindows($Process.Id).Handle)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Button -Button $Button

    $newWindow = $null
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $newWindow = [JLauncherPhase11.WindowProbe]::VisibleWindows($Process.Id) |
            Where-Object { $_.Handle -notin $existingHandles } |
            Select-Object -First 1
        if ($null -ne $newWindow) {
            break
        }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($null -eq $newWindow) {
        throw "$SurfaceName did not create a visible native window."
    }
    Write-Verbose "$SurfaceName window: handle=$($newWindow.Handle), title='$($newWindow.Title)'"

    return [pscustomobject]@{
        Milliseconds = $timer.Elapsed.TotalMilliseconds
        Window = $newWindow
    }
}

function Close-BenchmarkWindow {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [long] $WindowHandle,

        [Parameter(Mandatory = $true)]
        [string] $SurfaceName
    )

    [JLauncherPhase11.WindowProbe]::Close($WindowHandle)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (-not ([JLauncherPhase11.WindowProbe]::VisibleWindows($Process.Id).Handle -contains $WindowHandle)) {
            return
        }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)
    $visibleWindows = [JLauncherPhase11.WindowProbe]::VisibleWindows($Process.Id) |
        ForEach-Object { "$($_.Handle):$($_.Title)" }
    throw "$SurfaceName did not close after the benchmark. Visible windows: $($visibleWindows -join ', ')"
}

function Measure-NewWindow {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process,

        [Parameter(Mandatory = $true)]
        [System.Windows.Automation.AutomationElement] $Button,

        [Parameter(Mandatory = $true)]
        [string] $SurfaceName
    )

    $measurement = Open-NewWindow -Process $Process -Button $Button -SurfaceName $SurfaceName
    Close-BenchmarkWindow -Process $Process -WindowHandle $measurement.Window.Handle -SurfaceName $SurfaceName
    return $measurement.Milliseconds
}

function Measure-ServerDestination {
    param(
        [Parameter(Mandatory = $true)]
        [object[]] $Elements,

        [Parameter(Mandatory = $true)]
        [string] $DestinationName
    )

    $destination = $null
    foreach ($element in $Elements) {
        if ($element.Current.ControlType -eq [System.Windows.Automation.ControlType]::ListItem -and
            $element.Current.Name -eq $DestinationName) {
            $destination = $element
            break
        }
    }
    if ($null -eq $destination) {
        throw "The populated Servers window did not expose its $DestinationName destination."
    }

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $selectionPattern = [System.Windows.Automation.SelectionItemPattern] $destination.GetCurrentPattern(
        [System.Windows.Automation.SelectionItemPattern]::Pattern)
    $selectionPattern.Select()
    return $timer.Elapsed.TotalMilliseconds
}

$results = [Collections.Generic.List[object]]::new()
$totalRuns = $WarmupRuns + $Runs

for ($run = 1; $run -le $totalRuns; $run++) {
    $profileDirectory = New-BenchmarkProfile
    $captureThisRun = $null -ne $resolvedScreenshotDirectory -and $run -eq ($WarmupRuns + 1)
    $process = $null
    try {
        $startupTimer = [Diagnostics.Stopwatch]::StartNew()
        $quotedProfileDirectory = '"{0}"' -f $profileDirectory
        $process = Start-Process -FilePath $resolvedExecutable -ArgumentList @('--dir', $quotedProfileDirectory) -PassThru

        Wait-Until -FailureMessage 'J Launcher did not show its setup window.' -Condition {
            $process.Refresh()
            return $process.HasExited -or $process.MainWindowHandle -ne 0
        }
        if ($process.HasExited) {
            throw "J Launcher exited with code $($process.ExitCode) before showing a window."
        }

        $setupWindowMilliseconds = $startupTimer.Elapsed.TotalMilliseconds
        $setupHandle = $process.MainWindowHandle
        $setupElements = Get-Descendants -WindowHandle $setupHandle
        $finishButton = Find-Button -Elements $setupElements -NameMatches {
            param($name) $name -eq 'Finish'
        }
        if ($null -eq $finishButton) {
            $setupNames = foreach ($element in $setupElements) {
                if (-not [string]::IsNullOrWhiteSpace($element.Current.Name)) {
                    "$($element.Current.ControlType.ProgrammaticName):$($element.Current.Name)"
                }
            }
            throw "The isolated setup window '$($process.MainWindowTitle)' did not expose its Finish button. Controls: $($setupNames -join ' | ')"
        }
        Invoke-Button -Button $finishButton

        Wait-Until -FailureMessage 'J Launcher did not replace setup with its main window.' -Condition {
            $process.Refresh()
            return $process.HasExited -or ($process.MainWindowHandle -ne 0 -and $process.MainWindowHandle -ne $setupHandle)
        }
        if ($process.HasExited) {
            throw "J Launcher exited with code $($process.ExitCode) before showing its main window."
        }

        $mainWindowMilliseconds = $startupTimer.Elapsed.TotalMilliseconds
        Start-Sleep -Milliseconds 100
        foreach ($startupWindow in [JLauncherPhase11.WindowProbe]::VisibleWindows($process.Id)) {
            if ($startupWindow.Handle -ne $process.MainWindowHandle) {
                $startupElements = Get-Descendants -WindowHandle ([IntPtr] $startupWindow.Handle)
                $startupNames = foreach ($element in $startupElements) {
                    if (-not [string]::IsNullOrWhiteSpace($element.Current.Name)) {
                        "$($element.Current.ControlType.ProgrammaticName):$($element.Current.Name)"
                    }
                }
                Write-Verbose "Additional startup window '$($startupWindow.Title)': $($startupNames -join ' | ')"
            }
        }
        $mainElements = Get-Descendants -WindowHandle $process.MainWindowHandle
        if ($captureThisRun) {
            [JLauncherPhase11.WindowProbe]::Capture(
                $process.MainWindowHandle,
                (Join-Path $resolvedScreenshotDirectory "phase11-$($ProfileScenario.ToLowerInvariant())-main.png"))
        }
        $settingsButton = Find-Button -Elements $mainElements -NameMatches {
            param($name) $name -eq 'Settings'
        }
        $serversButton = Find-Button -Elements $mainElements -NameMatches {
            param($name) $name.StartsWith('Servers', [StringComparison]::Ordinal)
        }
        if ($null -eq $settingsButton -or $null -eq $serversButton) {
            throw 'The main window did not expose both Settings and Servers buttons.'
        }

        $settingsMeasurement = Open-NewWindow -Process $process -Button $settingsButton -SurfaceName 'Settings'
        $settingsMilliseconds = $settingsMeasurement.Milliseconds
        if ($captureThisRun) {
            Start-Sleep -Milliseconds 100
            [JLauncherPhase11.WindowProbe]::Capture(
                $settingsMeasurement.Window.Handle,
                (Join-Path $resolvedScreenshotDirectory "phase11-$($ProfileScenario.ToLowerInvariant())-settings.png"))
        }
        Close-BenchmarkWindow -Process $process -WindowHandle $settingsMeasurement.Window.Handle -SurfaceName 'Settings'
        $serversMeasurement = Open-NewWindow -Process $process -Button $serversButton -SurfaceName 'Servers'
        $serversMilliseconds = $serversMeasurement.Milliseconds

        if ($captureThisRun) {
            Start-Sleep -Milliseconds 100
            [JLauncherPhase11.WindowProbe]::Capture(
                $serversMeasurement.Window.Handle,
                (Join-Path $resolvedScreenshotDirectory "phase11-$($ProfileScenario.ToLowerInvariant())-servers.png"))
        }

        $serverDestinationMeasurements = [ordered]@{}
        if ($ProfileScenario -eq 'Populated') {
            $serverElements = Get-Descendants -WindowHandle ([IntPtr] $serversMeasurement.Window.Handle)
            $selectedBenchmarkServer = $serverElements | Where-Object {
                $_.Current.Name -eq 'Benchmark Server 01'
            } | Select-Object -First 1
            if ($null -eq $selectedBenchmarkServer) {
                throw 'The populated fixture did not select Benchmark Server 01 in the Servers window.'
            }
            foreach ($destinationName in @('Console', 'Mods', 'Players', 'Files', 'Backups', 'Tools', 'Settings', 'Home')) {
                $serverDestinationMeasurements[$destinationName] = Measure-ServerDestination -Elements $serverElements -DestinationName $destinationName
            }
        }
        Close-BenchmarkWindow -Process $process -WindowHandle $serversMeasurement.Window.Handle -SurfaceName 'Servers'

        Start-Sleep -Seconds $IdleSampleSeconds
        $process.Refresh()
        $cpuStart = $process.TotalProcessorTime
        $cpuTimer = [Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Seconds $IdleSampleSeconds
        $process.Refresh()
        $cpuPercent = (($process.TotalProcessorTime - $cpuStart).TotalMilliseconds / $cpuTimer.Elapsed.TotalMilliseconds * 100) /
            [Environment]::ProcessorCount

        $result = [ordered]@{
            Run = $run
            Warmup = $run -le $WarmupRuns
            SetupWindowMilliseconds = [Math]::Round($setupWindowMilliseconds, 1)
            MainWindowMilliseconds = [Math]::Round($mainWindowMilliseconds, 1)
            SettingsWindowMilliseconds = [Math]::Round($settingsMilliseconds, 1)
            ServersWindowMilliseconds = [Math]::Round($serversMilliseconds, 1)
            IdleCpuPercent = [Math]::Round($cpuPercent, 3)
            PrivateMemoryMiB = [Math]::Round($process.PrivateMemorySize64 / 1MB, 1)
            WorkingSetMiB = [Math]::Round($process.WorkingSet64 / 1MB, 1)
            HandleCount = $process.HandleCount
            ThreadCount = $process.Threads.Count
        }
        foreach ($destination in $serverDestinationMeasurements.GetEnumerator()) {
            $result["Server$($destination.Key)Milliseconds"] = [Math]::Round($destination.Value, 1)
        }
        $results.Add([pscustomobject] $result)
    }
    finally {
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
        }
        if (Test-Path -LiteralPath $profileDirectory) {
            Remove-Item -LiteralPath $profileDirectory -Recurse -Force
        }
    }
}

if ([IO.Directory]::Exists($resolvedProfileRoot) -and
    -not [IO.Directory]::EnumerateFileSystemEntries($resolvedProfileRoot).GetEnumerator().MoveNext()) {
    [IO.Directory]::Delete($resolvedProfileRoot)
}

$measuredResults = @($results | Where-Object { -not $_.Warmup })
$summaryProperties = [Collections.Generic.List[string]]::new()
$summaryProperties.AddRange([string[]] @(
    'SetupWindowMilliseconds',
    'MainWindowMilliseconds',
    'SettingsWindowMilliseconds',
    'ServersWindowMilliseconds',
    'IdleCpuPercent',
    'PrivateMemoryMiB',
    'WorkingSetMiB',
    'HandleCount',
    'ThreadCount'))
if ($ProfileScenario -eq 'Populated') {
    foreach ($destinationName in @('Console', 'Mods', 'Players', 'Files', 'Backups', 'Tools', 'Settings', 'Home')) {
        $summaryProperties.Add("Server${destinationName}Milliseconds")
    }
}

$summary = [ordered]@{}
foreach ($property in $summaryProperties) {
    $values = @($measuredResults.$property | Sort-Object)
    $middle = [Math]::Floor($values.Count / 2)
    $median = if ($values.Count % 2 -eq 0) {
        ($values[$middle - 1] + $values[$middle]) / 2
    }
    else {
        $values[$middle]
    }
    $summary[$property] = [Math]::Round($median, 3)
}

$report = [ordered]@{
    Timestamp = [DateTime]::Now.ToString('o')
    Executable = $resolvedExecutable
    ProfileScenario = $ProfileScenario
    InstanceCount = if ($ProfileScenario -eq 'Populated') { $InstanceCount } else { 0 }
    ServerCount = if ($ProfileScenario -eq 'Populated') { $ServerCount } else { 0 }
    ProcessorCount = [Environment]::ProcessorCount
    WarmupRuns = $WarmupRuns
    MeasuredRuns = $Runs
    CacheControl = 'Operating-system file caches were not evicted; first-run samples are observational, not certified cold starts.'
    Median = $summary
    Results = $results
}

$budgets = [ordered]@{
    SetupWindowMilliseconds = $MaximumSetupWindowMilliseconds
    MainWindowMilliseconds = $MaximumMainWindowMilliseconds
    SettingsWindowMilliseconds = $MaximumSettingsWindowMilliseconds
    ServersWindowMilliseconds = $MaximumServersWindowMilliseconds
    IdleCpuPercent = $MaximumIdleCpuPercent
    PrivateMemoryMiB = $MaximumPrivateMemoryMiB
}
if ($ProfileScenario -eq 'Populated') {
    foreach ($destinationName in @('Console', 'Mods', 'Players', 'Files', 'Backups', 'Tools', 'Settings', 'Home')) {
        $budgets["Server${destinationName}Milliseconds"] = $MaximumServerTabMilliseconds
    }
}
$budgetResults = foreach ($budget in $budgets.GetEnumerator()) {
    [pscustomobject]@{
        Metric = $budget.Key
        Median = $summary[$budget.Key]
        Maximum = $budget.Value
        Passed = $summary[$budget.Key] -le $budget.Value
    }
}
$report.Budgets = $budgetResults

$results | Format-Table -AutoSize
''
'Measured medians (warm-up runs excluded):'
$summary.GetEnumerator() | Format-Table -AutoSize
''
'Performance budgets:'
$budgetResults | Format-Table -AutoSize

if (-not [string]::IsNullOrWhiteSpace($JsonOutput)) {
    $jsonPath = [IO.Path]::GetFullPath($JsonOutput)
    $jsonDirectory = [IO.Path]::GetDirectoryName($jsonPath)
    if (-not [string]::IsNullOrEmpty($jsonDirectory)) {
        [IO.Directory]::CreateDirectory($jsonDirectory) | Out-Null
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonPath -Encoding utf8
    "JSON report: $jsonPath"
}

if ($EnforceBudgets -and ($budgetResults.Passed -contains $false)) {
    throw 'One or more Phase 11 performance budgets failed.'
}
