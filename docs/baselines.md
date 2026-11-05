# Recording Baselines

A **Baseline** is ground truth: what real Chrome sent. This page is how to record the Baselines of a **Chrome Release** on Windows, because the **Persona** is Windows Chrome. They end up in `tests/baselines/RELEASE/`, where [Parity Checks](parity.md) compare with them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md), and [capture.md](capture.md) explains the capture kit.

## What it takes

- A Windows machine with someone logged in. Chrome runs windowed on that desktop, never headless, because headless Chrome is not the Chrome the Persona claims to be. It needs no clicks, whether you work at the machine or over SSH.
- uv, and a copy of the repository. The machine needs no native build, no Git and no GitHub login. Over SSH, from a checkout:

  ```bash
  git archive --format=tar HEAD -o mitto.tar
  scp mitto.tar HOST:
  ssh HOST mkdir mitto
  ssh HOST tar -xf mitto.tar -C mitto
  ```

- Chrome at the exact stable build, with nothing that can update it.

## Install Chrome

Google's update server names the stable build of a Chrome Release and its installer. Ask it in PowerShell on the Windows machine, with the Chrome Release in `targetversionprefix`:

```powershell
$request = @"
<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" updaterversion="1.3.37.0" shell_version="1.3.37.0" ismachine="0" sessionid="{$([guid]::NewGuid())}" installsource="ondemand" requestid="{$([guid]::NewGuid())}" dedup="cr">
<hw physmemory="16" sse="1" sse2="1" sse3="1" ssse3="1" sse41="1" sse42="1" avx="1"/>
<os platform="win" version="10.0.26100.0" sp="" arch="x64"/>
<app appid="{8A69D345-D564-463C-AFF1-A69D9E530F96}" version="" nextversion="" ap="x64-stable-statsdef_1" lang="en" brand="GCEA" installage="-1"><updatecheck targetversionprefix="157."/></app>
</request>
"@
$answer = [xml](Invoke-WebRequest https://tools.google.com/service/update2 -Method Post -ContentType application/xml -Body $request -UseBasicParsing).Content
$check = $answer.response.app.updatecheck
$package = $check.manifest.packages.package
$url = @($check.urls.url | Where-Object { $_.codebase -like 'https://dl.google.com/*' })[0].codebase + $package.name
$check.manifest.version
$url
```

Compare the version with [Google's release feed](https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases?filter=endtime=none) before you go on: it has to be stable there. Then download the installer, and check that it is the file the server named and that Google signed it:

```powershell
curl.exe -fL --retry 5 -o chrome_installer.exe $url
(Get-FileHash chrome_installer.exe -Algorithm SHA256).Hash -eq $package.hash_sha256
(Get-AuthenticodeSignature chrome_installer.exe).SignerCertificate.Subject
```

The first line must say `True`, and the second must start with `CN=Google LLC`. Install it as the logged-in user. At the machine, run `.\chrome_installer.exe --verbose-logging --do-not-launch-chrome --channel=stable`. Those are the arguments the server's answer gives. Over SSH, the same through a task on that user's desktop:

```powershell
$installer = (Resolve-Path chrome_installer.exe).Path
$action = New-ScheduledTaskAction -Execute $installer -Argument '--verbose-logging --do-not-launch-chrome --channel=stable'
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName mitto-chrome-install -Action $action -Principal $principal
Start-ScheduledTask -TaskName mitto-chrome-install
while ((Get-ScheduledTask -TaskName mitto-chrome-install).State -eq 'Running') { Start-Sleep -Seconds 5 }
Unregister-ScheduledTask -TaskName mitto-chrome-install -Confirm:$false
```

Check the result straight away:

```powershell
(Get-Item "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe").VersionInfo.ProductVersion
Get-ScheduledTask | Where-Object { $_.TaskName -like 'Google*' }
Get-Service | Where-Object { $_.Name -like 'Google*' }
Test-Path "$env:LOCALAPPDATA\Google\GoogleUpdater"
```

The first line must be the version from before. The others must show nothing and `False`.

That is what holds updates back. This installer is the one the updater itself runs, so it installs Chrome and no updater, and nothing can move Chrome to another build. The installer on Chrome's download page brings the updater along and takes whatever build the rollout gives it. Holding that back with registry policies only works on a managed machine. The script below also fails if Chrome's version changes during a session.

To remove Chrome afterwards, with the version it has:

```powershell
& "$env:LOCALAPPDATA\Google\Chrome\Application\157.0.8103.39\Installer\setup.exe" --uninstall --channel=stable --force-uninstall
```

## The variations seed and the country

Chrome fetches a variations seed, and field trials from it apply from the next start. Without it, Chrome looks fine and runs with no field trials, so a recording session waits for the seed, restarts Chrome and checks that `chrome://version` lists active variations.

Studies in a seed can filter by country, so every recording of a Chrome Release uses one fixed country, including later comparisons with it. That country is the US, to go with the `en-US` locale, and `--variations-override-country=us` on every start of Chrome fixes it. Chrome uses that flag instead of what it stored for both session and permanent consistency. Do not leave it to the network's exit: a seed response only tells Chrome the exit's country in its `X-Country` header, and `variations_country` in `Local State` holds that. The seed's bytes were the same through every exit we tried, so that header is the only way the exit reaches Chrome, and the flag replaces it. Which country `Local State` shows only depends on the network. Check that it has a value, which shows the seed arrived, and do not write the value down.

To see that the flag works, read the field trials in `chrome://version/?show-variations-cmd` with and without it. With Chrome 157 and the seed of 2026-11-05, `ContextualTasksDesktop` filters on the US and `LensSidePanelUnification` on everywhere but the US, each with one group at 100%. With the flag the first is there and the second is not. Without it, from an exit elsewhere, it is the other way round.

Capture traffic stays out of all this. Chrome resolves `*.localhost` itself and never sends it through a proxy, and the capture server only listens on loopback. The raw recording holds each client's address as `peer`, so that this can be checked.

## Record a session

From the repository's root on the Windows machine, with a new directory for both paths. `recordings` is ignored by Git:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\record.ps1 -Out recordings\a -UserDataDir $env:USERPROFILE\mitto-chrome\a
```

Over SSH, a dropped connection would end it, so start it outside the SSH session and read its log. An SSH exit of 255 says nothing about the job:

```powershell
$record = 'powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\record.ps1 -Out recordings\a -UserDataDir %USERPROFILE%\mitto-chrome\a'
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "cmd /c $record > record-a.log 2>&1"; CurrentDirectory = "$env:USERPROFILE\mitto" }
Get-Content "$env:USERPROFILE\mitto\record-a.log"
```

It takes about a minute, and ends with `Done:` or with an error. Windows of Chrome open and close on the desktop meanwhile.

### What it does

[`tools/baselines/record.ps1`](../tools/baselines/record.ps1) does the following. At the machine you can do the same by hand, with `chrome` being `%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe`:

1. It starts Chrome with the first five flags below and `about:blank`, waits until `Local State` has `variations_country`, about 15 seconds, and closes Chrome through its window. Do not kill it: Chrome counts that as a crash, and a crash streak makes it give up its seed.
2. It starts the capture server, `uv run python -m tools.capture --out recordings\a`, which prints the hash and the three addresses.
3. For each address, it starts Chrome again with the flags and the address, in the order HTTP/1.1, HTTP/2, HTTP/3. It waits until the server has written that protocol's files, when the page says "Recorded". It reads `chrome://version/?show-variations-cmd` in a new tab of that same Chrome, and closes Chrome. The server exits after the third.
4. It checks that every client's address is loopback, that `Accept-Language` is `en-US`, that Chrome lists active variations, and that Chrome's version did not change.
5. It writes `chrome.txt`: the build, the system, the flags, the seed's serial number and, for each protocol, the network-related part of that page. That part is the field trials and features whose names have to do with connections, protocols, DNS, headers or priorities. The script's `$NetworkNames` is the exact list.

| Flag | On | Why |
|---|---|---|
| `--user-data-dir=DIRECTORY` | Every start | A new directory for each session, with no `--headless` anywhere. Chrome ignores the certificate flag without it |
| `--no-first-run` | Every start | No first-run dialogs |
| `--no-default-browser-check` | Every start | No question about the default browser |
| `--lang=en-US` | Every start | The `en-US` locale, whatever language Windows has |
| `--variations-override-country=us` | Every start | See above |
| `--ignore-certificate-errors-spki-list=HASH` | Recording starts | Trusts the capture server's private certificate |
| `--origin-to-force-quic-on=AUTHORITY` | The HTTP/3 start | Without it, Chrome refuses a certificate from an unknown root over QUIC |
| `--remote-debugging-port=0` | Recording starts, by the script | How the script reads `chrome://version` without a person. At the machine you need no such flag, and `-NoDebuggingPort` leaves it out, without field trials and features in `chrome.txt` |

## Check and commit

1. Record a second session, with new paths, and compare the normal forms where you have Git, such as with `git diff --no-index recordings/a/http1.json recordings/b/http1.json`. They should be identical. If they are not, find the cause and write it under the Chrome Release below. `chrome.txt` will differ, as Chrome 157 shows below.
2. Check that the files hold nothing personal. A normal form has placeholders for the host and no paths. `chrome.txt` has no paths, and the script leaves the page's command line and profile path out. Search both for the machine's name, the user's name and anything like a path or an address, in place of `NAME` here:

   ```bash
   grep -n -i -E 'NAME|[a-z]:\\|/users/|[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:' recordings/a/http?.json recordings/a/chrome.txt
   ```

3. Copy `http1.json`, `http2.json`, `http3.json` and `chrome.txt` of the first session to `tests/baselines/RELEASE/`. Leave out the raw recordings and logs, which are not Baselines.
4. Regenerate the known-gaps list on a Mac or Linux **Host**, and read its diff. See [parity.md](parity.md#the-known-gaps-list).

## Chrome 157

Build 157.0.8103.39, recorded on 2026-11-05 and 2026-11-06 on Windows 11 build 26300, with a Windows display language other than English. The files in `tests/baselines/157` come from the first session.

Five sessions from new user data directories gave identical normal forms for all four **Request Kinds** over all three protocols. One of them used `-NoDebuggingPort`, so the debugging port leaves no trace on the wire.

Their field trials do differ, with no effect on the normal form:

| Field trials | Differ between | Why |
|---|---|---|
| `HttpCacheZstdCompression`, `PervasiveSharedDictionaries`, `PrioritizeMainFrameNavigationNetworkResponse`, `PrioritizeResizeTaskRunnerOnStartupStudy`, `PrivateAiCloudflareProxy`, `ServiceWorkerRaceNetworkRequestDeprecateTwoPhaseWrite`, `VariationsFeatureAccessLogging` | User data directories | Permanent consistency: a new directory is a new client |
| `OptimisticDnsForTcp`, `ReduceMojoURLLoaderFactoryCloning` | Any two starts of Chrome | Session consistency: Chrome draws them at each start, which is why `chrome.txt` has one list per protocol |

The same seed has one study of QUIC with session consistency that no recording here drew, `IncreaseAmplificationFactor`, with the feature `TryQuicByDefault`. If a later HTTP/3 recording differs in its transport parameters, look at its entry in `chrome.txt` first.

What can still move without a new Chrome Release is in [capture.md](capture.md#what-can-differ-between-chrome-installations).
