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

Google's update server names the stable build of a Chrome Release and its installer. Ask it in PowerShell on the Windows machine, with the Chrome Release in place of `RELEASE` in `targetversionprefix`:

```powershell
$request = @"
<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" updaterversion="1.3.37.0" shell_version="1.3.37.0" ismachine="0" sessionid="{$([guid]::NewGuid())}" installsource="ondemand" requestid="{$([guid]::NewGuid())}" dedup="cr">
<hw physmemory="16" sse="1" sse2="1" sse3="1" ssse3="1" sse41="1" sse42="1" avx="1"/>
<os platform="win" version="10.0.26100.0" sp="" arch="x64"/>
<app appid="{8A69D345-D564-463C-AFF1-A69D9E530F96}" version="" nextversion="" ap="x64-stable-statsdef_1" lang="en" brand="GCEA" installage="-1"><updatecheck targetversionprefix="RELEASE."/></app>
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

To remove Chrome afterwards, with the build it has in place of `BUILD`:

```powershell
& "$env:LOCALAPPDATA\Google\Chrome\Application\BUILD\Installer\setup.exe" --uninstall --channel=stable --force-uninstall
```

### An older Chrome Release

The update server also answers for older Chrome Releases, so ask it the same way with an older one in `targetversionprefix`. A build that stopped serving is not in the release feed's list above. To check an older build, ask the feed for the builds of that Chrome Release that can be pinned, with the Chrome Release in place of `RELEASE` and the one after it in place of `NEXT`. The server's answer must be the newest of them:

```text
https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases?filter=version>=RELEASE,version<NEXT,pinnable=true
```

Download and check the installer as above, then unpack it instead of installing it. A user can only have one build installed, and this leaves that one as it is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\unpack.ps1 -Installer chrome_installer.exe -Out $env:USERPROFILE\mitto-chrome-bin\RELEASE
```

It needs no administrator and no updater can reach the result. Name `Chrome-bin\chrome.exe` in that directory with `-Chrome` when you record. It gets a variations seed like an installed Chrome, and [on the wire it is the same](#chrome-157).

Google does not say how long the server keeps answering. On 2026-11-06 it answered for every Chrome Release from 145 to 157, and not for 144 or older. So keep the installer until the Chrome Release leaves the **Supported Window**.

### Which build

Record the build that the server answers for the pin, with the Chrome Release in `targetversionprefix`. That is the newest build of the Chrome Release that the release feed marks as `pinnable`. Do not take the newest build that Chromium Dash or the feed's full list shows. Those lists also hold builds that Google gives only to a share of installations, and the server does not give them for a pin.

On 2026-11-07 the server answered this, which kept the builds the Baselines already came from:

| `targetversionprefix` | Answer |
|---|---|
| `155.` | 155.0.8059.85 |
| `156.` | 156.0.8078.80 |
| `157.` | 157.0.8103.39 |
| `155.0.8059.86`, `156.0.8078.81` or `156.0.8078.82` | `noupdate` |

The feed gives each build its share of a fraction group, and only shares in the same group compare. That day there were two groups. One holds the pinnable builds, and 157.0.8103.39 had all of it. In the other, none pinnable, 156.0.8078.80 had 49.5%, 156.0.8078.81 and 157.0.8103.38 24.75% each, and 156.0.8078.82 and 158.0.8127.18 0.5% each. 155.0.8059.86 had 10 to 25% of that group from 2026-10-20 to 2026-10-26, against 49.5 to 80% for 155.0.8059.85, and none since. So the pinned build of each older Chrome Release was also the one with the largest share, and the newer builds were rollouts to a few installations.

## The variations seed and the country

Chrome fetches a variations seed, and field trials from it apply from the next start. Without it, Chrome looks fine and runs with no field trials, so a recording session waits for the seed, restarts Chrome and checks that `chrome://version` lists active variations.

Studies in a seed can filter by country, so every recording of a Chrome Release uses one fixed country, including later comparisons with it. That country is the US, to go with the `en-US` locale, and `--variations-override-country=us` on every start of Chrome fixes it. Chrome uses that flag instead of what it stored for both session and permanent consistency. Do not leave it to the network's exit: a seed response only tells Chrome the exit's country in its `X-Country` header, and `variations_country` in `Local State` holds that. The seed's bytes were the same through every exit we tried, so that header is the only way the exit reaches Chrome, and the flag replaces it. Which country `Local State` shows only depends on the network, so do not write the value down.

`variations_country` does not show that the seed arrived either, because a seed can come without it. `variations_seed_serial_number` in `Local State` does. Chrome 157 writes the serial number once it has stored a seed whose signature holds (`VariationsSeedStore::StoreValidatedSeed` in `components/variations/variations_seed_store.cc`), and the country only when the seed came over HTTPS with an `X-Country` header (`VariationsService::OnSimpleLoaderComplete` in `components/variations/service/variations_service.cc`). On 2026-11-07, 9 of 54 new user data directories of Chrome 155, 156 and 157 had a seed and its serial number without `variations_country`, after Chrome had run from them. Every one of the 54 had a serial number. The ones of Chrome 157 without a country had `variations_seed_date` a few seconds before their fetch time, and Chrome 157 only takes that date from a response over HTTPS, so their seeds did not come over its fallback to plain HTTP. When a response leaves `X-Country` out is not known. A session of Chrome 157 whose seed came without it listed active variations, and its normal forms were byte for byte the Baselines.

To see that the flag works, read the field trials in `chrome://version/?show-variations-cmd` with and without it. With Chrome 157 and the seed of 2026-11-05, `ContextualTasksDesktop` filters on the US and `LensSidePanelUnification` on everywhere but the US, each with one group at 100%. With the flag the first is there and the second is not. Without it, from an exit elsewhere, it is the other way round.

Capture traffic stays out of all this. Chrome resolves `*.localhost` itself and never sends it through a proxy. The [case pages](capture.md#recording-cases) are at names that are not local, so their starts of Chrome have `--no-proxy-server`, and `--host-resolver-rules` sends those names to loopback with no DNS. The capture server only listens on loopback, and its own proxy refuses a tunnel to any name but its own, so nothing that reaches it leaves the machine. The raw recording holds each client's address as `peer`, so that this can be checked.

`chrome.txt` shows what one session drew, and only names that look network-related. The seed holds every study. To read it, take `VariationsSafeSeedV2` from the user data directory: zstd over a protocol buffer whose field 1 is the `VariationsSeed` of `components/variations/proto/variations_seed.proto`, with the serial number that `chrome.txt` names. A directory that Chrome has started from only once has the seed in `VariationsSeedV2` instead, and a `VariationsSafeSeedV2` of a few bytes. Once the safe seed holds it, `VariationsSeedV2` only points there. In the seed of 2026-11-05, no study names a feature that decides the ClientHello over TCP.

### Features on in Chrome's own code

`PartitionConnectionsByNetworkIsolationKey` and `SplitCacheByNetworkIsolationKey` are off in `//net` and on in Chrome, with no seed involved. `ChromeMainDelegate::CommonEarlyInitialization` in `chrome/app/chrome_main_delegate.cc` calls `NetworkAnonymizationKey::PartitionByDefault()` and `HttpCache::SplitCacheFeatureEnableByDefault()`, in 155.0.8059.85, 156.0.8078.80 and 157.0.8103.39 alike. These turn the features on unless the command line or a field trial overrides them.

Neither name is in any seed we read: those of 2026-11-05 and 2026-11-06, for all three Chrome Releases. A search of the decompressed seed finds no study or feature with `NetworkIsolationKey`, `NetworkAnonymizationKey`, `PartitionConnections` or `SplitCache` in its name, while `TryQuicByDefault` is there.

`chrome://version` lists overrides and not what is on by default, so `$NetworkNames`, which matches both names, has nothing to find there. That is why `chrome.txt` has a line for each under each run, saying `not overridden` or naming the switch that overrides it. A session started with `--enable-features=PartitionConnectionsByNetworkIsolationKey --disable-features=SplitCacheByNetworkIsolationKey` shows `--enable-features` and `--disable-features` on those lines, and both names in those lists.

Partitioning shows on the wire, although [not in a normal form](capture.md#what-it-does-not-capture-yet). Over HTTP/2 and HTTP/3, `navigation_link_cross_site` opens a connection of its own, although the certificate of an open connection to the same address covers its name. With `--disable-features=PartitionConnectionsByNetworkIsolationKey` it goes over the same connection as the navigations before it. The normal forms of those two sessions are identical in all nine files. The `connections` in a raw recording show the difference.

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

It takes about a minute and a half, and ends with `Done:` or with an error. Windows of Chrome open and close on the desktop meanwhile. If a case page fails, the reason is in `server-errors.log` in the output directory. A line there that ends in `bytes that did not wait for an answer to CONNECT` is no failure: during the starts through the proxy, Chrome's component updater sends its `POST` to `update.googleapis.com` over plain HTTP through the proxy, and the proxy drops it.

### What it does

[`tools/baselines/record.ps1`](../tools/baselines/record.ps1) does the following. At the machine you can do the same by hand, with `chrome` being `%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe`:

1. It starts Chrome with the first five flags below and `about:blank`, waits until `Local State` has [`variations_seed_serial_number` or `variations_country`](#the-variations-seed-and-the-country), about 15 seconds, and closes Chrome through its window. It gives up if neither comes within six minutes. Do not kill it: Chrome counts that as a crash, and a crash streak makes it give up its seed.
2. It starts the capture server, `uv run python -m tools.capture --out recordings\a --click-in USER_DATA_DIRECTORY`, which writes the address and flags of [each run](capture.md#recording-cases) to `runs.json`.
3. For each run, it starts Chrome again with its flags and its address, in the order of that list. The server clicks what the case pages ask to have clicked, through the debugging port. The script waits until the server has written that run's files, when the page says "Recorded". It reads `chrome://version/?show-variations-cmd` in a new tab of that same Chrome, and closes Chrome. The server exits after the last.
4. It checks that every client's address is loopback, that `Accept-Language` is `en-US`, that Chrome lists active variations, and that Chrome's version did not change.
5. It writes `chrome.txt`: the product and its build, the system, the flags, the seed's serial number and, for each run, the network-related part of that page. That part is the field trials and features whose names have to do with connections, protocols, DNS, headers or priorities. The script's `$NetworkNames` is the exact list. Each run also gets a line for each of [the two features Chrome turns on in its own code](#features-on-in-chromes-own-code).

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
   grep -n -i -E 'NAME|[a-z]:\\|/users/|[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:' recordings/a/http?.json recordings/a/cases/{http?,plain_http,proxy_http?,client_hints_*}.json recordings/a/chrome.txt
   ```

3. On the Windows machine, check that each value the normal form hides as `<machine>` has [the form a Session's has](capture.md#the-form-of-a-hidden-value). Each directory should say `pass`. It prints no value, but the raw recordings it reads hold this machine's own, so they stay on it:

   ```powershell
   uv run python -m tools.capture.machine_forms recordings\a recordings\b
   ```

4. Copy `http1.json`, `http2.json`, `http3.json` and `chrome.txt` of the first session to `tests/baselines/RELEASE/`, and the normal forms in its `cases` directory to `tests/baselines/RELEASE/cases/`. Leave out `runs.json`, the raw recordings and the logs, which are not Baselines.
5. Write the Persona's file again, because `sec-ch-ua-full-version` and `sec-ch-ua-full-version-list` name the build. See [persona.md](persona.md#write-one).
6. Regenerate the known-gaps list on a Mac or Linux **Host**, and read its diff. See [parity.md](parity.md#the-known-gaps-list).

## Where each Chrome Release comes from

| Chrome Release | Source | Build | SHA-256 of the installer | Corrections |
|---|---|---|---|---|
| 155 | [Google's update server](#an-older-chrome-release), unpacked | 155.0.8059.85 | `2a9b18526096b614fbfda4324280fc69316e42dcb0668301f2cd8a4524054664` | None |
| 156 | [Google's update server](#an-older-chrome-release), unpacked | 156.0.8078.80 | `43cca215206b634b89d226ed71522c7f58c5430b8559b92e782dd7bd77add7cc` | None |
| 157 | [Google's update server](#install-chrome), installed | 157.0.8103.39 | `389b9e21de9a520a6e24ba807ee8760409ccc837457bbcc460d2fdce0d5e8df5` | None |

All of them are stable Chrome signed by Google, so what they send needs no correcting. Each is [the build to record](#which-build): the newest build of its Chrome Release that the release feed marks as `pinnable`, which is what the server answers with. The 155 and 156 builds were checked on 2026-11-06 for hash, signature and version, and [recorded](#chrome-155-and-156) that day. On 2026-11-07 the server still named all three builds and these installers, with the hashes above, and [sessions with that day's seed](#the-seed-of-2026-11-07) gave the Baselines byte for byte.

A recording of an older Chrome Release gets the seed of the day it is recorded, as every installation of that Chrome Release does on that day. Each Chrome Release gets [its own cut](#their-variations-seeds) of that seed.

[Chrome for Testing](#chrome-for-testing) is only the fallback, for a Chrome Release the server no longer answers for.

## Chrome 157

Build 157.0.8103.39, recorded on 2026-11-05 and 2026-11-06 on Windows 11 build 26300, with a Windows display language other than English. `http1.json`, `http2.json` and `http3.json` in `tests/baselines/157` come from the first session. The [Recording Cases](#the-recording-cases) came a day later.

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

Builds 155.0.8059.85 and 156.0.8078.80, [unpacked](#an-older-chrome-release), recorded on 2026-11-06 on the same Windows machine as Chrome 157. `http1.json`, `http2.json` and `http3.json` in `tests/baselines/155` and `tests/baselines/156` come from the first session of each. The [Recording Cases](#the-recording-cases) came later that day.

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

The serial number in `chrome.txt` can differ between sessions of one Chrome Release, in the part before the `#`. The studies stay the same when it does. The `chrome.txt` files in the repository now come from [a later seed](#their-seed).

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

## The Recording Cases

`tests/baselines/RELEASE/cases` comes from the first of three sessions of each Chrome Release, recorded on 2026-11-06 through `record.ps1` with [the same builds](#where-each-chrome-release-comes-from), on the same Windows machine, windowed.

The three sessions of each Chrome Release gave identical normal forms in all nine files, and none drew [the field trial that reaches the wire](#chrome-157). In each, `http1.json`, `http2.json` and `http3.json` came out byte for byte the same as the Baselines already there, so neither the extended kit nor a newer seed moved them. The checks in [`tests/capture/test_chrome.py`](../tests/capture/test_chrome.py) that only read a recording hold for all nine sessions, so each case is what it says it is in windowed Chrome too.

A connection looks the same in every file of a protocol. `tls`, `quic`, `http2` and `http3` are the same in a main file, in its file in `cases` and, over HTTP/1.1 and HTTP/2, in the file through the proxy. So the cases differ from each other in their requests only.

### What differs between Chrome Releases

Only `sec-ch-ua` and `User-Agent`, with [the values above](#what-differs-from-chrome-157). Over the six files in `cases` from that day that is 76 fields of `sec-ch-ua` and 82 of `User-Agent` per Chrome Release. Plain HTTP and a `CONNECT` have no `sec-ch-ua`, so they differ in `User-Agent` alone.

### Their seed

Version `20261106-010110.492000-production`, a day newer than [the one above](#their-variations-seeds), with 630, 618 and 584 studies for Chrome 155, 156 and 157. All sessions of a Chrome Release got the same bytes. `IncreaseHttp3Usage` is the same group as before in all of them.

Field trials differ between sessions as they did before, with no effect on the normal form. These are new in that list: `PrioritizeResizeTaskRunnerOnStartupStudy` and `WinSboxCsrssLockdown` between user data directories and `CacheGurlSchemeIsHttpOrHttpsResult` between starts in Chrome 155, `SearchPrefetchHighPriorityPrefetches` between user data directories in Chrome 156, and `PreconnectManagerDirectFastPath` between starts in Chrome 157.

### Client hints

`cases/client_hints_http1.json`, `cases/client_hints_http2.json`, `cases/client_hints_http3.json`, `cases/client_hints_plain_http.json` and `chrome.txt` come from the first of three later sessions of each Chrome Release, recorded on 2026-11-07, on the same machine with the same builds and the same seed.

The three sessions of each Chrome Release gave identical normal forms in all thirteen files, and none drew [the field trial that reaches the wire](#chrome-157). The nine files from before came out byte for byte the same in all nine sessions, so neither these runs nor another day moved them. The checks in [`tests/capture/test_chrome.py`](../tests/capture/test_chrome.py) that only read a recording hold for these cases in each Chrome Release. A connection looks the same here as in the main file of its protocol, so these cases too differ from the others in their requests only.

The values that [follow the machine](capture.md#what-stays-and-what-does-not) are `<machine>` in these files, and they differ from what [a Session claims](persona.md#what-they-say) on purpose. `sec-ch-ua-platform-version` is too, since [it was hidden](#the-platform-version-hidden) on 2026-11-08.

Between Chrome Releases these files differ only in `sec-ch-ua`, `User-Agent`, `sec-ch-ua-full-version` and `sec-ch-ua-full-version-list`:

| Chrome | `sec-ch-ua-full-version` | `sec-ch-ua-full-version-list` |
|---|---|---|
| 155 | `"155.0.8059.85"` | `"Google Chrome";v="155.0.8059.85", "Chromium";v="155.0.8059.85", "Not(A:Brand";v="24.0.0.0"` |
| 156 | `"156.0.8078.80"` | `"Not:A-Brand";v="8.0.0.0", "Chromium";v="156.0.8078.80", "Google Chrome";v="156.0.8078.80"` |
| 157 | `"157.0.8103.39"` | `"Not-A.Brand";v="99.0.0.0", "Google Chrome";v="157.0.8103.39", "Chromium";v="157.0.8103.39"` |

Over the four files that is 123 fields of `sec-ch-ua`, 125 of `User-Agent` and 42 each of the other two per Chrome Release.

The seed's version is the same as [the day before](#their-seed), and so is the part of its serial number after the `#`, so the studies have not changed. The part before the `#` has grown. `OptimisticDnsForTcp` is in the `chrome.txt` of some user data directories and in none of the starts of others, which goes beyond [what Chrome 157 showed](#chrome-157), and the normal form is the same in both.

### The platform version hidden

On 2026-11-08 `sec-ch-ua-platform-version` joined the hints that the normal form hides, because each Session claims [a Windows of its own](persona.md#which-windows). The Baselines were written again with that one value changed: `"19.0.0"` became `<machine>` 17 times in each of `cases/client_hints_http1.json`, `cases/client_hints_http2.json` and `cases/client_hints_http3.json`, of each Chrome Release, and nothing else changed. On the Windows machine, the normal forms of the first session of each Chrome Release that recorded [the client hints](#client-hints), with that line hidden the same way, are byte for byte the Baselines in all thirteen files.

[The check of hidden values](capture.md#the-form-of-a-hidden-value) passed on the raw recordings of all three of those sessions of each Chrome Release, and of the three of [the seed of 2026-11-07](#the-seed-of-2026-11-07): 600 values in 13 raw recordings for each, and every one of them in its form.

## The seed of 2026-11-07

Later on 2026-11-07, three more sessions of each Chrome Release were recorded through `record.ps1`, with [the same builds](#where-each-chrome-release-comes-from) on the same machine. Each session recorded Chrome 155, 156 and 157 in turn, so Chrome 157 checks each session's seed. All nine gave normal forms byte for byte the same as `tests/baselines` in all thirteen files, so the Baselines stay as they are, `chrome.txt` included, and so do the Personas. None of them drew [the field trial that reaches the wire](#chrome-157).

Their seed had the version `20261106-170047.808000-production`, with 635, 623 and 589 studies for Chrome 155, 156 and 157. One session of Chrome 156 got `20261107-030038.770000-production` instead, with 626 studies. One of Chrome 157 got a seed of the same version with another part after the `#` and 591 studies. Field trials differed between sessions as before, with no effect on the normal form. New in that list are `CacheGurlSchemeIsHttpOrHttpsResult`, `HttpCacheZstdCompression`, `PreconnectManagerDirectFastPath` and `VariationsFeatureAccessLogging` in Chrome 156, and `SearchPrefetchHighPriorityPrefetches` and `WinSboxCsrssLockdown` in Chrome 157.

### Merkle Tree Certificates

No study in any of those seeds names `VerifyMTCs`, or anything with `MTC` or `Merkle` in its name, for any of the three Chrome Releases. So the feature stays at its default, which is off in 157. The normal forms did not change, so `trust_anchors` names the same 28 IDs as before in every file with TLS, and no Merkle Tree CA was added. [capture.md](capture.md#what-this-does-not-cover) says what would change if it were.

### The Platform Runtime component

None of the three has it. `chrome://components` listed no Platform Runtime from new user data directories after 30 seconds, about 7 minutes and 16 minutes. It listed none after a later start either, once the seed's field trials applied. No `PlatformRuntime` directory appeared.

The seeds do have a study for it, `PlatformRuntimeComponentStudy`, the same in all of them. Its groups turn the feature `EnablePlatformRuntimeComponent` on for 1% of user data directories and off for another 1%. They leave the other 98% at the feature's default, which is off. The study only applies to stable Chrome on Windows from 157.0.8079.0, so Chrome 155 and 156 never draw it, and our Chrome 157 drew the default group.

Chrome 157's source has two more conditions, in `MaybeRegisterPlatformRuntimeComponent` in `chrome/browser/component_updater/platform_runtime_component_installer.cc`. The browser must be Google Chrome on Windows. Its `chrome.exe` must also run from the directory that the registry names for the installation, so an [unpacked](#an-older-chrome-release) Chrome never registers the component.

It does not change the wire of a Baseline. `RequestHeaderIntegrityURLLoaderThrottle` hands every request to `PlatformRuntimeHeaders::Apply`, which adds no line. It only replaces the value of a line that the request already has, among those Chrome adds outside CORS, when the component's library returned a value under that name. Those are the lines that the same throttle adds for [Google's domains](capture.md#what-chrome-adds-for-googles-domains) alone, and the names of the case pages are not Google's. A Session sends none of those lines anyway. In the 1% of installed Chrome 157 that do have the component, it can change their values on Google's domains. We did not measure that: forcing the feature on would install the component into the directory of the installed Chrome that the Baselines come from.

## A Chrome that has run for a while

A Baseline comes from a new user data directory, so it cannot show what a directory gains over time. [capture.md](capture.md#what-can-differ-between-chrome-installations) has the comparison for the trust anchors. To repeat it:

1. Start Chrome from a new directory with [the first five flags](#what-it-does), and leave it until `PKIMetadata\VERSION\crs.pb` is in that directory. That took 335 seconds on 2026-11-06. Close it through its window.
2. Record from the same directory, which takes about 4 minutes:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\baselines\record.ps1 -Out recordings\settled -UserDataDir $env:USERPROFILE\mitto-chrome\a -ExistingUserDataDir -SettleSeconds 20
   ```

3. Compare the normal forms with the Baselines, as in [Check and commit](#check-and-commit). What this records is a comparison, `chrome.txt` says so, and it does not go under `tests/baselines`.

`-ExistingUserDataDir` takes a directory that holds a variations seed. `-SettleSeconds` lets each start of Chrome run on an empty tab before the page opens. Chrome loads its components about two seconds after a start, and without the wait a recording is over by then, so it shows the compiled-in root store.

To read a list without recording, `crs.pb` is a `RootStore` from `net/cert/root_store.proto`. Chrome sends the `trust_anchor_id` of each entry in `trust_anchors`, and of each entry in `additional_certs` that has `tls_trust_anchor`.

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

`-Tickets` is for another comparison: it has the capture server give out session tickets, and takes Chrome through [the runs that resume](capture.md#how-the-server-does-it).

For a session with no seed, `-NoVariationsSeed` takes the place of waiting for one. Whenever the Chrome is not Google Chrome, or `-NoVariationsSeed`, `-ExtraFlags`, `-ExistingUserDataDir`, `-SettleSeconds` or `-Tickets` is there, `chrome.txt` says that it describes a comparison and not a Baseline.
