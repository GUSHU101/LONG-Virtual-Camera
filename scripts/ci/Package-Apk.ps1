param([ValidateSet('debug','release')][string]$Variant = 'debug')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$sdkRoot = $env:ANDROID_HOME
if (!$sdkRoot) { $sdkRoot = $env:ANDROID_SDK_ROOT }
if (!$sdkRoot) { throw 'Set ANDROID_HOME or ANDROID_SDK_ROOT' }
$buildTools = Join-Path $sdkRoot 'build-tools/36.0.0'
$signTool = Join-Path $buildTools $(if ($IsWindows) { 'apksigner.bat' } else { 'apksigner' })
$aapt = Join-Path $buildTools $(if ($IsWindows) { 'aapt2.exe' } else { 'aapt2' })
$apks = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot "app-control/build/outputs/apk/$Variant") -Filter '*.apk' -File)
if ($apks.Count -ne 1) { throw "Expected exactly one $Variant APK; got $($apks.Count)" }
$apk = $apks[0].FullName
$signature = & $signTool verify --verbose --print-certs $apk 2>&1
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed (unsigned release is not publishable)' }
$badging = & $aapt dump badging $apk 2>&1
if ($LASTEXITCODE -ne 0) { throw 'APK manifest inspection failed' }
$expectedId = 'io.github.longcontroller' + $(if ($Variant -eq 'debug') { '.debug' } else { '' })
if (($badging -join "`n") -notmatch [regex]::Escape("package: name='$expectedId'")) { throw 'Unexpected applicationId' }
$zip = [IO.Compression.ZipFile]::OpenRead($apk)
try {
    foreach ($name in @('assets/xposed_init', 'assets/long/payload-manifest.json', 'classes.dex')) {
        if (!$zip.GetEntry($name)) { throw "Missing APK entry: $name" }
    }
    $stream = [IO.StreamReader]::new($zip.GetEntry('assets/long/payload-manifest.json').Open())
    try { $manifest = $stream.ReadToEnd() | ConvertFrom-Json } finally { $stream.Dispose() }
    if ($manifest.targetProfile -ne 'redfin-UP1A.231105.001.B2' -or $manifest.crossAppBackendEnabled -ne $false) {
        throw 'Target profile or baseline backend gate is incorrect'
    }
    $entryStream = [IO.StreamReader]::new($zip.GetEntry('assets/xposed_init').Open())
    try {
        if ($entryStream.ReadToEnd().Trim() -ne 'io.github.longcontroller.xposed.LongCameraModule') { throw 'Wrong Xposed entrypoint' }
    } finally { $entryStream.Dispose() }
    # Parse each actual DEX class_def table, not arbitrary strings/references.
    $classes = [Collections.Generic.HashSet[string]]::new()
    foreach ($entry in $zip.Entries | Where-Object { $_.FullName -match '^classes\d*\.dex$' }) {
        $memory = [IO.MemoryStream]::new()
        $inputStream = $entry.Open()
        try { $inputStream.CopyTo($memory); $dex = $memory.ToArray() }
        finally { $inputStream.Dispose(); $memory.Dispose() }
        if ($dex.Length -lt 112 -or [Text.Encoding]::ASCII.GetString($dex,0,4) -ne "dex`n") { throw 'Invalid DEX header' }
        $stringCount = [BitConverter]::ToUInt32($dex,56)
        $stringOffset = [BitConverter]::ToUInt32($dex,60)
        $typeCount = [BitConverter]::ToUInt32($dex,64)
        $typeOffset = [BitConverter]::ToUInt32($dex,68)
        $classCount = [BitConverter]::ToUInt32($dex,96)
        $classOffset = [BitConverter]::ToUInt32($dex,100)
        for ($i = 0; $i -lt $classCount; $i++) {
            $typeIndex = [BitConverter]::ToUInt32($dex,($classOffset + $i * 32))
            if ($typeIndex -ge $typeCount) { throw 'Invalid DEX class index' }
            $stringIndex = [BitConverter]::ToUInt32($dex,($typeOffset + $typeIndex * 4))
            if ($stringIndex -ge $stringCount) { throw 'Invalid DEX descriptor index' }
            $position = [BitConverter]::ToUInt32($dex,($stringOffset + $stringIndex * 4))
            $lengthBytes = 0
            do {
                $lengthByte = $dex[$position]; $position++; $lengthBytes++
                if ($lengthBytes -gt 5) { throw 'Invalid DEX string length' }
            } while (($lengthByte -band 128) -ne 0)
            $end = $position
            while ($end -lt $dex.Length -and $dex[$end] -ne 0) { $end++ }
            if ($end -eq $dex.Length) { throw 'Unterminated DEX descriptor' }
            $descriptor = [Text.Encoding]::UTF8.GetString($dex,$position,($end - $position))
            if ($descriptor.StartsWith('Lde/robv/android/xposed/')) { throw 'Xposed API implementation must not be packaged' }
            [void]$classes.Add($descriptor)
        }
    }
    foreach ($class in @('MainActivity','MediaConfigProvider','NativeCameraProbeActivity','media/VideoFrameProbe','xposed/LongCameraModule')) {
        if (!$classes.Contains("Lio/github/longcontroller/$class;")) { throw "Required implementation absent from DEX: $class" }
    }
    if ($classes.Contains('Lio/github/longcontroller/xposed/Camera1Hooks;') -or $classes.Contains('Lio/github/longcontroller/xposed/Camera2Hooks;')) {
        throw 'Quarantined hook prototype found in baseline APK'
    }
} finally { $zip.Dispose() }
$destination = Join-Path $projectRoot "dist/$Variant"
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$finalApk = Join-Path $destination 'LONG-Controller.apk'
Copy-Item -LiteralPath $apk -Destination $finalApk -Force
$hash = (Get-FileHash -LiteralPath $finalApk -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  LONG-Controller.apk" | Set-Content -LiteralPath (Join-Path $destination 'SHA256SUMS.txt') -Encoding utf8NoBOM
@(
    'Artifact: Pixel 5 native-baseline controller; cross-app substitution disabled'
    "Variant: $Variant"
    "ApplicationId: $expectedId"
    'Target: redfin / Android 14 / API 34 / UP1A.231105.001.B2'
    'Packaging evidence: one APK, expected package/profile, actual DEX entrypoints, no bundled Xposed API classes, apksigner verification passed'
    'Unit/lint evidence: see separate Gradle reports; packaging does not run tests'
    'Device evidence: NOT_RUN'
    'Camera1 / Camera2 / CameraX / NDK / WebRTC virtual-input evidence: NOT_RUN'
    "GitHub SHA (empty for local builds): $env:GITHUB_SHA"
    $signature
) | Set-Content -LiteralPath (Join-Path $destination 'BUILD-EVIDENCE.txt') -Encoding utf8NoBOM
Write-Output "Verified artifact: $finalApk SHA256=$hash"
