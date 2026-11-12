# Run from any directory: powershell -ExecutionPolicy Bypass -File D:\dev\ImmersiveTrenchesMod\deploy.ps1
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$GameRoot = $env:GoHGameRoot,
    [string]$MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = 'E:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell'
}
$GameRoot = [IO.Path]::GetFullPath($GameRoot)
$modRoot = Join-Path $GameRoot 'mods\ImmersiveTrenches'
$sourceRoot = Join-Path $repoRoot 'mods\ImmersiveTrenches'

function Assert-GameClosed {
    $running = @(Get-Process call_to_arms, call_to_arms_ed, call_to_arms_server -ErrorAction SilentlyContinue)
    if ($running.Count) {
        $details = ($running | ForEach-Object { '{0} (PID {1})' -f $_.Name, $_.Id }) -join ', '
        throw "Close the game, editor and local server before deployment: $details"
    }
}

function Assert-SameFile([string]$Source, [string]$Destination) {
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash) {
        throw "Installed file differs from source: $Destination"
    }
}

function Invoke-BuildTarget([string]$Project, [string]$Target) {
    & $MSBuildPath $Project "/t:$Target" "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:GoHGameRoot=$GameRoot" '/v:minimal' '/nologo'
    if ($LASTEXITCODE -ne 0) { throw "MSBuild $Target failed (exit $LASTEXITCODE)." }
}

foreach ($relative in @('binaries\x64\call_to_arms.exe', 'binaries\x64\call_to_arms_ed.exe', 'binaries\x64\mss64.dll', 'binaries\x64\mss')) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameRoot $relative))) {
        throw "Game installation not found: $GameRoot (missing $relative)"
    }
}
Assert-GameClosed
if (-not $MSBuildPath) {
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) { $MSBuildPath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $candidates = @(& $vswhere -latest -prerelease -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')
            if ($LASTEXITCODE -eq 0 -and $candidates.Count) { $MSBuildPath = $candidates[0] }
        }
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
    throw 'MSBuild not found. Install VS/Build Tools with MSVC v145, or specify -MSBuildPath.'
}

Push-Location -LiteralPath $repoRoot
try {
    Write-Host "Building $Configuration|x64..."
    Invoke-BuildTarget (Join-Path $repoRoot 'ImmersiveTrenchesMod.sln') 'Build'
    Assert-GameClosed
    Invoke-BuildTarget (Join-Path $repoRoot 'ImmersiveTrenchesMod.vcxproj') 'Deploy'
    Invoke-BuildTarget (Join-Path $repoRoot 'ImmersiveTrenchesMod.vcxproj') 'DeployResources'

    $verified = 0
    foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
        $relative = $file.FullName.Substring($sourceRoot.Length + 1)
        Assert-SameFile $file.FullName (Join-Path $modRoot $relative)
        $verified++
    }
    Assert-SameFile (Join-Path $repoRoot "out\x64\$Configuration\ImmersiveTrenchesMod.asi") (Join-Path $GameRoot 'binaries\x64\mss\ImmersiveTrenchesMod.asi')
    Assert-SameFile (Join-Path $repoRoot "out\x64\$Configuration\ImmersiveTrenchesModLogic.dll") (Join-Path $modRoot 'ImmersiveTrenchesModLogic.dll')
    if (Test-Path -LiteralPath (Join-Path $modRoot 'ImmersiveTrenchesMod.ini')) {
        throw 'Obsolete ImmersiveTrenchesMod.ini was not removed.'
    }

    # DeployResources copies files without removing old names. Retire only these
    # known renamed folders, after verifying replacements and backing up originals.
    $dirt = 'resource\entity\landscape\cut_entity\immersiveTrenches\dirt'
    foreach ($pair in @(@('trench_dirt_fire_position1', 'trench_dirt_fire_position_through'), @('trench_dirt_fire_position2', 'trench_dirt_fire_position_dead_end'))) {
        $old = Join-Path (Join-Path $modRoot $dirt) $pair[0]
        if (-not (Test-Path -LiteralPath $old) -or (Test-Path -LiteralPath (Join-Path (Join-Path $sourceRoot $dirt) $pair[0]))) { continue }
        $replacement = Join-Path (Join-Path $modRoot $dirt) $pair[1]
        if (-not (Test-Path -LiteralPath $replacement -PathType Container)) { throw "Replacement folder missing: $replacement" }
        $resolved = (Resolve-Path -LiteralPath $old).Path
        $expected = [IO.Path]::GetFullPath($old)
        if ($resolved -ne $expected -or -not $resolved.StartsWith($modRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe obsolete path: $resolved" }
        $items = @(Get-Item -LiteralPath $resolved) + @(Get-ChildItem -LiteralPath $resolved -Recurse -Force)
        if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw "Refusing to remove linked folder: $resolved" }
        $backup = Join-Path $repoRoot ('out\deploy-backups\' + [guid]::NewGuid().ToString() + '\' + $pair[0])
        New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
        Copy-Item -LiteralPath $resolved -Destination $backup -Recurse
        foreach ($file in Get-ChildItem -LiteralPath $resolved -Recurse -File -Force) {
            Assert-SameFile $file.FullName (Join-Path $backup $file.FullName.Substring($resolved.Length + 1))
        }
        Assert-GameClosed
        Remove-Item -LiteralPath $resolved -Recurse -Force
        Write-Host "Retired $($pair[0]); backup: $backup"
    }
    # Remove disabled MG construction overrides after installing the replacement.
    foreach ($relative in Get-Content -LiteralPath (Join-Path $repoRoot 'deployment\retired-mg-files.txt')) {
        if ([string]::IsNullOrWhiteSpace($relative)) { continue }
        $target = [IO.Path]::GetFullPath((Join-Path $modRoot $relative))
        if (-not $target.StartsWith($modRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe retired path: $target" }
        if (Test-Path -LiteralPath (Join-Path $sourceRoot $relative)) { throw "Retired resource is active: $relative" }
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $backup = Join-Path $repoRoot ('out\deploy-backups\disabled-mg\' + $relative)
            New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
            Copy-Item -LiteralPath $target -Destination $backup -Force
            Assert-SameFile $target $backup
            Assert-GameClosed
            Remove-Item -LiteralPath $target -Force
        }
    }
    Write-Host "Deployed to: $GameRoot"
    Write-Host "Verified $($verified + 2) files. Restart the game to load this version."
}
finally { Pop-Location }
