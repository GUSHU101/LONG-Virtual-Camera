# Read-only ADB preflight. Does not unlock, reboot, root, install or capture pixels.
param([string]$Serial, [string]$AdbPath = 'adb')
$ErrorActionPreference = 'Stop'
$adb = (Get-Command $AdbPath -ErrorAction Stop).Source
$devices = & $adb devices
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed' }
$ready = @($devices | Where-Object { $_ -match '^([^\s]+)\s+device\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
if (!$Serial) {
    if ($ready.Count -ne 1) { throw 'Connect exactly one authorized device or pass -Serial explicitly' }
    $Serial = $ready[0]
}
if ($Serial -notin $ready) { throw 'Selected device is not connected and authorized' }
function Read-Property([string]$Name) {
    $value = & $adb -s $Serial shell getprop $Name
    if ($LASTEXITCODE -ne 0) { throw "Unable to read $Name" }
    return ($value -join '').Trim()
}
$observed = [ordered]@{
    manufacturer = Read-Property 'ro.product.manufacturer'
    model = Read-Property 'ro.product.model'
    device = Read-Property 'ro.product.device'
    api = Read-Property 'ro.build.version.sdk'
    buildId = Read-Property 'ro.build.id'
    securityPatch = Read-Property 'ro.build.version.security_patch'
    buildType = Read-Property 'ro.build.type'
    buildTags = Read-Property 'ro.build.tags'
    abis = Read-Property 'ro.product.cpu.abilist'
    fingerprint = Read-Property 'ro.build.fingerprint'
    verifiedBootState = Read-Property 'ro.boot.verifiedbootstate'
    flashLocked = Read-Property 'ro.boot.flash.locked'
}
$expected = @{ manufacturer='Google';model='Pixel 5';device='redfin';api='34';buildId='UP1A.231105.001.B2';securityPatch='2023-11-05';buildType='user';buildTags='release-keys' }
$mismatches = @($expected.Keys | Sort-Object | Where-Object { $observed[$_] -cne $expected[$_] })
if ('arm64-v8a' -notin ($observed.abis -split ',')) { $mismatches += 'abis' }
[ordered]@{
    schema=1
    capturedUtc=[DateTime]::UtcNow.ToString('o')
    target='redfin-UP1A.231105.001.B2'
    propertyMatch=($mismatches.Count -eq 0)
    mismatchedFields=$mismatches
    observed=$observed
    evidence='CAPABILITY_ONLY'
    nativeFrames='NOT_RUN'
    virtualFrames='NOT_RUN'
    note='No serial, pixels or root credentials collected. Properties are not attestation.'
} | ConvertTo-Json -Depth 5
