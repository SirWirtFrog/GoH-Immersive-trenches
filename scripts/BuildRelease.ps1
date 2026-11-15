[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('out\releases\ImmersiveTrenches-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) {
    throw "Release destination already exists: $destination"
}

if (-not $MSBuildPath) {
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) { $MSBuildPath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $MSBuildPath = & $vswhere -latest -prerelease -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        }
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
    throw 'MSBuild not found. Install VS/Build Tools with MSVC v145, or specify -MSBuildPath.'
}

$resources = Join-Path $repoRoot 'mods\ImmersiveTrenches'
if (-not (Test-Path -LiteralPath (Join-Path $resources 'mod.info') -PathType Leaf)) {
    throw "Resource mod not found: $resources"
}
& $MSBuildPath (Join-Path $repoRoot 'ImmersiveTrenchesMod.sln') '/t:Build' '/p:Configuration=Release' '/p:Platform=x64' '/v:minimal' '/nologo'
if ($LASTEXITCODE -ne 0) { throw "Release build failed (exit $LASTEXITCODE)." }

$build = Join-Path $repoRoot 'out\x64\Release'
$loader = Get-Item -LiteralPath (Join-Path $build 'ImmersiveTrenchesMod.asi')
$logic = Get-Item -LiteralPath (Join-Path $build 'ImmersiveTrenchesModLogic.dll')
New-Item -ItemType Directory -Path $destination | Out-Null
$miles = New-Item -ItemType Directory -Path (Join-Path $destination 'binaries\x64\mss')
$mods = New-Item -ItemType Directory -Path (Join-Path $destination 'mods')
Copy-Item -LiteralPath $resources -Destination $mods.FullName -Recurse
Copy-Item -LiteralPath $loader.FullName -Destination $miles.FullName
Copy-Item -LiteralPath $logic.FullName -Destination (Join-Path $mods.FullName 'ImmersiveTrenches')
Write-Host "Release ready: $destination"
