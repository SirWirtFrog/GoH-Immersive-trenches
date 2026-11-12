<#
.SYNOPSIS
Build and install the current checkout of Immersive Trenches.
.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\Install.ps1
.EXAMPLE
.\Install.ps1 -GameRoot 'D:\Games\Gates of Hell' -Configuration Debug
.NOTES
Requires Visual Studio/Build Tools with MSVC v145 and Windows SDK.
Close the game, GEM Editor and local server before running.
The shared deployment script installs resources, verifies SHA-256 hashes,
removes the obsolete INI and backs up retired MG construction resources.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$GameRoot,
    [string]$MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$deploymentScript = Join-Path $PSScriptRoot 'deploy.ps1'
if (-not (Test-Path -LiteralPath $deploymentScript -PathType Leaf)) {
    throw 'deploy.ps1 is missing. Run Install.ps1 from a complete project checkout.'
}
& $deploymentScript @PSBoundParameters
