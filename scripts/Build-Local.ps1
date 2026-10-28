# PowerShell 7+. Stage into an ASCII path to avoid Windows/JVM argument-file encoding bugs.
param([ValidateSet('debug','release')][string]$Variant = 'debug')
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$env:JAVA_HOME -or !(Test-Path -LiteralPath (Join-Path $env:JAVA_HOME 'bin/java.exe'))) {
    throw 'Set JAVA_HOME to a JDK 17 or 21 installation'
}
if (!$env:ANDROID_HOME) { throw 'Set ANDROID_HOME to the Android SDK directory' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('long-vcam-' + [guid]::NewGuid().ToString('N'))
if ($stage -match '[^\x00-\x7F]') { throw 'TEMP must use an ASCII path for this process' }
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($file in @('settings.gradle.kts','build.gradle.kts','gradle.properties','gradlew','gradlew.bat')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $stage
}
foreach ($folder in @('gradle','scripts')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $folder) -Destination $stage -Recurse
}
$app = New-Item -ItemType Directory -Path (Join-Path $stage 'app-control')
foreach ($file in @('build.gradle.kts','proguard-rules.pro')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "app-control/$file") -Destination $app.FullName
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'app-control/src') -Destination $app.FullName -Recurse
$taskVariant = (Get-Culture).TextInfo.ToTitleCase($Variant)
Write-Output "Build staging directory: $stage"
Push-Location $stage
try {
    & ./gradlew.bat --no-daemon --no-build-cache ":app-control:test${taskVariant}UnitTest" ":app-control:lint${taskVariant}" ":app-control:assemble${taskVariant}"
    if ($LASTEXITCODE -ne 0) { throw "Build or tests failed. Reports remain in $stage" }
    & ./scripts/ci/Package-Apk.ps1 -Variant $Variant
    $destination = Join-Path $projectRoot "dist/$Variant"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -Path (Join-Path $stage "dist/$Variant/*") -Destination $destination -Force
    Copy-Item -LiteralPath (Join-Path $stage 'app-control/build/reports') -Destination (Join-Path $destination 'reports') -Recurse -Force
    Write-Output "Verified build copied to $destination"
} finally {
    Pop-Location
    Write-Output "Staging directory retained for diagnostics: $stage"
}
