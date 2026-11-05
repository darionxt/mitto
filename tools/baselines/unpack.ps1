<#
.SYNOPSIS
    Unpacks Chrome from its installer into a directory, without installing it. docs/baselines.md explains when.

.DESCRIPTION
    The installer holds a 7z archive with Chrome-bin in it, which is what installing Chrome would put in place. This
    runs nothing from the installer, writes only to the new directory and leaves an installed Chrome alone. It checks
    that Google signed the installer and the Chrome that comes out of it.

.PARAMETER Installer
    The installer that Google's update server named.

.PARAMETER Out
    Where Chrome goes, as Out\Chrome-bin\chrome.exe. It must not exist yet.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$Out
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# A 7z archive starts with this signature. Its header of 32 bytes says where the archive's last part is and how long.
$ArchiveSignature = [byte[]](0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C)
$ArchiveHeaderSize = 32
$NextPartOffsetAt = 12
$NextPartSizeAt = 20
# Smaller ones are not Chrome: its archive takes hundreds of megabytes.
$SmallestArchive = 100MB

function Assert-SignedByGoogle([string]$Path) {
    $signature = Get-AuthenticodeSignature $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike 'CN=Google LLC*') {
        throw "$Path does not have a valid signature from Google: $($signature.Status)"
    }
}

function Test-ArchiveSignature([byte[]]$Bytes, [int]$At) {
    for ($i = 0; $i -lt $ArchiveSignature.Length; $i++) {
        if ($Bytes[$At + $i] -ne $ArchiveSignature[$i]) { return $false }
    }
    return $true
}

function Find-Archive([byte[]]$Bytes) {
    $at = -1
    while ($true) {
        $at = [Array]::IndexOf($Bytes, $ArchiveSignature[0], $at + 1)
        if ($at -lt 0 -or $at + $ArchiveHeaderSize -gt $Bytes.Length) { throw 'The installer holds no archive of Chrome.' }
        if (-not (Test-ArchiveSignature $Bytes $at)) { continue }
        $offset = [decimal][BitConverter]::ToUInt64($Bytes, $at + $NextPartOffsetAt)
        $size = [decimal][BitConverter]::ToUInt64($Bytes, $at + $NextPartSizeAt)
        $length = $ArchiveHeaderSize + $offset + $size
        if ($length -ge $SmallestArchive -and $at + $length -le $Bytes.Length) {
            return @{ Offset = $at; Length = [int]$length }
        }
    }
}

if (Test-Path $Out) { throw "$Out exists." }
$Installer = (Resolve-Path $Installer).Path
Assert-SignedByGoogle $Installer
New-Item -ItemType Directory -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$bytes = [IO.File]::ReadAllBytes($Installer)
$found = Find-Archive $bytes
$archive = Join-Path $Out 'chrome.7z'
$stream = [IO.File]::Create($archive)
try {
    $stream.Write($bytes, $found.Offset, $found.Length)
} finally {
    $stream.Dispose()
}
$bytes = $null

& tar.exe -xf $archive -C $Out
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE." }
Remove-Item $archive

$chrome = Join-Path $Out 'Chrome-bin\chrome.exe'
if (-not (Test-Path $chrome)) { throw "The archive held no $chrome." }
Assert-SignedByGoogle $chrome
Write-Output "$((Get-Item $chrome).VersionInfo.ProductName) $((Get-Item $chrome).VersionInfo.ProductVersion): $chrome"
