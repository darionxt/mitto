<#
.SYNOPSIS
    Records in real Chrome on Windows what the Runtime's tests compare with Chrome. docs/runtime.md explains it.

.DESCRIPTION
    Serves record.html and probe.js from this script's directory on the loopback address, and starts Chrome on that
    page from a new user data directory, with the flags of a Baseline's first start. Chrome runs windowed on the desktop
    of whoever is logged in at the machine, also when this script runs over SSH. What the page posts goes into
    <build>.json in the output directory, where <build> is the build of Chrome that recorded it, such as
    157.0.8103.39.json. Then the script closes Chrome and deletes the directory.

    It needs the rights to listen on the loopback address with HttpListener, which an administrator has.

.PARAMETER Chrome
    The Chrome executable: the installed one by default.

.PARAMETER Out
    Where the recording goes: this script's directory by default.
#>
[CmdletBinding()]
param(
    [string]$Chrome = (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    [string]$Out = '',
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Windows PowerShell has no $PSScriptRoot yet where the parameters get their defaults.
if (-not $Out) { $Out = $PSScriptRoot }

# Names of their own, so that this runs beside tools/baselines/record.ps1, whose tasks are mitto-baselines-*.
$TaskPrefix = 'mitto-runtime-record'
$CloseTimeoutSeconds = 60
$Files = @{ '/record.html' = 'text/html; charset=utf-8'; '/probe.js' = 'text/javascript; charset=utf-8' }

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

function Get-OurChromeProcesses([string]$UserDataDir) {
    # Only the Chrome that uses our directory: the user may have another one open.
    $mark = "--user-data-dir=`"$UserDataDir`""
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($mark) }
}

function Stop-Chrome([string]$UserDataDir) {
    $deadline = (Get-Date).AddSeconds($CloseTimeoutSeconds)
    $nextRequest = Get-Date
    while ($true) {
        $processes = @(Get-OurChromeProcesses $UserDataDir)
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

function Get-FreePort {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $probe.Start()
    try { return $probe.LocalEndpoint.Port } finally { $probe.Stop() }
}

if (-not (Test-Path $Chrome)) { throw "No Chrome at $Chrome." }
$port = Get-FreePort
$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:$port/")
$listener.Start()
$userDataDir = Join-Path ([IO.Path]::GetTempPath()) "mitto-runtime-$([guid]::NewGuid().ToString('N'))"
$body = $null
try {
    # The flags of a Baseline's first start (docs/baselines.md). With a new directory, Chrome has no variations seed
    # yet, so no field trial changes its features.
    $flags = @(
        "--user-data-dir=`"$userDataDir`""
        '--no-first-run'
        '--no-default-browser-check'
        '--lang=en-US'
        '--variations-override-country=us'
        "http://127.0.0.1:$port/record.html"
    )
    Start-InConsole 'chrome' $Chrome ($flags -join ' ')
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($null -eq $body) {
        $pending = $listener.GetContextAsync()
        while (-not $pending.Wait(500)) {
            if ((Get-Date) -gt $deadline) { throw "The page posted nothing within $TimeoutSeconds seconds." }
        }
        $context = $pending.Result
        $path = $context.Request.Url.AbsolutePath
        $response = $context.Response
        if ($context.Request.HttpMethod -eq 'POST' -and $path -eq '/result') {
            $reader = [IO.StreamReader]::new($context.Request.InputStream, [Text.Encoding]::UTF8)
            $body = $reader.ReadToEnd()
            $reader.Close()
            $response.StatusCode = 204
        } elseif ($Files.ContainsKey($path)) {
            $bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $path.TrimStart('/')))
            $response.ContentType = $Files[$path]
            $response.OutputStream.Write($bytes, 0, $bytes.Length)
        } else {
            $response.StatusCode = 404
        }
        $response.Close()
    }
} finally {
    $listener.Stop()
    Stop-Chrome $userDataDir
    if (Test-Path $userDataDir) { Remove-Item -Recurse -Force $userDataDir }
}

$result = $body | ConvertFrom-Json
if ($result.PSObject.Properties.Name -contains 'error') { throw "The page failed: $($result.error)" }
if (-not $result.chrome) { throw 'The page did not say which Chrome recorded it.' }
$file = Join-Path $Out "$($result.chrome).json"
[IO.File]::WriteAllText($file, $body + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Recorded $file"
