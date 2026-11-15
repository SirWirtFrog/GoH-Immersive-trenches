#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$GameRoot = $env:GoHGameRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$showInstallResult = [string]::IsNullOrWhiteSpace($GameRoot)

function Show-InstallResult([string]$Message, [switch]$Failed) {
    Add-Type -AssemblyName System.Windows.Forms
    $icon = if ($Failed) { [Windows.Forms.MessageBoxIcon]::Error } else { [Windows.Forms.MessageBoxIcon]::Information }
    $null = [Windows.Forms.MessageBox]::Show($Message, 'Immersive Trenches Installer', [Windows.Forms.MessageBoxButtons]::OK, $icon)
}

trap {
    if ($showInstallResult) {
        $message = "Installation failed.`r`n`r`n$($_.Exception.Message)"
        try {
            Show-InstallResult $message -Failed
        } catch {
            Write-Warning $message
        }
    }
    break
}

function Assert-GameClosed {
    $gamePrefix = $GameRoot.TrimEnd('\') + '\'
    $running = @(Get-Process call_to_arms, call_to_arms_ed, call_to_arms_server -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase) })
    if ($running.Count) {
        throw 'Close the game, GEM Editor and local server before installing the mod.'
    }
}

function Move-InstallItem($Item, [string]$Source, [string]$Destination) {
    if ($Item.Directory) {
        [IO.Directory]::Move($Source, $Destination)
    } else {
        [IO.File]::Move($Source, $Destination)
    }
}

function Test-GameRoot([string]$Path) {
    return [IO.File]::Exists([IO.Path]::Combine($Path, 'binaries\x64\call_to_arms.exe')) -and
        [IO.File]::Exists([IO.Path]::Combine($Path, 'binaries\x64\mss64.dll'))
}

function Select-GameRoot {
    if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne [Threading.ApartmentState]::STA) {
        throw 'Start PowerShell with -STA to open the folder picker, or pass the game folder with -GameRoot.'
    }
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.Application]::EnableVisualStyles()
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    try {
        $dialog.Description = 'Select the Call to Arms - Gates of Hell game folder'
        $dialog.ShowNewFolderButton = $false
        if ($dialog.PSObject.Properties['UseDescriptionForTitle']) {
            $dialog.UseDescriptionForTitle = $true
        }
        while ($dialog.ShowDialog() -eq [Windows.Forms.DialogResult]::OK) {
            if (Test-GameRoot $dialog.SelectedPath) { return $dialog.SelectedPath }
            $null = [Windows.Forms.MessageBox]::Show('Choose the game folder that contains binaries\x64\call_to_arms.exe.',
                'Game folder not found', [Windows.Forms.MessageBoxButtons]::OK, [Windows.Forms.MessageBoxIcon]::Warning)
        }
    } finally {
        $dialog.Dispose()
    }
}

if ($showInstallResult) {
    $GameRoot = Select-GameRoot
    if ([string]::IsNullOrWhiteSpace($GameRoot)) {
        Write-Host 'Installation canceled.'
        return
    }
}
$GameRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GameRoot.Trim().Trim('"'))
if (-not (Test-GameRoot $GameRoot)) {
    throw "Game installation not found at '$GameRoot'. Pass the game folder with -GameRoot."
}
Assert-GameClosed

$loaderPath = 'binaries\x64\mss\ImmersiveTrenchesMod.asi'
$modPath = 'mods\ImmersiveTrenches'
$items = @(
    @{ Relative = $modPath; Directory = $true; Saved = $false; Installed = $false }
    @{ Relative = $loaderPath; Directory = $false; Saved = $false; Installed = $false }
)

# Hold the lock while downloading and replacing files in this game installation.
$lockPath = Join-Path $GameRoot '.ImmersiveTrenches-install.lock'
$lock = [IO.FileStream]::new($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None, 4096, [IO.FileOptions]::DeleteOnClose)
try {
    $repository = 'SirWirtFrog/GoH-Immersive-trenches'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $headers = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/releases/latest" -Headers $headers -TimeoutSec 30
    $assetName = "ImmersiveTrenches-$($release.tag_name)-x64.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $assetName -and $_.state -eq 'uploaded' })
    if ($assets.Count -ne 1 -or $assets[0].digest -notmatch '^sha256:[0-9a-f]{64}$') {
        throw "Release '$($release.tag_name)' must have one uploaded $assetName with a SHA-256 digest."
    }
    $asset = $assets[0]

    # Stage on the game volume so that installation and rollback use file renames.
    $work = Join-Path $GameRoot ('.ImmersiveTrenches-install-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $work
    $zip = Join-Path $work 'release.zip'
    $package = Join-Path $work 'package'
    $backup = Join-Path $work 'backup'
    $rollbackFailed = $false
    try {
        Write-Host "Downloading Immersive Trenches $($release.tag_name)..."
        Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $zip -TimeoutSec 180
        $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
        if ("sha256:$hash" -ine $asset.digest) {
            throw "SHA-256 mismatch for $assetName."
        }

        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            foreach ($entry in $archive.Entries) {
                $name = $entry.FullName.Replace('\', '/')
                $allowed = $name -cin @('binaries/', 'binaries/x64/', 'binaries/x64/mss/', 'mods/', $loaderPath.Replace('\', '/')) -or
                    $name.StartsWith('mods/ImmersiveTrenches/', [StringComparison]::Ordinal)
                if (-not $allowed -or $name -match '(^|/)(\.{1,2})(/|$)|:') {
                    throw "Unexpected release archive entry: $name"
                }
            }
        } finally {
            $archive.Dispose()
        }
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $package)
        foreach ($relative in @($loaderPath, "$modPath\ImmersiveTrenchesModLogic.dll", "$modPath\mod.info")) {
            if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) {
                throw "Incomplete release archive. Missing $relative."
            }
        }

        Assert-GameClosed
        foreach ($relative in @('binaries', 'binaries\x64', 'binaries\x64\mss', 'mods', $modPath, $loaderPath)) {
            $path = Join-Path $GameRoot $relative
            $existing = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
            if ($existing -and ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Linked install path is unsupported: $path"
            }
        }
        try {
            foreach ($item in $items) {
                $target = Join-Path $GameRoot $item.Relative
                $saved = Join-Path $backup $item.Relative
                $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
                if (Test-Path -LiteralPath $target) {
                    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force
                    Move-InstallItem $item $target $saved
                    $item.Saved = $true
                }
                Move-InstallItem $item (Join-Path $package $item.Relative) $target
                $item.Installed = $true
            }
        } catch {
            $installError = $_
            for ($index = $items.Count - 1; $index -ge 0; $index--) {
                $item = $items[$index]
                $target = Join-Path $GameRoot $item.Relative
                try {
                    if ($item.Installed) {
                        Move-InstallItem $item $target (Join-Path $package $item.Relative)
                    }
                    if ($item.Saved) {
                        Move-InstallItem $item (Join-Path $backup $item.Relative) $target
                    }
                } catch {
                    $rollbackFailed = $true
                    Write-Warning "Could not roll back '$target'. Recovery directory: $work. $_"
                }
            }
            if ($rollbackFailed) {
                throw "Installation failed and rollback is incomplete. Recovery files: $work. $installError"
            }
            throw $installError
        }
    } finally {
        if (-not $rollbackFailed) {
            $resolvedWork = (Resolve-Path -LiteralPath $work).ProviderPath
            if ((Split-Path -Parent $resolvedWork) -ne $GameRoot.TrimEnd('\') -or
                (Split-Path -Leaf $resolvedWork) -notmatch '^\.ImmersiveTrenches-install-[0-9a-f]{32}$') {
                throw "Unexpected temporary install directory: $resolvedWork"
            }
            Remove-Item -LiteralPath $resolvedWork -Recurse -Force
        }
    }
} finally {
    $lock.Dispose()
}

$message = "Successfully installed Immersive Trenches $($release.tag_name).`r`n`r`nGame folder: $GameRoot"
Write-Host $message
if ($showInstallResult) {
    Show-InstallResult $message
}
