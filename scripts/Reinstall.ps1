<#
.SYNOPSIS
Build Immersive Trenches, remove the installed copy, then deploy the new build and resources.
.EXAMPLE
.\scripts\Reinstall.ps1 -GameRoot 'D:\Games\Gates of Hell' -Configuration Debug
.NOTES
Close the game, GEM Editor and local server before running.
The entire installed mod directory is removed, including files added locally.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$GameRoot,
    [string]$MSBuildPath
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'deploy.ps1') -Reinstall @PSBoundParameters
