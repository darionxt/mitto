<#
.SYNOPSIS
    Records one session of Chrome Baselines on Windows. docs/baselines.md explains the procedure.

.DESCRIPTION
    Starts Chrome from a new user data directory and lets its variations seed arrive. Then it starts the capture server
    and records each protocol in a new start of Chrome from that directory, so that field trials apply. Chrome always
    runs on the desktop of whoever is logged in at the machine, windowed, also when this script runs over SSH.

    The output directory gets what the capture server writes, and chrome.txt, which says which Chrome recorded it.

.PARAMETER Out
    Where the recordings go. It must not hold a recording yet.

.PARAMETER UserDataDir
    The user data directory. It must not exist yet.

.PARAMETER Country
    The country Chrome evaluates its field trials for.

.PARAMETER Chrome
    The Chrome executable: the installed one, or one that tools/baselines/unpack.ps1 unpacked.

.PARAMETER NoDebuggingPort
    Starts Chrome without a debugging port, so that it holds nothing but the flags of the procedure. This script reads
    chrome://version through that port, so chrome.txt then has no field trials or features.

.PARAMETER NoVariationsSeed
    For a Chrome that fetches no variations seed, such as Chrome for Testing as it comes. The first start lasts as long
    as a seed usually takes, and Chrome may list no active variations. What this records is a comparison, not a Baseline.

.PARAMETER ExtraFlags
    More flags for every start of Chrome, for a comparison. chrome.txt lists them. What this records is not a Baseline.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$UserDataDir,
    [string]$Country = 'us',
    [string]$Chrome = (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    [switch]$NoDebuggingPort,
    [switch]$NoVariationsSeed,
    [string[]]$ExtraFlags = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Web.Extensions

# What decides whether a field trial or feature goes into chrome.txt, by its name. It casts a wide net on purpose: a
# name too many costs one line, and a name too few hides what changed the wire.
$NetworkNames = '(?i)quic|http|spdy|tls|ssl|encryptedclienthello|dns|network|socket|cert|alps|mlkem|kyber|tcp|ipv[46]|ipmatching' +
    '|preconnect|proxy|priorit|urlloader|structuredheaders|shareddictionar|zstd|brotli|clienthint|acceptlang|useragent' +
    '|reduceua|connection|preload|prefetch|prerender|hsts|(?-i:ECH|H2|H3)'

$Protocols = @(
    @{ Stem = 'http1'; Label = 'HTTP/1.1' }
    @{ Stem = 'http2'; Label = 'HTTP/2' }
    @{ Stem = 'http3'; Label = 'HTTP/3' }
)
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

function Test-SeedArrived {
    $state = Read-LocalState
    # The value does not matter: it follows the network, and --variations-override-country decides what counts.
    return ($null -ne $state) -and ($state.PSObject.Properties.Name -contains 'variations_country') -and
        [bool]$state.variations_country
}

function Get-SeedSerialNumber {
    $state = Read-LocalState
    if (($null -ne $state) -and ($state.PSObject.Properties.Name -contains 'variations_seed_serial_number')) {
        return $state.variations_seed_serial_number
    }
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

function Read-VersionPage {
    # Chrome does not open chrome:// addresses from its command line, so the debugging port opens this one. The page's
    # command line and paths hold the user's name, so this takes only the rest.
    $portFile = Join-Path $UserDataDir 'DevToolsActivePort'
    Wait-Until 'the debugging port' 30 { Test-Path $portFile }
    $port = (Get-Content $portFile)[0]
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
    return $lines
}

function Assert-Recording([string]$Stem) {
    # The capture traffic must stay on this machine, whatever proxy the network has.
    $raw = [IO.File]::ReadAllText((Join-Path $Out "$Stem.raw.json")) | ConvertFrom-Json
    $peers = @($raw.connections | ForEach-Object { $_.peer } | Sort-Object -Unique)
    $elsewhere = @($peers | Where-Object { $_ -notin '127.0.0.1', '::1' })
    if ($peers.Count -eq 0 -or $elsewhere.Count -gt 0) { throw "$Stem has peers other than loopback: $($peers -join ', ')" }
    $text = [IO.File]::ReadAllText((Join-Path $Out "$Stem.json"))
    if ($text -notmatch '(?i)accept-language: en-US') { throw "$Stem was not recorded with an en-US Accept-Language." }
    Write-Step "$Stem peers: $($peers -join ', '); Accept-Language is en-US"
}

$repository = Split-Path (Split-Path $PSScriptRoot)
# Chrome starts in another directory, so a relative path would mean something else to it.
$UserDataDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($UserDataDir)
if (-not (Test-Path $Chrome)) { throw "No Chrome at $Chrome." }
$Chrome = (Resolve-Path $Chrome).Path
if (Test-Path $UserDataDir) { throw "$UserDataDir exists. Each session needs a new user data directory." }
foreach ($protocol in $Protocols) {
    if (Test-Path (Join-Path $Out "$($protocol.Stem).json")) { throw "$Out already holds a recording." }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$installed = (Get-Item $Chrome).VersionInfo.ProductVersion
$product = (Get-Item $Chrome).VersionInfo.ProductName
$isBaseline = ($product -eq 'Google Chrome') -and (-not $NoVariationsSeed) -and ($ExtraFlags.Count -eq 0)
Write-Step "$product $installed"

$BaseFlags = @(
    "--user-data-dir=`"$UserDataDir`""
    '--no-first-run'
    '--no-default-browser-check'
    '--lang=en-US'
    "--variations-override-country=$Country"
) + $ExtraFlags
$RecordingFlags = @('--ignore-certificate-errors-spki-list=<hash>')
if (-not $NoDebuggingPort) { $RecordingFlags += '--remote-debugging-port=0' }
$server = $null
$pages = @{}
try {
    if ($NoVariationsSeed) {
        Write-Step 'First start, with no variations seed to wait for'
        Start-Chrome @() 'about:blank'
        Start-Sleep -Seconds $FirstStartWithoutSeedSeconds
        Stop-Chrome
    } else {
        Write-Step 'Waiting for the variations seed'
        Start-Chrome @() 'about:blank'
        Wait-Until 'the variations seed' $SeedTimeoutSeconds { Test-SeedArrived }
        Stop-Chrome
        if (-not (Test-SeedArrived)) { throw 'Local State lost variations_country when Chrome closed.' }
    }

    $serverOutput = Join-Path $Out 'server.log'
    $serverErrors = Join-Path $Out 'server-errors.log'
    $server = Start-Process -FilePath 'uv' -ArgumentList @('run', 'python', '-m', 'tools.capture', '--out', "`"$Out`"") `
        -WorkingDirectory $repository -NoNewWindow -PassThru `
        -RedirectStandardOutput $serverOutput -RedirectStandardError $serverErrors
    $null = $server.Handle # Without this, its exit code gets lost.
    Wait-Until 'the capture server' 120 {
        if ($server.HasExited) { throw "The capture server stopped: $(Get-Content $serverErrors -Raw)" }
        (Test-Path $serverOutput) -and (Select-String -Path $serverOutput -Pattern '^Waiting for' -Quiet)
    }
    $printed = Get-Content $serverOutput
    $spki = (($printed | Select-String -Pattern '^SPKI hash: (.+)$').Matches[0].Groups[1].Value)
    foreach ($protocol in $Protocols) {
        $line = $printed | Select-String -Pattern ('^' + [regex]::Escape($protocol.Label) + ':\s+(\S+)$')
        $protocol.Url = $line.Matches[0].Groups[1].Value
    }

    foreach ($protocol in $Protocols) {
        Write-Step "Recording $($protocol.Label)"
        $flags = @($RecordingFlags | ForEach-Object { $_.Replace('<hash>', $spki) })
        if ($protocol.Stem -eq 'http3') { $flags += "--origin-to-force-quic-on=$(([Uri]$protocol.Url).Authority)" }
        Remove-Item (Join-Path $UserDataDir 'DevToolsActivePort') -ErrorAction SilentlyContinue
        Start-Chrome $flags $protocol.Url
        $written = Join-Path $Out "$($protocol.Stem).json"
        Wait-Until "the test page on $($protocol.Url)" $PageTimeoutSeconds {
            if ($server.HasExited -and -not (Test-Path $written)) {
                throw "The capture server stopped: $(Get-Content $serverErrors -Raw)"
            }
            Test-Path $written
        }
        if (-not $NoDebuggingPort) { $pages[$protocol.Stem] = Read-VersionPage }
        Stop-Chrome
    }

    if (-not $server.WaitForExit(30000)) { throw 'The capture server did not finish.' }
    if ($server.ExitCode -ne 0) { throw "The capture server failed: $(Get-Content $serverErrors -Raw)" }
    $server = $null

    foreach ($protocol in $Protocols) { Assert-Recording $protocol.Stem }
    if ((Get-Item $Chrome).VersionInfo.ProductVersion -ne $installed) { throw 'Chrome changed version during the session.' }

    $flags = @($BaseFlags | Select-Object -Skip 1) + $RecordingFlags
    $purpose = 'Which Chrome recorded the Baselines in this directory. See docs/baselines.md.'
    if (-not $isBaseline) { $purpose = 'Which Chrome recorded this comparison, which is not a Baseline. See docs/baselines.md.' }
    $note = @(
        $purpose
        ''
        "Recorded: $(Get-Date -Format 'yyyy-MM-dd')"
        "Product: $product"
        "Chrome: $installed"
        "Flags: --user-data-dir=<new directory> $($flags -join ' ')"
        'Flags for HTTP/3 only: --origin-to-force-quic-on=<authority>'
        "Variations seed: $(Get-SeedSerialNumber)"
    )
    if ($pages.Count -gt 0) {
        $first = $pages['http1']
        $note += @("Version: $($first.version)", "System: $($first.system)", "User agent: $($first.userAgent)")
    }
    foreach ($protocol in $Protocols) {
        if (-not $pages.ContainsKey($protocol.Stem)) { continue }
        $page = $pages[$protocol.Stem]
        if (-not $page.version.StartsWith($installed)) { throw "chrome://version says $($page.version), not $installed." }
        if (-not $NoVariationsSeed -and -not $page.active) {
            throw "chrome://version lists no active variations for $($protocol.Stem), so the seed did not apply."
        }
        $note += @('', "$($protocol.Stem).json: the network-related part of chrome://version/?show-variations-cmd in the Chrome that recorded it")
        $note += Format-NetworkPart $page
    }
    [IO.File]::WriteAllText((Join-Path $Out 'chrome.txt'), (($note -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Step "Done: $Out"
} finally {
    Stop-Chrome
    if ($null -ne $server -and -not $server.HasExited) { & taskkill.exe /T /F /PID $server.Id | Out-Null }
}
