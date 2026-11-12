<#
.SYNOPSIS
Remove installed Immersive Trenches files. Sources and external backups are retained.
.EXAMPLE
.\uninstall.ps1 -WhatIf
.EXAMPLE
.\uninstall.ps1 -GameRoot 'D:\Games\Gates of Hell'
#>
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param([string]$GameRoot = $env:GoHGameRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = 'E:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell'
}
$gameDirectory = [IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$repository = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
foreach ($marker in @('binaries\x64\call_to_arms.exe','binaries\x64\mss64.dll','binaries\x64\mss')) {
    if (!(Test-Path -LiteralPath (Join-Path $gameDirectory $marker))) {
        throw "Game installation not found: $gameDirectory (missing $marker)"
    }
}

# Fixed allowlist: never enumerate another mod or delete the shared Miles folder.
$targets = @(
    (Join-Path $gameDirectory 'binaries\x64\mss\ImmersiveTrenchesMod.asi'),
    (Join-Path $gameDirectory 'binaries\x64\mss\ImmersiveTrenchesMod.pdb'),
    (Join-Path $gameDirectory 'mods\ImmersiveTrenches')
)
function Assert-SafeTarget([string]$Target) {
    $absolute = [IO.Path]::GetFullPath($Target)
    if (!$absolute.StartsWith($gameDirectory+'\',[StringComparison]::OrdinalIgnoreCase) -or
        $absolute.StartsWith($repository+'\',[StringComparison]::OrdinalIgnoreCase) -or
        $absolute.Equals($repository,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe uninstall target: $absolute"
    }
    # Reject linked ancestors rather than following them outside the installation.
    $ancestor = $absolute
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Linked uninstall path is not supported: $ancestor"
            }
        }
        $ancestor = Split-Path $ancestor -Parent
    }
    if (Test-Path -LiteralPath $absolute -PathType Container) {
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push($absolute)
        while ($pending.Count) {
            foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Linked item inside mod directory: $($item.FullName)"
                }
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            }
        }
    }
}
function Assert-GameClosed {
    $running = @(Get-Process call_to_arms,call_to_arms_ed,call_to_arms_server -ErrorAction SilentlyContinue)
    if ($running.Count) {
        $details = ($running | ForEach-Object { '{0} (PID {1})' -f $_.Name,$_.Id }) -join ', '
        throw "Close the game, editor and local server before uninstalling: $details"
    }
}

# Validate every path before removing the first file.
foreach ($target in $targets) { Assert-SafeTarget $target }
$installed = @($targets | Where-Object { Test-Path -LiteralPath $_ })
if (!$installed.Count) { Write-Host "Immersive Trenches is already uninstalled from $gameDirectory"; return }
if (!$WhatIfPreference) { Assert-GameClosed }
foreach ($target in $installed) {
    if ($PSCmdlet.ShouldProcess($target,'Permanently remove installed Immersive Trenches files')) {
        Assert-GameClosed
        Assert-SafeTarget $target
        Remove-Item -LiteralPath $target -Recurse -Force
        if (Test-Path -LiteralPath $target) { throw "Uninstall target remains: $target" }
        Write-Host "Removed: $target"
    }
}
if (!$WhatIfPreference) { Write-Host 'Uninstall complete. Sources and external backups were retained.' }
