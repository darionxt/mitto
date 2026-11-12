<#
.SYNOPSIS
    Records one session of Chrome Baselines on Windows. docs/baselines.md explains the procedure.

.DESCRIPTION
    Starts Chrome from a new user data directory and lets its variations seed arrive. Then it starts the capture server
    and takes a new start of Chrome from that directory through each of the server's runs, so that field trials apply:
    first the page whose Script Fingerprint it records, then the wire's runs. Chrome always runs on the desktop of
    whoever is logged in at the machine, windowed, also when this script runs over SSH.

    The output directory gets what the capture server writes, and chrome.txt, which says which Chrome recorded it.

.PARAMETER Out
    Where the recordings go. It must not hold a recording yet.

.PARAMETER UserDataDir
    The user data directory. It must not exist yet, unless -ExistingUserDataDir is there.

.PARAMETER Country
    The country Chrome evaluates its field trials for.

.PARAMETER Chrome
    The Chrome executable: the installed one, or one that tools/baselines/unpack.ps1 unpacked.

.PARAMETER NoDebuggingPort
    Starts Chrome without a debugging port, so that it holds nothing but the flags of the procedure. This script reads
    chrome://version through that port, so chrome.txt then has no field trials or features, and nothing checks the
    groups of the studies that change what a page sees. The case pages get their clicks through it too, so this records
    the Script Fingerprint and the main Request Kinds only.

.PARAMETER NoVariationsSeed
    For a Chrome that fetches no variations seed, such as Chrome for Testing as it comes. The first start lasts as long
    as a seed usually takes, and Chrome may list no active variations. What this records is a comparison, not a Baseline.

.PARAMETER ExtraFlags
    More flags for every start of Chrome, for a comparison. chrome.txt lists them. What this records is not a Baseline.

.PARAMETER ExistingUserDataDir
    Records with a user data directory that has run before and holds a variations seed, to see what a Chrome that has
    been running for a while sends. Close its Chrome first. What this records is a comparison, not a Baseline.

.PARAMETER SettleSeconds
    Lets each start of Chrome run this long on an empty tab before the page opens. In its first seconds, Chrome has not
    loaded its components yet and a recording is over by then. What this records is a comparison, not a Baseline.

.PARAMETER Tickets
    Has the capture server give out session tickets, and takes Chrome through the runs that resume instead of the others.
    What this records is a comparison, not a Baseline.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$UserDataDir,
    [string]$Country = 'us',
    [string]$Chrome = (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    [switch]$NoDebuggingPort,
    [switch]$NoVariationsSeed,
    [string[]]$ExtraFlags = @(),
    [switch]$ExistingUserDataDir,
    [int]$SettleSeconds = 0,
    [switch]$Tickets
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Web.Extensions

# What decides whether a field trial or feature goes into chrome.txt, by its name. It casts a wide net on purpose: a
# name too many costs one line, and a name too few hides what changed the wire.
$NetworkNames = '(?i)quic|http|spdy|tls|ssl|encryptedclienthello|dns|network|socket|cert|alps|mlkem|kyber|tcp|ipv[46]|ipmatching' +
    '|preconnect|proxy|priorit|urlloader|structuredheaders|shareddictionar|zstd|brotli|clienthint|acceptlang|useragent' +
    '|reduceua|connection|preload|prefetch|prerender|hsts|mtc|(?-i:ECH|H2|H3)'

# Chrome turns these on in ChromeMainDelegate::CommonEarlyInitialization unless something overrides them, and //net has
# them off. chrome://version only lists overrides, so with none, only these lines of chrome.txt name them.
$OnInChromesCode = @('PartitionConnectionsByNetworkIsolationKey', 'SplitCacheByNetworkIsolationKey')

$RunsWithoutClicks = @('script/page', 'http1', 'http2', 'http3')
$RunsFile = 'runs.json'
$CertificateFlag = '--ignore-certificate-errors-spki-list'
# A recording start of the wire has a debugging port with no fixed number, through which the server clicks the case pages
# and this script reads chrome://version. That port makes Chrome a browser under automation, whose navigator.webdriver is
# true, so the start of a Script Fingerprint has a fixed port instead, which changes nothing a page sees, and which only
# reads chrome://version once the page has posted its record (#207).
$WireDebuggingPort = '--remote-debugging-port=0'
$ScriptDebuggingPort = '--remote-debugging-port=9207'
# The studies of the variations seed that split what a page sees by user data directory, which #207 found in the seed of
# 2026-11-11 for Chrome 155, 156 and 157. A Baseline of the Script Fingerprint comes from a directory in none of their
# groups but Default, as most installations are.
$PageStudies = @('FencedFramesDeprecation', 'UniversalOptOutSettings', 'V8Turbolev')
$TaskPrefix = 'mitto-baselines'
$SeedTimeoutSeconds = 360
$FirstStartWithoutSeedSeconds = 15
$PageTimeoutSeconds = 120
$CloseTimeoutSeconds = 60

function Write-Step([string]$Message) {
    Write-Output "$(Get-Date -Format 'HH:mm:ss') $Message"
}

function Wait-Until([string]$What, [int]$TimeoutSeconds, [scriptblock]$Condition) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (& $Condition)) {
        if ((Get-Date) -gt $deadline) { throw "Gave up after $TimeoutSeconds seconds waiting for $What." }
        Start-Sleep -Milliseconds 500
    }
}

function Start-InConsole([string]$Name, [string]$Execute, [string]$Arguments) {
    # Over SSH, nothing reaches the logged-in desktop. A task with an interactive principal does, with the user's normal
    # token. What it starts keeps running after the task is gone.
    $task = "$TaskPrefix-$Name"
    $action = New-ScheduledTaskAction -Execute $Execute -Argument $Arguments
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
    try {
        Start-ScheduledTask -TaskName $task
        Start-Sleep -Seconds 1
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false
    }
}

function Start-Chrome([string[]]$Flags, [string]$Url) {
    Start-InConsole 'chrome' $Chrome (($BaseFlags + $Flags + $Url) -join ' ')
}

function Format-Flag([string]$Flag) {
    # Chrome gets one command line, so a value with spaces in it needs quotes.
    $name, $value = $Flag -split '=', 2
    if ($value -match ' ') { return "$name=`"$value`"" }
    return $Flag
}

function Get-OurChromeProcesses {
    # Only the Chrome that uses our directory: the user may have another one open.
    $mark = "--user-data-dir=`"$UserDataDir`""
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($mark) }
}

function Stop-Chrome {
    # A clean exit, because a forced one counts towards Chrome's crash streak, which can make it give up its seed.
    $deadline = (Get-Date).AddSeconds($CloseTimeoutSeconds)
    $nextRequest = Get-Date
    while ($true) {
        $processes = @(Get-OurChromeProcesses)
        if ($processes.Count -eq 0) { return }
        if ((Get-Date) -gt $deadline) { throw "Chrome did not close within $CloseTimeoutSeconds seconds." }
        $browser = @($processes | Where-Object { $_.CommandLine -notmatch '--type=' })
        if ($browser.Count -gt 0 -and (Get-Date) -ge $nextRequest) {
            $close = "`$null = (Get-Process -Id $($browser[0].ProcessId)).CloseMainWindow()"
            Start-InConsole 'close' 'powershell.exe' "-NoProfile -NonInteractive -WindowStyle Hidden -Command `"$close`""
            $nextRequest = (Get-Date).AddSeconds(15)
        }
        Start-Sleep -Milliseconds 500
    }
}

function Read-LocalState {
    $path = Join-Path $UserDataDir 'Local State'
    if (-not (Test-Path $path)) { return $null }
    try {
        return [IO.File]::ReadAllText($path) | ConvertFrom-Json
    } catch {
        # Chrome was replacing the file.
        return $null
    }
}

function Test-LocalStateHas($State, [string]$Name) {
    return ($null -ne $State) -and ($State.PSObject.Properties.Name -contains $Name) -and [bool]$State.$Name
}

function Test-SeedArrived {
    # Chrome writes the serial number once it has stored a seed whose signature holds, and variations_country only when
    # the seed's response named a country, which not every response does. The country's value does not matter: it
    # follows the network, and --variations-override-country decides what counts.
    $state = Read-LocalState
    return (Test-LocalStateHas $state 'variations_seed_serial_number') -or (Test-LocalStateHas $state 'variations_country')
}

function Get-SeedSerialNumber {
    $state = Read-LocalState
    if (Test-LocalStateHas $state 'variations_seed_serial_number') { return $state.variations_seed_serial_number }
    return 'none'
}

function Invoke-PageScript([string]$WebSocketUrl, [string]$Expression) {
    $none = [Threading.CancellationToken]::None
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = [int]::MaxValue
    $socket = New-Object System.Net.WebSockets.ClientWebSocket
    try {
        $socket.ConnectAsync([Uri]$WebSocketUrl, $none).Wait()
        $call = @{ id = 1; method = 'Runtime.evaluate'; params = @{ expression = $Expression; returnByValue = $true; awaitPromise = $true } }
        $bytes = [Text.Encoding]::UTF8.GetBytes($serializer.Serialize($call))
        $socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, $none).Wait()
        $buffer = New-Object byte[] 65536
        while ($true) {
            $message = New-Object IO.MemoryStream
            do {
                $receive = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $none)
                if (-not $receive.Wait(30000)) { throw 'The page did not answer.' }
                $message.Write($buffer, 0, $receive.Result.Count)
            } while (-not $receive.Result.EndOfMessage)
            $answer = $serializer.DeserializeObject([Text.Encoding]::UTF8.GetString($message.ToArray()))
            if ($answer['id'] -eq 1) { return $answer['result']['result']['value'] }
        }
    } finally {
        $socket.Dispose()
    }
}

function Get-DebuggingPort([string]$Flag) {
    # Chrome writes the port it chose to DevToolsActivePort only for port 0. A fixed port is the flag's.
    $port = ($Flag -split '=', 2)[1]
    if ($port -ne '0') { return $port }
    $portFile = Join-Path $UserDataDir 'DevToolsActivePort'
    Wait-Until 'the debugging port' 30 { Test-Path $portFile }
    return (Get-Content $portFile)[0]
}

function Read-VersionPage([string]$Flag) {
    # Chrome does not open chrome:// addresses from its command line, so the debugging port opens this one. The page's
    # command line and paths hold the user's name, so this takes only the rest.
    $port = Get-DebuggingPort $Flag
    $target = Invoke-RestMethod -Method Put -Uri "http://127.0.0.1:$port/json/new?chrome://version/?show-variations-cmd" -TimeoutSec 15
    Start-Sleep -Seconds 1
    $script = @'
new Promise((resolve) => {
  const start = Date.now();
  const text = (id) => document.getElementById(id).innerText.trim();
  const tick = () => {
    const command = document.getElementById('variations-cmd').textContent;
    if (command || Date.now() - start > 20000) {
      resolve(JSON.stringify({
        version: text('copy-content'), system: text('copy-os-content'), userAgent: text('useragent'),
        source: text('variations-source'), command: command, active: text('variations-list'),
      }));
    } else {
      setTimeout(tick, 200);
    }
  };
  tick();
})
'@
    return (Invoke-PageScript $target.webSocketDebuggerUrl $script) | ConvertFrom-Json
}

function Get-Switch([string]$Command, [string]$Name) {
    $match = [regex]::Match($Command, "--$Name=`"([^`"]*)`"")
    if ($match.Success) { return $match.Groups[1].Value }
    return ''
}

function Format-NetworkPart($Page) {
    # A feature names its field trial as Feature<Trial, and a star marks what has been used so far, which says nothing
    # about what the page load did, so it goes away.
    $command = $Page.command
    $features = @{}
    foreach ($switch in 'enable-features', 'disable-features') {
        $features[$switch] = @(((Get-Switch $command $switch) -split ',') | ForEach-Object { $_.TrimStart('*') } |
                Where-Object { $_ -match $NetworkNames } | Sort-Object)
    }
    $wanted = @{}
    foreach ($entry in @($features['enable-features']) + @($features['disable-features'])) {
        if ($entry -match '<(.+)$') { $wanted[$Matches[1]] = $true }
    }
    $fields = (Get-Switch $command 'force-fieldtrials') -split '/'
    $trials = @()
    for ($i = 0; $i + 1 -lt $fields.Count; $i += 2) {
        $name = $fields[$i].TrimStart('*')
        if ($name -match $NetworkNames -or $wanted.ContainsKey($name)) {
            $wanted[$name] = $true
            $trials += "$name/$($fields[$i + 1])"
        }
    }
    $parameters = @(((Get-Switch $command 'force-fieldtrial-params') -split ',') |
            Where-Object { $_ -and $wanted.ContainsKey(($_ -split '\.')[0]) } | Sort-Object)
    $lines = @('--force-fieldtrials:') + @($trials | Sort-Object | ForEach-Object { "  $_" })
    $lines += @('--force-fieldtrial-params:') + @($parameters | ForEach-Object { "  $_" })
    $lines += @('--enable-features:') + @($features['enable-features'] | ForEach-Object { "  $_" })
    $lines += @('--disable-features:') + @($features['disable-features'] | ForEach-Object { "  $_" })
    $lines += 'On in Chrome''s own code, unless overridden:'
    foreach ($name in $OnInChromesCode) {
        $state = 'not overridden'
        foreach ($switch in 'enable-features', 'disable-features') {
            if (@($features[$switch] | Where-Object { ($_ -split '<')[0] -eq $name }).Count -gt 0) { $state = "--$switch" }
        }
        $lines += "  ${name}: $state"
    }
    return $lines
}

function Get-PageStudyGroups($Page) {
    # A study that the seed does not hold for this Chrome Release, or that the directory is not in, has no group.
    $fields = (Get-Switch $Page.command 'force-fieldtrials') -split '/'
    $groups = [ordered]@{}
    foreach ($study in $PageStudies) { $groups[$study] = 'none' }
    for ($i = 0; $i + 1 -lt $fields.Count; $i += 2) {
        $name = $fields[$i].TrimStart('*')
        if ($groups.Contains($name)) { $groups[$name] = $fields[$i + 1] }
    }
    return $groups
}

function Assert-ScriptRecording([string]$Name) {
    # The page must stay on this machine too, and its Chrome must have the en-US locale.
    $raw = [IO.File]::ReadAllText((Join-Path $Out "$Name.raw.json")) | ConvertFrom-Json
    $peers = @($raw.peers | Sort-Object -Unique)
    $elsewhere = @($peers | Where-Object { $_ -notin '127.0.0.1', '::1' })
    if ($peers.Count -eq 0 -or $elsewhere.Count -gt 0) { throw "$Name has peers other than loopback: $($peers -join ', ')" }
    $text = [IO.File]::ReadAllText((Join-Path $Out "$Name.json"))
    if (-not $text.Contains('["language", "en-US"]')) { throw "$Name was not recorded with the en-US locale." }
    Write-Step "$Name peers: $($peers -join ', '); navigator.language is en-US"
}

function Assert-Recording([string]$Name) {
    # The capture traffic must stay on this machine, whatever proxy the network has.
    $raw = [IO.File]::ReadAllText((Join-Path $Out "$Name.raw.json")) | ConvertFrom-Json
    $peers = @($raw.connections | ForEach-Object { $_.peer } | Sort-Object -Unique)
    $elsewhere = @($peers | Where-Object { $_ -notin '127.0.0.1', '::1' })
    if ($peers.Count -eq 0 -or $elsewhere.Count -gt 0) { throw "$Name has peers other than loopback: $($peers -join ', ')" }
    $text = [IO.File]::ReadAllText((Join-Path $Out "$Name.json"))
    if ($text -notmatch '(?i)accept-language: en-US') { throw "$Name was not recorded with an en-US Accept-Language." }
    Write-Step "$Name peers: $($peers -join ', '); Accept-Language is en-US"
}

$repository = Split-Path (Split-Path $PSScriptRoot)
# Chrome starts in another directory, so a relative path would mean something else to it.
$UserDataDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($UserDataDir)
if (-not (Test-Path $Chrome)) { throw "No Chrome at $Chrome." }
$Chrome = (Resolve-Path $Chrome).Path
if ((Test-Path $UserDataDir) -and -not $ExistingUserDataDir) { throw "$UserDataDir exists. Each session needs a new user data directory." }
if ($Tickets -and $NoDebuggingPort) { throw 'The runs that resume are not the main runs, which is all -NoDebuggingPort records.' }
if ($ExistingUserDataDir) {
    # Before the try below, whose cleanup closes every Chrome on this directory.
    if (@(Get-OurChromeProcesses).Count -gt 0) { throw "A Chrome is still running from $UserDataDir." }
    if (-not (Test-SeedArrived)) { throw "$UserDataDir holds no variations seed." }
}
# A file that is there already would count as that run's recording, with nothing recorded.
if ((Test-Path $Out) -and @(Get-ChildItem $Out -Recurse -File -Filter '*.json').Count -gt 0) {
    throw "$Out already holds a recording."
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$installed = (Get-Item $Chrome).VersionInfo.ProductVersion
$product = (Get-Item $Chrome).VersionInfo.ProductName
$isBaseline = ($product -eq 'Google Chrome') -and (-not $NoVariationsSeed) -and ($ExtraFlags.Count -eq 0) -and (-not $ExistingUserDataDir) -and ($SettleSeconds -eq 0) -and (-not $Tickets)
Write-Step "$product $installed"

$BaseFlags = @(
    "--user-data-dir=`"$UserDataDir`""
    '--no-first-run'
    '--no-default-browser-check'
    '--lang=en-US'
    "--variations-override-country=$Country"
) + $ExtraFlags
function Get-DebuggingFlags($Run) {
    if ($NoDebuggingPort) { return @() }
    if ($Run.fingerprint -eq 'script') { return @($ScriptDebuggingPort) }
    return @($WireDebuggingPort)
}
$server = $null
$pages = @{}
try {
    if ($ExistingUserDataDir) {
        Write-Step 'The directory holds a variations seed already'
    } elseif ($NoVariationsSeed) {
        Write-Step 'First start, with no variations seed to wait for'
        Start-Chrome @() 'about:blank'
        Start-Sleep -Seconds $FirstStartWithoutSeedSeconds
        Stop-Chrome
    } else {
        Write-Step 'Waiting for the variations seed'
        Start-Chrome @() 'about:blank'
        Wait-Until 'the variations seed' $SeedTimeoutSeconds { Test-SeedArrived }
        Stop-Chrome
        if (-not (Test-SeedArrived)) { throw 'Local State lost the variations seed when Chrome closed.' }
        $country = if (Test-LocalStateHas (Read-LocalState) 'variations_country') { 'with' } else { 'without' }
        Write-Step "Variations seed $(Get-SeedSerialNumber), $country variations_country in Local State"
    }

    $serverOutput = Join-Path $Out 'server.log'
    $serverErrors = Join-Path $Out 'server-errors.log'
    $serverArguments = @('run', 'python', '-m', 'tools.capture', '--out', "`"$Out`"")
    if ($Tickets) { $serverArguments += '--tickets' }
    if ($NoDebuggingPort) {
        $serverArguments += @('--runs') + $RunsWithoutClicks
    } else {
        $serverArguments += @('--click-in', "`"$UserDataDir`"")
    }
    $server = Start-Process -FilePath 'uv' -ArgumentList $serverArguments `
        -WorkingDirectory $repository -NoNewWindow -PassThru `
        -RedirectStandardOutput $serverOutput -RedirectStandardError $serverErrors
    $null = $server.Handle # Without this, its exit code gets lost.
    Wait-Until 'the capture server' 120 {
        if ($server.HasExited) { throw "The capture server stopped: $(Get-Content $serverErrors -Raw)" }
        (Test-Path $serverOutput) -and (Select-String -Path $serverOutput -Pattern '^Waiting for' -Quiet)
    }
    $runs = ([IO.File]::ReadAllText((Join-Path $Out $RunsFile)) | ConvertFrom-Json).runs

    foreach ($run in $runs) {
        Write-Step "Recording $($run.label)"
        $flags = @($run.flags | ForEach-Object { Format-Flag $_ }) + @(Get-DebuggingFlags $run)
        Remove-Item (Join-Path $UserDataDir 'DevToolsActivePort') -ErrorAction SilentlyContinue
        if ($SettleSeconds -gt 0) {
            # The second start only hands the address to the Chrome that is running.
            Start-Chrome $flags 'about:blank'
            Start-Sleep -Seconds $SettleSeconds
        }
        Start-Chrome $flags $run.url
        $written = Join-Path $Out "$($run.name).json"
        Wait-Until "the page at $($run.url)" $PageTimeoutSeconds {
            if ($server.HasExited -and -not (Test-Path $written)) {
                throw "The capture server stopped: $(Get-Content $serverErrors -Raw)"
            }
            Test-Path $written
        }
        if (-not $NoDebuggingPort) { $pages[$run.name] = Read-VersionPage (@(Get-DebuggingFlags $run)[0]) }
        if ($isBaseline -and $run.fingerprint -eq 'script' -and $pages.ContainsKey($run.name)) {
            # A group of these is a choice of the directory, which a new one draws again.
            $groups = Get-PageStudyGroups $pages[$run.name]
            $drawn = @($groups.Keys | Where-Object { $groups[$_] -notin 'Default', 'none' } | ForEach-Object { "$_/$($groups[$_])" })
            if ($drawn.Count -gt 0) {
                throw "This directory drew $($drawn -join ', '), which changes what a page sees. Record again from a new user data directory."
            }
        }
        Stop-Chrome
    }

    if (-not $server.WaitForExit(30000)) { throw 'The capture server did not finish.' }
    if ($server.ExitCode -ne 0) { throw "The capture server failed: $(Get-Content $serverErrors -Raw)" }
    $server = $null

    foreach ($run in $runs) {
        if ($run.fingerprint -eq 'script') { Assert-ScriptRecording $run.name } else { Assert-Recording $run.name }
    }
    if ((Get-Item $Chrome).VersionInfo.ProductVersion -ne $installed) { throw 'Chrome changed version during the session.' }

    $flags = @($BaseFlags | Select-Object -Skip 1) + "$CertificateFlag=<hash>"
    $directory = if ($ExistingUserDataDir) { '<directory that has run before>' } else { '<new directory>' }
    $purpose = 'Which Chrome recorded the Baselines in this directory. See docs/baselines.md.'
    if (-not $isBaseline) { $purpose = 'Which Chrome recorded this comparison, which is not a Baseline. See docs/baselines.md.' }
    $note = @(
        $purpose
        ''
        "Recorded: $(Get-Date -Format 'yyyy-MM-dd')"
        "Product: $product"
        "Chrome: $installed"
        "Flags: --user-data-dir=$directory $($flags -join ' ')"
        'Flags of a recording on top of those, without their values but the debugging port''s:'
    )
    foreach ($run in $runs) {
        $names = @($run.flags | ForEach-Object { ($_ -split '=', 2)[0] } | Where-Object { $_ -ne $CertificateFlag })
        $note += "  $($run.name).json: $((@($names) + @(Get-DebuggingFlags $run)) -join ' ')"
    }
    $note += "Variations seed: $(Get-SeedSerialNumber)"
    if ($pages.Count -gt 0) {
        $first = $pages[$runs[0].name]
        $note += @("Version: $($first.version)", "System: $($first.system)", "User agent: $($first.userAgent)")
    }
    foreach ($run in $runs) {
        if (-not $pages.ContainsKey($run.name)) { continue }
        $page = $pages[$run.name]
        if (-not $page.version.StartsWith($installed)) { throw "chrome://version says $($page.version), not $installed." }
        if (-not $NoVariationsSeed -and -not $page.active) {
            throw "chrome://version lists no active variations for $($run.name), so the seed did not apply."
        }
        if ($run.fingerprint -eq 'script') {
            $note += @('', "$($run.name).json: the groups of the studies that change what a page sees, in the Chrome that recorded it")
            $groups = Get-PageStudyGroups $page
            foreach ($study in $groups.Keys) { $note += "  ${study}: $($groups[$study])" }
            continue
        }
        $note += @('', "$($run.name).json: the network-related part of chrome://version/?show-variations-cmd in the Chrome that recorded it")
        $note += Format-NetworkPart $page
    }
    [IO.File]::WriteAllText((Join-Path $Out 'chrome.txt'), (($note -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Step "Done: $Out"
} finally {
    Stop-Chrome
    if ($null -ne $server -and -not $server.HasExited) { & taskkill.exe /T /F /PID $server.Id | Out-Null }
}
