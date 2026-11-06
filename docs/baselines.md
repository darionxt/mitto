# Recording Baselines

A **Baseline** is ground truth: what real Chrome sent. This page is how to record the Baselines of a **Chrome Release** on Windows, because the **Persona** is Windows Chrome, and [where each Chrome Release comes from](#where-each-chrome-release-comes-from). They end up in `tests/baselines/RELEASE/`, where [Parity Checks](parity.md) compare with them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md), and [capture.md](capture.md) explains the capture kit.

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

### An older Chrome Release

The update server also answers for older Chrome Releases, so ask it the same way with `155.` or `156.` in `targetversionprefix`. A build that stopped serving is not in the release feed's list above. To check an older build, ask the feed for the builds of that Chrome Release that can be pinned. The server's answer must be the newest of them:

```text
https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases?filter=version>=155,version<156,pinnable=true
```

Download and check the installer as above, then unpack it instead of installing it. A user can only have one build installed, and this leaves that one as it is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\unpack.ps1 -Installer chrome_installer.exe -Out $env:USERPROFILE\mitto-chrome-bin\155
```

It needs no administrator and no updater can reach the result. Name `Chrome-bin\chrome.exe` in that directory with `-Chrome` when you record. It gets a variations seed like an installed Chrome, and [on the wire it is the same](#chrome-157).

Google does not say how long the server keeps answering. On 2026-11-06 it answered for every Chrome Release from 145 to 157, and not for 144 or older. So keep the installer until the Chrome Release leaves the **Supported Window**.

## The variations seed and the country

Chrome fetches a variations seed, and field trials from it apply from the next start. Without it, Chrome looks fine and runs with no field trials, so a recording session waits for the seed, restarts Chrome and checks that `chrome://version` lists active variations.

Studies in a seed can filter by country, so every recording of a Chrome Release uses one fixed country, including later comparisons with it. That country is the US, to go with the `en-US` locale, and `--variations-override-country=us` on every start of Chrome fixes it. Chrome uses that flag instead of what it stored for both session and permanent consistency. Do not leave it to the network's exit: a seed response only tells Chrome the exit's country in its `X-Country` header, and `variations_country` in `Local State` holds that. The seed's bytes were the same through every exit we tried, so that header is the only way the exit reaches Chrome, and the flag replaces it. Which country `Local State` shows only depends on the network. Check that it has a value, which shows the seed arrived, and do not write the value down.

To see that the flag works, read the field trials in `chrome://version/?show-variations-cmd` with and without it. With Chrome 157 and the seed of 2026-11-05, `ContextualTasksDesktop` filters on the US and `LensSidePanelUnification` on everywhere but the US, each with one group at 100%. With the flag the first is there and the second is not. Without it, from an exit elsewhere, it is the other way round.

Capture traffic stays out of all this. Chrome resolves `*.localhost` itself and never sends it through a proxy. The [case pages](capture.md#recording-cases) are at names that are not local, so their starts of Chrome have `--no-proxy-server`, and `--host-resolver-rules` sends those names to loopback with no DNS. The capture server only listens on loopback, and its own proxy refuses a tunnel to any name but its own, so nothing that reaches it leaves the machine. The raw recording holds each client's address as `peer`, so that this can be checked.

`chrome.txt` shows what one session drew, and only names that look network-related. The seed holds every study. To read it, take `VariationsSafeSeedV2` from the user data directory: zstd over a protocol buffer whose field 1 is the `VariationsSeed` of `components/variations/proto/variations_seed.proto`, with the serial number that `chrome.txt` names. In the seed of 2026-11-05, no study names a feature that decides the ClientHello over TCP.

## Record a session

From the repository's root on the Windows machine, with a new directory for both paths. `recordings` is ignored by Git:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\record.ps1 -Out recordings\a -UserDataDir $env:USERPROFILE\mitto-chrome\a
```

For an unpacked Chrome, add `-Chrome` with its `chrome.exe`.

Over SSH, a dropped connection would end it, so start it outside the SSH session and read its log. An SSH exit of 255 says nothing about the job:

```powershell
$record = 'powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\record.ps1 -Out recordings\a -UserDataDir %USERPROFILE%\mitto-chrome\a'
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "cmd /c $record > record-a.log 2>&1"; CurrentDirectory = "$env:USERPROFILE\mitto" }
Get-Content "$env:USERPROFILE\mitto\record-a.log"
```

It takes about two minutes, and ends with `Done:` or with an error. Windows of Chrome open and close on the desktop meanwhile. If a case page fails, the reason is in `server-errors.log` in the output directory.

### What it does

[`tools/baselines/record.ps1`](../tools/baselines/record.ps1) does the following. At the machine you can do the same by hand, with `chrome` being `%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe`:

1. It starts Chrome with the first five flags below and `about:blank`, waits until `Local State` has `variations_country`, about 15 seconds, and closes Chrome through its window. Do not kill it: Chrome counts that as a crash, and a crash streak makes it give up its seed.
2. It starts the capture server, `uv run python -m tools.capture --out recordings\a --click-in USER_DATA_DIRECTORY`, which writes the address and flags of [each run](capture.md#recording-cases) to `runs.json`.
3. For each run, it starts Chrome again with its flags and its address, in the order of that list. The server clicks what the case pages ask to have clicked, through the debugging port. The script waits until the server has written that run's files, when the page says "Recorded". It reads `chrome://version/?show-variations-cmd` in a new tab of that same Chrome, and closes Chrome. The server exits after the last.
4. It checks that every client's address is loopback, that `Accept-Language` is `en-US`, that Chrome lists active variations, and that Chrome's version did not change.
5. It writes `chrome.txt`: the product and its build, the system, the flags, the seed's serial number and, for each run, the network-related part of that page. That part is the field trials and features whose names have to do with connections, protocols, DNS, headers or priorities. The script's `$NetworkNames` is the exact list.

| Flag | On | Why |
|---|---|---|
| `--user-data-dir=DIRECTORY` | Every start | A new directory for each session, with no `--headless` anywhere. Chrome ignores the certificate flag without it |
| `--no-first-run` | Every start | No first-run dialogs |
| `--no-default-browser-check` | Every start | No question about the default browser |
| `--lang=en-US` | Every start | The `en-US` locale, whatever language Windows has |
| `--variations-override-country=us` | Every start | See above |
| `--ignore-certificate-errors-spki-list=HASH` | Recording starts | Trusts the capture server's private certificate |
| `--origin-to-force-quic-on=AUTHORITY,...` | The HTTP/3 starts | Without it, Chrome refuses a certificate from an unknown root over QUIC. A case page has three names, so that run lists three |
| `--host-resolver-rules=RULES` | Starts at a case page, except through the proxy | Sends the names of the case pages to loopback |
| `--no-proxy-server` | Starts at a case page, except through the proxy | Names that are not local would otherwise go to whatever proxy the machine has |
| `--proxy-server=URL` | The starts through the proxy | Makes Chrome reach the case page through the capture server's proxy. What else Chrome sends meanwhile, such as asking for a new seed, the proxy refuses |
| `--remote-debugging-port=0` | Recording starts, by the script | How the script reads `chrome://version` and clicks the case pages without a person. At the machine you need no such flag, and you click yourself. `-NoDebuggingPort` leaves it out, without field trials and features in `chrome.txt` and with only the main **Request Kinds** |

## Check and commit

1. Record a second session, with new paths, and compare the normal forms where you have Git, such as with `git diff --no-index recordings/a/http1.json recordings/b/http1.json` and the same for each file in `cases`. They should be identical. If they are not, find the cause and write it under the Chrome Release below. `chrome.txt` will differ, as Chrome 157 shows below.
2. Check that the files hold nothing personal. A normal form has placeholders for the host and no paths. `chrome.txt` has no paths, and the script leaves the page's command line and profile path out. Search both for the machine's name, the user's name and anything like a path or an address, in place of `NAME` here:

   ```bash
   grep -n -i -E 'NAME|[a-z]:\\|/users/|[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:' recordings/a/http?.json recordings/a/cases/{http?,plain_http,proxy_http?}.json recordings/a/chrome.txt
   ```

3. Copy `http1.json`, `http2.json`, `http3.json` and `chrome.txt` of the first session to `tests/baselines/RELEASE/`, and the normal forms in its `cases` directory to `tests/baselines/RELEASE/cases/`. Leave out `runs.json`, the raw recordings and the logs, which are not Baselines.
4. Regenerate the known-gaps list on a Mac or Linux **Host**, and read its diff. See [parity.md](parity.md#the-known-gaps-list).

## Where each Chrome Release comes from

| Chrome Release | Source | Build | SHA-256 of the installer | Corrections |
|---|---|---|---|---|
| 155 | [Google's update server](#an-older-chrome-release), unpacked | 155.0.8059.85 | `2a9b18526096b614fbfda4324280fc69316e42dcb0668301f2cd8a4524054664` | None |
| 156 | [Google's update server](#an-older-chrome-release), unpacked | 156.0.8078.80 | `43cca215206b634b89d226ed71522c7f58c5430b8559b92e782dd7bd77add7cc` | None |
| 157 | [Google's update server](#install-chrome), installed | 157.0.8103.39 | `389b9e21de9a520a6e24ba807ee8760409ccc837457bbcc460d2fdce0d5e8df5` | None |

All of them are stable Chrome signed by Google, so what they send needs no correcting. Each is the newest build of its Chrome Release that the release feed marks as `pinnable`, which is what the server answers with. The 155 and 156 builds were checked on 2026-11-06 for hash, signature and version, and [recorded](#chrome-155-and-156) that day.

A recording of an older Chrome Release gets the seed of the day it is recorded, as every installation of that Chrome Release does on that day. Each Chrome Release gets [its own cut](#their-variations-seeds) of that seed.

[Chrome for Testing](#chrome-for-testing) is only the fallback, for a Chrome Release the server no longer answers for.

## Chrome 157

Build 157.0.8103.39, recorded on 2026-11-05 and 2026-11-06 on Windows 11 build 26300, with a Windows display language other than English. The files in `tests/baselines/157` come from the first session.

Five sessions from new user data directories gave identical normal forms for all four **Request Kinds** over all three protocols. One of them used `-NoDebuggingPort`, so the debugging port leaves no trace on the wire.

On 2026-11-06, with the same seed, four sessions from a Chrome [unpacked](#an-older-chrome-release) from the same installer gave those normal forms too, so unpacking changes nothing on the wire. So did three of four more from the installed Chrome. The fourth drew the field trial below.

The field trials of the first five do differ, with no effect on the normal form:

| Field trials | Differ between | Why |
|---|---|---|
| `HttpCacheZstdCompression`, `PervasiveSharedDictionaries`, `PrioritizeMainFrameNavigationNetworkResponse`, `PrioritizeResizeTaskRunnerOnStartupStudy`, `PrivateAiCloudflareProxy`, `ServiceWorkerRaceNetworkRequestDeprecateTwoPhaseWrite`, `VariationsFeatureAccessLogging` | User data directories | Permanent consistency: a new directory is a new client |
| `OptimisticDnsForTcp`, `ReduceMojoURLLoaderFactoryCloning` | Any two starts of Chrome | Session consistency: Chrome draws them at each start, which is why `chrome.txt` has one list per protocol |

One field trial does reach the normal form: `IncreaseAmplificationFactor`, with the feature `TryQuicByDefault`. It takes 3 of the 100 slots of a layer, so about 3% of user data directories have it, and then each start of Chrome draws one of three groups. Those set `google_connection_options` to `["8AFF", "ORIG"]`, `["5AFF", "ORIG"]` or the usual `["ORIG"]`. One directory of the 13 here had it, and its HTTP/3 start drew `8AFF`.

A Baseline holds what most installations send. So if a session's HTTP/3 normal form has such an option, and `chrome.txt` shows that group under that file, record another session and leave that one out. The study applies from 146.0.7680.71, so this goes for Chrome 155 and 156 too.

Another reaches the wire where no normal form can show it. `IncreaseHttp3Usage`, with the feature `ConfigureQuicHints`, is in every `chrome.txt` here, and it tells Chrome that `google.com` and `www.google.com` speak QUIC before either has sent an Alt-Svc. The capture server is neither of them, so [`native/network_session_configurator.cc`](../native/network_session_configurator.cc) states this by hand.

What can still move without a new Chrome Release is in [capture.md](capture.md#what-can-differ-between-chrome-installations).

## Chrome 155 and 156

Builds 155.0.8059.85 and 156.0.8078.80, [unpacked](#an-older-chrome-release), recorded on 2026-11-06 on the same Windows machine as Chrome 157. The files in `tests/baselines/155` and `tests/baselines/156` come from the first session of each.

Five sessions of each Chrome Release, from new user data directories, gave identical normal forms for all four **Request Kinds** over all three protocols. None of the ten drew [the field trial that reaches the wire](#chrome-157).

### What differs from Chrome 157

Only `sec-ch-ua` and `User-Agent`, in every Request Kind over every protocol. That is 24 fields per Chrome Release. Everything else in the normal form is the same, including the ClientHello with `trust_anchors`, the HTTP/2 preface, the QUIC parameters and the order of the headers. So [their Personas](persona.md#what-differs-between-chrome-releases) differ in those two lines only.

| Chrome | `sec-ch-ua` | `User-Agent` has |
|---|---|---|
| 155 | `"Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"` | `Chrome/155.0.0.0` |
| 156 | `"Not:A-Brand";v="8", "Chromium";v="156", "Google Chrome";v="156"` | `Chrome/156.0.0.0` |
| 157 | `"Not-A.Brand";v="99", "Google Chrome";v="157", "Chromium";v="157"` | `Chrome/157.0.0.0` |

### Their variations seeds

Each Chrome Release gets its own seed. The server cuts them from one seed by milestone. All three carry the version `20261105-010033.555000-production`:

| Chrome | Studies | Bytes |
|---|---|---|
| 155 | 632 | About 237,880 |
| 156 | 620 | 229,296 |
| 157 | 586 | 209,169 |

Every study in a newer seed is in the older seeds, byte for byte, and all three have the same layers. What an older seed has on top are the studies with a `max_version` below the next Chrome Release, so the server drops a study once a Chrome Release is past it. It does not drop a study for its `min_version`, which Chrome itself applies: 70 studies in 155's seed start after 155.

So what Chrome 157 showed does not simply carry over, because the older seeds have 46 and 34 more studies. Those whose names have to do with the network are in `chrome.txt` and [below](#their-field-trials). Of the others, two come near the network: `RestrictLinkHeaderOnSubresource`, about `Link` response headers, which the capture server does not send, and `PqcCupSigning`, about signing update checks, which do not go to the capture server.

`IncreaseAmplificationFactor` is one of the shared studies, so what [Chrome 157](#chrome-157) shows about it holds here too. It has the same groups, the same `quic_options` and the same slots of its layer.

The serial number in `chrome.txt` can differ between sessions of one Chrome Release, in the part before the `#`. The studies stay the same when it does.

### Their field trials

Sessions differ in the same way as those of Chrome 157, with no effect on the normal form:

| Field trials | Differ between | Chrome Release |
|---|---|---|
| `OptimisticDnsForTcp`, `ReduceMojoURLLoaderFactoryCloning` | Any two starts of Chrome | Both |
| `PervasiveSharedDictionaries`, `SafeBrowsingWaitForDnsForRealTimeLookup`, `ServiceWorkerRaceNetworkRequestDeprecateTwoPhaseWrite` | User data directories | Both |
| `HttpCacheZstdCompression`, `VariationsFeatureAccessLogging` | User data directories | 155 |
| `PageActionsPrioritySelector`, `PrioritizeResizeTaskRunnerOnStartupStudy` | User data directories | 156 |

These apply to the older Chrome Releases and never appeared in the 10 sessions of stable Chrome 157 whose `chrome.txt` we compared, because their `max_version` comes before 157.0.8103.39 or before the 156 build. The normal form shows no trace of them, since it is Chrome 157's but for the two headers:

| Field trial | `max_version` | Chrome Release |
|---|---|---|
| `CdmServicePriorityElevation` | 157.0.8096.0 | 155 and 156 |
| `HttpsFirstBalancedModeAutoEnableDesktop` | 157.0.8093.0 | 155 and 156 |
| `IgnoreQuicCryptoConfigMemoryPressure` | 157.0.8089.0 | 155 and 156 |
| `EnforceP2PSocketPortRestrictionsKillSwitch` | 156.0.8061.0 | 155 |
| `KillSwitch_SearchNavigationPrefetch` | 156.0.8076.0 | 155 |
| `WebRTC-MediaTaskQueuePriorities` | 156.0.8073.999 | 155 |

`IncreaseHttp3Usage` is the same in all three: the group `EnabledWithConfigureMinimalQuicHints`, with `quic_hints` naming `google.com` and `www.google.com` on port 443, under each protocol in each `chrome.txt`. So [the hints](#chrome-157) that [`native/network_session_configurator.cc`](../native/network_session_configurator.cc) states hold for Chrome 155 and 156 as well.

Five directories are few for a field trial as rare as `IncreaseAmplificationFactor`, so another rare one could still be there. The check on each session's HTTP/3 normal form stays.

## Chrome for Testing

Chrome for Testing keeps Windows builds of older Chrome Releases, so we checked whether it can stand in for stable Chrome. As it comes, it cannot. With two flags, it matches apart from one header. That makes it a usable fallback, but a Baseline from it holds a value we wrote ourselves, so stable Chrome stays the source as long as the update server has it.

The comparison is build 157.0.8103.39 against `tests/baselines/157`, recorded on 2026-11-06 with the procedure above. Sessions of each kind below gave identical normal forms. It says nothing about what the normal form [does not capture](capture.md#what-it-does-not-capture-yet), and builds of Chrome for Testing at 155 and 156 were not recorded.

### The build

- **Where it comes from.** `https://storage.googleapis.com/chrome-for-testing-public/VERSION/win64/chrome-win64.zip`. The bucket holds more than [its index](https://googlechromelabs.github.io/chrome-for-testing/known-good-versions-with-downloads.json) lists. On 2026-11-06 the index stopped at 155.0.8059.84, 156.0.8078.80 and 157.0.8103.38, and the bucket also had 155.0.8059.85, 156.0.8078.82 and 157.0.8103.39.
- **Whether it matches stable.** In version and source, yes: `chrome://version` gives both the same revision (`9a31f0ac`, `branch-heads/8103@{#1035}`) and V8. It is still a different build, without Google Chrome's branding. Build 157.0.8103.38 differs from it by one commit, the version number, and gave the same normal forms.
- **Whether it is signed.** No. `chrome.exe` and `chrome.dll` are `NotSigned`, so only the bucket's `x-goog-hash` header says a download arrived whole.

### Its variations seed and field trials

It fetches no seed by itself: only builds with Google Chrome's branding do, as `IsFetchingEnabled` in `components/variations/service/variations_service.cc` shows. In six minutes, a net log has no request for a seed, and `Local State` gets no `variations_country`. Stable Chrome asks within seconds.

It still has field trials. Builds without that branding apply the testing config in `testing/variations/fieldtrial_testing_config.json`, and `chrome://version` says `Variations Source: field-trial-config`. That is why [`record.ps1`](../tools/baselines/record.ps1) would find active variations even with no seed.

| Started with | Seed | Field trials | Normal form differs from stable in |
|---|---|---|---|
| The procedure's flags | None | The testing config, with experiments stable Chrome does not have | `sec-ch-ua`, extension `0x12e0`, `NOIP` |
| `--disable-field-trial-config` too | None | None | `sec-ch-ua` |
| `--variations-server-url=https://clientservices.googleapis.com/chrome-variations/seed --fake-variations-channel=stable` too | The same as stable's | Drawn from that seed | `sec-ch-ua` |

With those two flags it fetches the seed, and the testing config goes away. The seed is the one stable Chrome got, byte for byte. The second flag is there because Chrome for Testing has no channel, and studies filter on it. Without that flag the server sends another seed, and no network-related field trial applies. No filter in a study can see the branding. Its network-related field trials differ from stable's only as two stable user data directories differ from each other: the same seed, with other groups drawn. For example, `SafeBrowsingWaitForDnsForRealTimeLookup/Enabled_20260720` has a weight of 1 in 100 in it.

The two differences that come from the testing config, on top of `sec-ch-ua`:

| Field | Stable | Testing config | Study and feature |
|---|---|---|---|
| Extension `0x12e0` (`server_padding`), over all three protocols | Not sent | `{"type": "0x12e0", "data": "0000"}` | `PqcBandwidthExperiment`, `AddTLSServerHandshakePadding` |
| `google_connection_options`, over HTTP/3 | `["ORIG"]` | `["ORIG", "NOIP"]` | `IgnoreIPMatching`, `IgnoreIpMatching` |

Neither study nor feature is in stable's seed of that day. Sessions with no field trials and the four sessions with that seed also gave identical normal forms. None of those four drew the one field trial [that reaches the wire](#chrome-157).

### What needs correcting

Only `sec-ch-ua`, in every Request Kind over every protocol. That is 12 fields per Chrome Release:

| Chrome | `sec-ch-ua` |
|---|---|
| Chrome for Testing 157 | `"Chromium";v="157", "Not-A.Brand";v="99"` |
| Stable 157 | `"Not-A.Brand";v="99", "Google Chrome";v="157", "Chromium";v="157"` |
| Stable 156 | `"Not:A-Brand";v="8", "Chromium";v="156", "Google Chrome";v="156"` |
| Stable 155 | `"Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"` |

It lacks the brand, and adding the brand is not enough. `GenerateBrandVersionList` in `components/embedder_support/user_agent_utils.cc` shuffles the list by major version and by how many brands it holds, so take the value from real Chrome.

### If the fallback is needed

1. Repeat this comparison with the newest Chrome Release, where stable Chrome is still there to compare with. Anything but `sec-ch-ua` that differs comes back to the map as a decision.
2. Take the older build from the bucket, and record it with both flags. `-Chrome` names it and `-ExtraFlags` takes the flags, which is a list, so this needs `-Command` and not `-File`:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -Command "& tools\baselines\record.ps1 -Out recordings\a -UserDataDir $env:USERPROFILE\mitto-chrome\a -Chrome chrome-win64\chrome.exe -ExtraFlags '--variations-server-url=https://clientservices.googleapis.com/chrome-variations/seed','--fake-variations-channel=stable'"
   ```

3. Replace `sec-ch-ua` in the normal forms, and say so under that Chrome Release on this page.

For a session with no seed, `-NoVariationsSeed` takes the place of waiting for one. Whenever the Chrome is not Google Chrome, or either option is there, `chrome.txt` says that it describes a comparison and not a Baseline.
