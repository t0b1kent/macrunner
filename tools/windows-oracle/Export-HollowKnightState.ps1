[CmdletBinding()]
param(
    [string]$DestinationRoot = "\\Mac\Home\Desktop\MacRunner-Prism-Oracle",
    [string]$GameExecutable = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PayloadEntries {
    param([Parameter(Mandatory = $true)][string]$PayloadRoot)

    $resolvedRoot = (Resolve-Path -LiteralPath $PayloadRoot).Path.TrimEnd('\')
    $rootPrefix = $resolvedRoot + '\'
    $entries = @()
    foreach ($file in Get-ChildItem -LiteralPath $PayloadRoot -Force -Recurse -File | Sort-Object FullName) {
        if (-not $file.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Payload file escaped its root: $($file.FullName)"
        }
        $relative = $file.FullName.Substring($rootPrefix.Length).Replace('\', '/')
        $entries += [ordered]@{
            path = $relative
            size = [int64]$file.Length
            sha256 = Get-Sha256 -Path $file.FullName
        }
    }
    return @($entries)
}

function Test-BundlePayload {
    param([Parameter(Mandatory = $true)][string]$BundleRoot)

    $manifestPath = Join-Path $BundleRoot "PAYLOAD-MANIFEST.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Missing payload manifest: $manifestPath"
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $payloadRoot = Join-Path $BundleRoot "payload"
    $actualFiles = @(Get-ChildItem -LiteralPath $payloadRoot -Force -Recurse -File)
    if ($actualFiles.Count -ne [int]$manifest.file_count) {
        throw "Payload file count mismatch: expected $($manifest.file_count), found $($actualFiles.Count)"
    }

    foreach ($entry in @($manifest.files)) {
        $nativeRelative = ([string]$entry.path).Replace('/', '\')
        $filePath = Join-Path $payloadRoot $nativeRelative
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "Missing payload file: $($entry.path)"
        }

        $file = Get-Item -LiteralPath $filePath
        if ([int64]$file.Length -ne [int64]$entry.size) {
            throw "Payload size mismatch: $($entry.path)"
        }

        $actualHash = Get-Sha256 -Path $filePath
        if ($actualHash -ne [string]$entry.sha256) {
            throw "Payload SHA-256 mismatch: $($entry.path)"
        }
    }
}

function Find-HollowKnightExecutable {
    param([string]$ExplicitPath)

    $candidates = @()
    if ($ExplicitPath) {
        $candidates += $ExplicitPath
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\Hollow Knight\hollow_knight.exe")
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\Hollow Knight\Hollow Knight.exe")
    }
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles "Steam\steamapps\common\Hollow Knight\hollow_knight.exe")
        $candidates += (Join-Path $env:ProgramFiles "Steam\steamapps\common\Hollow Knight\Hollow Knight.exe")
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

$runningGame = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -match '^(Hollow Knight|hollow_knight)$'
})
if ($runningGame.Count -ne 0) {
    throw "Close Hollow Knight before exporting state. Running PID(s): $($runningGame.Id -join ', ')"
}

$saveSource = Join-Path $env:USERPROFILE "AppData\LocalLow\Team Cherry\Hollow Knight"
if (-not (Test-Path -LiteralPath $saveSource -PathType Container)) {
    throw "Hollow Knight LocalLow state directory is absent: $saveSource"
}

$registryKey = "HKCU\Software\Team Cherry\Hollow Knight"
& reg.exe query $registryKey *> $null
if ($LASTEXITCODE -ne 0) {
    throw "Hollow Knight PlayerPrefs registry key is absent: $registryKey"
}

if (-not (Test-Path -LiteralPath $DestinationRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
}

$runId = "hk-state-{0}" -f (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$stagingRoot = Join-Path $env:TEMP $runId
$incomingRoot = Join-Path $DestinationRoot (".incoming-" + $runId)
$finalRoot = Join-Path $DestinationRoot $runId

if ((Test-Path -LiteralPath $stagingRoot) -or
    (Test-Path -LiteralPath $incomingRoot) -or
    (Test-Path -LiteralPath $finalRoot)) {
    throw "Refusing to overwrite an existing export path for $runId"
}

try {
    $payloadRoot = Join-Path $stagingRoot "payload"
    $saveParent = Join-Path $payloadRoot "LocalLow\Team Cherry"
    New-Item -ItemType Directory -Path $saveParent -Force | Out-Null

    Copy-Item -LiteralPath $saveSource -Destination $saveParent -Recurse -Force

    $registryPath = Join-Path $payloadRoot "Hollow-Knight-HKCU.reg"
    & reg.exe export $registryKey $registryPath /y *> $null
    if (($LASTEXITCODE -ne 0) -or (-not (Test-Path -LiteralPath $registryPath -PathType Leaf))) {
        throw "Failed to export PlayerPrefs registry key: $registryKey"
    }

    $resolvedExecutable = Find-HollowKnightExecutable -ExplicitPath $GameExecutable
    $binaryIdentity = [ordered]@{
        executable_found = [bool]$resolvedExecutable
        executable_path = $resolvedExecutable
        executable_sha256 = $null
        unity_player_path = $null
        unity_player_sha256 = $null
    }
    if ($resolvedExecutable) {
        $binaryIdentity.executable_sha256 = Get-Sha256 -Path $resolvedExecutable
        $unityPlayer = Join-Path (Split-Path -Parent $resolvedExecutable) "UnityPlayer.dll"
        if (Test-Path -LiteralPath $unityPlayer -PathType Leaf) {
            $binaryIdentity.unity_player_path = $unityPlayer
            $binaryIdentity.unity_player_sha256 = Get-Sha256 -Path $unityPlayer
        }
    }

    $entries = @(Get-PayloadEntries -PayloadRoot $payloadRoot)
    if ($entries.Count -eq 0) {
        throw "Export payload is unexpectedly empty"
    }

    $totalBytes = [int64]0
    foreach ($entry in $entries) {
        $totalBytes += [int64]$entry.size
    }

    $payloadManifest = [ordered]@{
        schema = 1
        run_id = $runId
        file_count = $entries.Count
        total_bytes = $totalBytes
        files = $entries
    }
    $payloadManifestPath = Join-Path $stagingRoot "PAYLOAD-MANIFEST.json"
    Write-Utf8NoBom -Path $payloadManifestPath -Text (($payloadManifest | ConvertTo-Json -Depth 8) + "`n")

    $shaLines = foreach ($entry in $entries) {
        "{0}  payload/{1}" -f $entry.sha256, $entry.path
    }
    $shaPath = Join-Path $stagingRoot "SHA256SUMS"
    Write-Utf8NoBom -Path $shaPath -Text (($shaLines -join "`n") + "`n")

    $exportManifest = [ordered]@{
        schema = 1
        classification = "WINDOWS_ORACLE_STATE_EXPORT"
        run_id = $runId
        captured_utc = (Get-Date).ToUniversalTime().ToString("o")
        game_process_running = $false
        state_source = $saveSource
        registry_key = $registryKey
        payload_file_count = $entries.Count
        payload_total_bytes = $totalBytes
        payload_manifest_sha256 = Get-Sha256 -Path $payloadManifestPath
        sha256sums_sha256 = Get-Sha256 -Path $shaPath
        host = [ordered]@{
            computer_name = $env:COMPUTERNAME
            os_version = [Environment]::OSVersion.VersionString
            process_architecture = $env:PROCESSOR_ARCHITECTURE
        }
        game_binary = $binaryIdentity
    }
    $exportManifestPath = Join-Path $stagingRoot "EXPORT-MANIFEST.json"
    Write-Utf8NoBom -Path $exportManifestPath -Text (($exportManifest | ConvertTo-Json -Depth 8) + "`n")

    Test-BundlePayload -BundleRoot $stagingRoot

    New-Item -ItemType Directory -Path $incomingRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $stagingRoot '*') -Destination $incomingRoot -Recurse -Force
    Test-BundlePayload -BundleRoot $incomingRoot

    $incomingPayloadManifestHash = Get-Sha256 -Path (Join-Path $incomingRoot "PAYLOAD-MANIFEST.json")
    if ($incomingPayloadManifestHash -ne $exportManifest.payload_manifest_sha256) {
        throw "Payload manifest changed while copying to the Mac share"
    }

    $incomingSumsHash = Get-Sha256 -Path (Join-Path $incomingRoot "SHA256SUMS")
    if ($incomingSumsHash -ne $exportManifest.sha256sums_sha256) {
        throw "SHA256SUMS changed while copying to the Mac share"
    }

    $incomingManifestHash = Get-Sha256 -Path (Join-Path $incomingRoot "EXPORT-MANIFEST.json")
    $stagingManifestHash = Get-Sha256 -Path $exportManifestPath
    if ($incomingManifestHash -ne $stagingManifestHash) {
        throw "Export manifest changed while copying to the Mac share"
    }

    Rename-Item -LiteralPath $incomingRoot -NewName $runId
    Test-BundlePayload -BundleRoot $finalRoot
    Write-Host "READY_FOR_MAC_IMPORT=$finalRoot"
    Write-Host "FILES=$($entries.Count) BYTES=$totalBytes"
    Write-Host "EXPORT_MANIFEST_SHA256=$stagingManifestHash"
}
catch {
    if (Test-Path -LiteralPath $incomingRoot) {
        Remove-Item -LiteralPath $incomingRoot -Recurse -Force
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
