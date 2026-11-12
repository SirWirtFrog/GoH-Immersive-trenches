param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$DestinationRoot,
    [string]$BackupRoot,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\')
$destination = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\')
if (!(Test-Path -LiteralPath (Join-Path $source 'mod.info'))) { throw 'Source mod.info not found' }
if ($source.Equals($destination,[StringComparison]::OrdinalIgnoreCase) -or
    $destination.StartsWith($source+'\',[StringComparison]::OrdinalIgnoreCase) -or
    $source.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Source and destination overlap' }
$entityPath = 'resource\entity\landscape\cut_entity\IT\dirt'
$sourceEntities = Join-Path $source $entityPath
$destinationEntities = Join-Path $destination $entityPath
$definitions = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -LiteralPath $sourceEntities -Recurse -File -Filter '*.def' | ForEach-Object { [void]$definitions.Add($_.Name) }
$retired = @()
if (Test-Path -LiteralPath $destinationEntities) {
    foreach ($folder in Get-ChildItem -LiteralPath $destinationEntities -Directory -Filter 'trench_*') {
        if (Test-Path -LiteralPath (Join-Path $sourceEntities $folder.Name)) { continue }
        $canonical = if ($folder.Name.StartsWith('trench_dirt_')) { $folder.Name -replace '^trench_dirt_', 'trench_' } else { $folder.Name -replace '^trench_', 'trench_dirt_' }
        $normalized = $folder.Name -replace '-', '_'
        $defs = @(Get-ChildItem -LiteralPath $folder.FullName -Recurse -File -Filter '*.def')
        $duplicates = $defs.Count -gt 0 -and @($defs | Where-Object { !$definitions.Contains($_.Name) }).Count -eq 0
        if ((Test-Path -LiteralPath (Join-Path $sourceEntities $canonical)) -or (Test-Path -LiteralPath (Join-Path $sourceEntities $normalized)) -or $duplicates) {
            $resolved = [IO.Path]::GetFullPath($folder.FullName)
            if (!$resolved.StartsWith($destinationEntities+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Legacy target leaves entity directory' }
            if ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked legacy directory is not supported: $resolved" }
            $retired += $resolved
        }
    }
}
# Back up the mod's previous namespace to prevent duplicate entity registrations.
$legacyEntities = [IO.Path]::GetFullPath((Join-Path $destination 'resource\entity\landscape\cut_entity\immersiveTrenches\dirt'))
if (!$legacyEntities.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Legacy target leaves destination' }
if (Test-Path -LiteralPath $legacyEntities) {
    if ((Get-Item -LiteralPath $legacyEntities).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked legacy namespace is not supported' }
    foreach ($folder in Get-ChildItem -LiteralPath $legacyEntities -Directory -Filter 'trench_*') {
        if ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked legacy directory is not supported' }
        $retired += $folder.FullName
    }
}
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File)
if ($PlanOnly) {
    [pscustomobject]@{source=$source; destination=$destination; files=$files.Count; retired=$retired} | ConvertTo-Json -Depth 3
    exit 0
}
# Do not replace resources while this installation's game/editor has cached them.
$modsDirectory = Split-Path $destination -Parent
if ((Split-Path $modsDirectory -Leaf) -eq 'mods') {
    $gameDirectory = Split-Path $modsDirectory -Parent
    foreach ($process in Get-Process -Name call_to_arms,call_to_arms_ed -ErrorAction SilentlyContinue) {
        if (!$process.Path -or $process.Path.StartsWith($gameDirectory+'\',[StringComparison]::OrdinalIgnoreCase)) {
            throw "Close game/editor before DeployResources (PID $($process.Id))"
        }
    }
}
$backup = $null
if ($retired.Count -gt 0) {
    $backupBase = if ($BackupRoot) { [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\') } else { Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'ImmersiveTrenches\resource-backups' }
    if ($backupBase.Equals($destination,[StringComparison]::OrdinalIgnoreCase) -or $backupBase.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Backups must be outside the deployed mod' }
    $backup = Join-Path $backupBase ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    [pscustomobject]@{destination=$destination; retired=$retired} | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8
    foreach ($old in $retired) {
        $relative = $old.Substring($destination.Length+1)
        $target = [IO.Path]::GetFullPath((Join-Path $backup $relative))
        if (!$target.StartsWith($backup+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Backup target leaves backup directory' }
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Move-Item -LiteralPath $old -Destination $target
    }
}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($source.Length+1)
    $target = [IO.Path]::GetFullPath((Join-Path $destination $relative))
    if (!$target.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Resource target leaves destination' }
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}
Write-Host "Deployed $($files.Count) resources to $destination"
if ($backup) { Write-Host "Retired resource directories backed up to $backup" }
