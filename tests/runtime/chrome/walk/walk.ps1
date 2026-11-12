<#
.SYNOPSIS
    A throwaway driver for darionxt/mitto#207, never merged.

.DESCRIPTION
    On the Windows machine, with someone logged in at its desktop: starts walk_server.py, then for each run starts
    Chrome through a scheduled task on that desktop, on walk.html, waits for the walk's result in out\RUN.json, reads
    chrome://version through the debugging port when the run has one, and closes Chrome through its window. Every start
    has --lang=en-US and --variations-override-country=us. Each phase is a set of runs that #207 compares.

    It expects this directory to hold walk.js, walk.html, walk_server.py and cert\{cert.pem,key.pem,spki.txt}.

.PARAMETER Phase
    The phases to run, in order: a b c d e, as listed at the end of this file.

.PARAMETER Scale
    The device scale factor that the run with a forced scale uses.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Phase,
    [string]$Scale = '1'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Web.Extensions

$Here = $PSScriptRoot
$Out = Join-Path $Here 'out'
$Profiles = Join-Path $Here 'profiles'
$LogFile = Join-Path $Here 'walk.log'
$Python = Join-Path $env:USERPROFILE 'mitto-baselines\uv-python\cpython-3.12.16-windows-x86_64-none\python.exe'
$Installed = Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'
$Stable = Join-Path $env:USERPROFILE 'mitto-baselines\stable'
$HttpPort = 18207
$HttpsPort = 18208
$TaskPrefix = 'mitto-m207'
$Spki = (Get-Content (Join-Path $Here 'cert\spki.txt') -Raw).Trim()
$PageLoopback = "http://127.0.0.1:$HttpPort/walk.html"
$PageHttps = "https://capture.test:$HttpsPort/walk.html"
$PageHttpName = "http://capture.test:$HttpPort/walk.html"
$Resolver = @('--host-resolver-rules="MAP capture.test 127.0.0.1"', '--no-proxy-server')
$SpkiFlag = "--ignore-certificate-errors-spki-list=$Spki"

New-Item -ItemType Directory -Force -Path $Out, $Profiles | Out-Null

function Log([string]$Text) {
    $line = "$(Get-Date -Format 'HH:mm:ss') $Text"
    Add-Content -Path $LogFile -Value $line -Encoding utf8
    Write-Host $line
}

function Start-InConsole([string]$Name, [string]$Execute, [string]$Arguments) {
    # Over SSH nothing reaches the logged-in desktop. A task with an interactive principal does, with the user's normal
    # token. What it starts keeps running after the task is gone.
    $task = "$TaskPrefix-$Name"
    $action = New-ScheduledTaskAction -Execute $Execute -Argument $Arguments
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
    try {
        Start-ScheduledTask -TaskName $task
        Start-Sleep -Seconds 2
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false
    }
}

function Get-OurChrome([string]$UserDataDir) {
    # A caller wraps the result in @(), since PowerShell unrolls an array that a function returns.
    $mark = "--user-data-dir=`"$UserDataDir`""
    @(Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($mark) })
}

function Stop-OurChrome([string]$UserDataDir) {
    # Through its window: a forced exit counts towards Chrome's crash streak, which can make it drop its seed.
    $deadline = (Get-Date).AddSeconds(90)
    $next = Get-Date
    while ($true) {
        $processes = @(Get-OurChrome $UserDataDir)
        if ($processes.Count -eq 0) { return }
        if ((Get-Date) -gt $deadline) { throw "Chrome of $UserDataDir did not close." }
        $browser = @($processes | Where-Object { $_.CommandLine -notmatch '--type=' })
        if ($browser.Count -gt 0 -and (Get-Date) -ge $next) {
            $close = "`$null = (Get-Process -Id $($browser[0].ProcessId)).CloseMainWindow()"
            Start-InConsole 'close' 'powershell.exe' "-NoProfile -NonInteractive -WindowStyle Hidden -Command `"$close`""
            $next = (Get-Date).AddSeconds(20)
        }
        Start-Sleep -Milliseconds 500
    }
}

function Get-Serial([string]$UserDataDir) {
    $path = Join-Path $UserDataDir 'Local State'
    if (-not (Test-Path $path)) { return $null }
    for ($i = 0; $i -lt 5; $i++) {
        try {
            $state = Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($state.PSObject.Properties.Name -contains 'variations_seed_serial_number') {
                return $state.variations_seed_serial_number
            }
            return $null
        } catch { Start-Sleep -Milliseconds 300 }
    }
    return $null
}

function Invoke-PageScript([string]$WebSocketUrl, [string]$Expression) {
    $token = [Threading.CancellationToken]::None
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    try {
        $ws.ConnectAsync([Uri]$WebSocketUrl, $token).Wait()
        $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $serializer.MaxJsonLength = [int]::MaxValue
        $request = $serializer.Serialize(@{ id = 1; method = 'Runtime.evaluate'; params = @{ expression = $Expression; returnByValue = $true; awaitPromise = $true } })
        $bytes = [Text.Encoding]::UTF8.GetBytes($request)
        $ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $token).Wait()
        $buffer = New-Object byte[] 65536
        while ($true) {
            $stream = New-Object IO.MemoryStream
            do {
                $task = $ws.ReceiveAsync([ArraySegment[byte]]::new($buffer), $token)
                if (-not $task.Wait(30000)) { throw 'no answer from the page' }
                $stream.Write($buffer, 0, $task.Result.Count)
            } while (-not $task.Result.EndOfMessage)
            $message = $serializer.DeserializeObject([Text.Encoding]::UTF8.GetString($stream.ToArray()))
            if ($message['id'] -eq 1) { return $message['result']['result']['value'] }
        }
    } finally { $ws.Dispose() }
}

function Read-Version([int]$Port, [string]$File) {
    # A new tab of the same Chrome, after the walk has posted, so the walk's page has no client attached.
    $page = Invoke-RestMethod -Method Put -Uri "http://127.0.0.1:$Port/json/new?chrome://version/?show-variations-cmd" -TimeoutSec 15
    Start-Sleep -Seconds 2
    $expression = @'
new Promise((resolve) => {
  const start = Date.now();
  const text = (id) => { const e = document.getElementById(id); return e ? e.innerText : null; };
  const tick = () => {
    const cmd = document.getElementById('variations-cmd').textContent;
    if (cmd || Date.now() - start > 20000) {
      resolve(JSON.stringify({version: text('copy-content'), os: text('copy-os-content'), command_line: text('command_line'),
        source: text('variations-source'), seed: text('variations-seed'), cmd: cmd, list: text('variations-list')}));
    } else { setTimeout(tick, 200); }
  };
  tick();
})
'@
    $value = Invoke-PageScript $page.webSocketDebuggerUrl $expression
    [IO.File]::WriteAllText($File, $value, (New-Object Text.UTF8Encoding($false)))
}

function Save-Processes([string]$UserDataDir, [string]$File) {
    $all = @(Get-OurChrome $UserDataDir)
    $rows = foreach ($p in $all) {
        $type = 'browser'
        if ($p.CommandLine -match '--type=(\S+)') { $type = $Matches[1] }
        $priority = $null
        try { $priority = (Get-Process -Id $p.ProcessId).PriorityClass.ToString() } catch {}
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($p.ParentProcessId)" -ErrorAction SilentlyContinue
        [ordered]@{ type = $type; priority = $priority; parent = $(if ($parent) { $parent.Name } else { $null }); session = $p.SessionId }
    }
    $rows | ConvertTo-Json -Depth 4 | Set-Content -Path $File -Encoding utf8
}

function Invoke-Run {
    param(
        [string]$Name,
        [string]$Chrome,
        [string]$UserDataDir,
        [string]$Page,
        [string[]]$Extra = @(),
        [ValidateSet('task', 'lnk', 'affinity')][string]$Via = 'task',
        [int]$Port = -1,
        [int]$TimeoutSeconds = 120
    )
    $result = Join-Path $Out "$Name.json"
    Remove-Item "$result*" -ErrorAction SilentlyContinue
    if (@(Get-OurChrome $UserDataDir).Count -gt 0) { throw "Chrome of $UserDataDir is still running." }
    $flags = @("--user-data-dir=`"$UserDataDir`"", '--no-first-run', '--no-default-browser-check', '--lang=en-US', '--variations-override-country=us') + $Extra
    if ($Port -ge 0) { $flags += "--remote-debugging-port=$Port" }
    $flags += "${Page}?run=$Name"
    $arguments = $flags -join ' '
    Log "run $Name via $Via, serial before: $(Get-Serial $UserDataDir)"
    Log "  $Chrome $arguments"
    switch ($Via) {
        'task' { Start-InConsole 'chrome' $Chrome $arguments }
        'lnk' {
            # Through the shell, as a shortcut a person opens.
            $lnk = Join-Path $Here "$Name.lnk"
            $shell = New-Object -ComObject WScript.Shell
            $shortcut = $shell.CreateShortcut($lnk)
            $shortcut.TargetPath = $Chrome
            $shortcut.Arguments = $arguments
            $shortcut.Save()
            Start-InConsole 'lnk' 'explorer.exe' "`"$lnk`""
        }
        'affinity' { Start-InConsole 'chrome' 'cmd.exe' "/c start `"`" /affinity 3 `"$Chrome`" $arguments" }
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path $result)) {
        if ((Get-Date) -gt $deadline) {
            Log "  no result within $TimeoutSeconds s"
            Stop-OurChrome $UserDataDir
            return
        }
        Start-Sleep -Milliseconds 500
    }
    Log "  result $((Get-Item $result).Length) bytes"
    Save-Processes $UserDataDir (Join-Path $Out "$Name.proc.json")
    if ($Port -ge 0) {
        $usePort = $Port
        if ($Port -eq 0) { $usePort = [int](Get-Content (Join-Path $UserDataDir 'DevToolsActivePort'))[0] }
        try {
            Invoke-RestMethod -Uri "http://127.0.0.1:$usePort/json/version" -TimeoutSec 15 | ConvertTo-Json | Set-Content -Path (Join-Path $Out "$Name.devtools.json") -Encoding utf8
            Read-Version $usePort (Join-Path $Out "$Name.version.json")
        } catch { Log "  version page failed: $_" }
    }
    Stop-OurChrome $UserDataDir
    Log "  closed, serial after: $(Get-Serial $UserDataDir)"
}

function Wait-Seed([string]$Chrome, [string]$UserDataDir) {
    if (Get-Serial $UserDataDir) { Log "seed already in $UserDataDir"; return }
    $flags = @("--user-data-dir=`"$UserDataDir`"", '--no-first-run', '--no-default-browser-check', '--lang=en-US', '--variations-override-country=us', 'about:blank')
    Start-InConsole 'chrome' $Chrome ($flags -join ' ')
    $deadline = (Get-Date).AddSeconds(360)
    while (-not (Get-Serial $UserDataDir)) {
        if ((Get-Date) -gt $deadline) { Stop-OurChrome $UserDataDir; throw "No seed in $UserDataDir within 6 minutes." }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 5
    Stop-OurChrome $UserDataDir
    Log "seed arrived in $UserDataDir"
}

$server = Start-Process -FilePath $Python -PassThru -WindowStyle Hidden -ArgumentList @(
    "`"$(Join-Path $Here 'walk_server.py')`"", '--out', "`"$Out`"", '--http-port', $HttpPort, '--https-port', $HttpsPort,
    '--cert', "`"$(Join-Path $Here 'cert\cert.pem')`"", '--key', "`"$(Join-Path $Here 'cert\key.pem')`"")
Start-Sleep -Seconds 2
if ($server.HasExited) { throw 'The walk server did not start.' }
Log "server pid $($server.Id), phases $($Phase -join ' ')"

try {
    $c157 = $Installed
    $c155 = Join-Path $Stable '155.0.8059.85\Chrome-bin\chrome.exe'
    $c156 = Join-Path $Stable '156.0.8078.80\Chrome-bin\chrome.exe'
    $c157u = Join-Path $Stable '157.0.8103.39\Chrome-bin\chrome.exe'
    foreach ($p in $Phase) {
        Log "phase $p"
        switch ($p) {
            'a' {
                # The installed Engine Release: no seed, then the seed, and each flag and context in turn.
                $d = Join-Path $Profiles 'p1'
                Invoke-Run 'a01-157-p1-fresh' $c157 $d $PageLoopback
                Wait-Seed $c157 $d
                Invoke-Run 'a02-157-p1-seed' $c157 $d $PageLoopback
                Invoke-Run 'a03-157-p1-seed-again' $c157 $d $PageLoopback
                Invoke-Run 'a04-157-p1-seed-port9207' $c157 $d $PageLoopback -Port 9207
                Invoke-Run 'a05-157-p1-seed-port0' $c157 $d $PageLoopback -Port 0
                Invoke-Run 'a06-157-p1-seed-spki' $c157 $d $PageLoopback -Extra @($SpkiFlag) -Port 9207
                Invoke-Run 'a07-157-p1-seed-https' $c157 $d $PageHttps -Extra (@($SpkiFlag) + $Resolver) -Port 9207
                Invoke-Run 'a08-157-p1-seed-httpname' $c157 $d $PageHttpName -Extra $Resolver -Port 9207
                Invoke-Run 'a09-157-p1-seed-resolver' $c157 $d $PageLoopback -Extra $Resolver -Port 9207
                Invoke-Run 'a10-157-p1-seed-lnk' $c157 $d $PageLoopback -Via lnk -Port 9207
                Invoke-Run 'a11-157-p1-seed-affinity' $c157 $d $PageLoopback -Via affinity -Port 9207
                Invoke-Run 'a12-157-p1-seed-scale' $c157 $d $PageLoopback -Extra @("--force-device-scale-factor=$Scale") -Port 9207
                Invoke-Run 'a13-157-p1-seed-window' $c157 $d $PageLoopback -Extra @('--window-size=1100,750', '--window-position=60,40') -Port 9207
            }
            'b' {
                # A second new user data directory of the same Chrome.
                $d = Join-Path $Profiles 'p2'
                Invoke-Run 'b01-157-p2-fresh' $c157 $d $PageLoopback
                Wait-Seed $c157 $d
                Invoke-Run 'b02-157-p2-seed-port9207' $c157 $d $PageLoopback -Port 9207
                Invoke-Run 'b03-157-p2-seed' $c157 $d $PageLoopback
            }
            'c' {
                $d = Join-Path $Profiles 'p3'
                Invoke-Run 'c01-155-p3-fresh' $c155 $d $PageLoopback
                Wait-Seed $c155 $d
                Invoke-Run 'c02-155-p3-seed-port9207' $c155 $d $PageLoopback -Port 9207
                Invoke-Run 'c03-155-p3-seed-https' $c155 $d $PageHttps -Extra (@($SpkiFlag) + $Resolver) -Port 9207
            }
            'd' {
                $d = Join-Path $Profiles 'p4'
                Invoke-Run 'd01-156-p4-fresh' $c156 $d $PageLoopback
                Wait-Seed $c156 $d
                Invoke-Run 'd02-156-p4-seed-port9207' $c156 $d $PageLoopback -Port 9207
            }
            'e' {
                # The Engine Release unpacked, against the installed one.
                $d = Join-Path $Profiles 'p5'
                Invoke-Run 'e01-157u-p5-fresh' $c157u $d $PageLoopback
                Wait-Seed $c157u $d
                Invoke-Run 'e02-157u-p5-seed-port9207' $c157u $d $PageLoopback -Port 9207
            }
            default { throw "No phase $p." }
        }
    }
    Log 'done'
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}
