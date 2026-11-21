# Immersive Trenches

Immersive Trenches adds a trench construction menu for engineers in **Call to Arms – Gates of Hell: Ostfront**. Engineers can build straight sections, turns, junctions, exits, earthen bunkers, firing positions, and a trench depot that resupplies nearby units with ammunition.

Sections snap to open connections and rotate to align with them. You can queue multiple sections for construction. End caps close the open ends of completed trenches and disappear when a new section is connected.

## Build and install from source

Building requires **Visual Studio 2026** or **Build Tools 2026** with MSVC v145 and the Windows SDK. Close the game, GEM Editor, and local server. Open PowerShell in the project root and run the following command.

```powershell
.\Install.ps1 -GameRoot 'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell'
```

If PowerShell blocks the script, run `powershell -ExecutionPolicy Bypass -File .\Install.ps1`.

## Install a release build

Close the game, GEM Editor, and local server. Run [InstallLatest.ps1](InstallLatest.ps1), then select the game folder.

```powershell
pwsh -STA -ExecutionPolicy Bypass -File .\InstallLatest.ps1
```

In Windows PowerShell 5.1, use `powershell` in place of `pwsh` to open the classic folder picker. To find the game folder in Steam, open **Properties → Installed Files → Browse**. For unattended installation, pass `-GameRoot 'D:\Games\Call to Arms - Gates of Hell'` or set the `GoHGameRoot` environment variable.

To install manually, download the ZIP and copy its `binaries` and `mods` folders into the game folder. [GitHub Releases](https://github.com/SirWirtFrog/GoH-Immersive-trenches/releases)

After installation, launch the game and enable **Immersive Trenches** in the mod list.

The mod supports only **1.0.6.9-fd5c5ce0f x64** for the game and GEM Editor. Native hooks require an exact match for the executable’s PE timestamp and image size. In multiplayer, all players must use the same mod version.

## Additional commands

Run these commands from the project root. Close the game, GEM Editor, and local server before reinstalling or uninstalling.

| Task | Command |
| --- | --- |
| Build a package for manual installation | `.\scripts\BuildRelease.ps1` |
| Build and install a Debug build | `.\Install.ps1 -Configuration Debug` |
| Perform a clean reinstall | `.\scripts\Reinstall.ps1` |
| Preview which files will be removed | `.\scripts\uninstall.ps1 -WhatIf` |
| Uninstall the mod | `.\scripts\uninstall.ps1` |

The manual installation package is saved to `out\releases\ImmersiveTrenches-<timestamp>`. Use `-OutputDirectory` to choose a different location. Reinstalling or uninstalling removes the entire installed mod folder, including any files added manually.

<details>
<summary>Technical details and developer checks</summary>

Miles Sound System loads `ImmersiveTrenchesMod.asi` from `binaries\x64\mss`. On `RIB_Main(upDown=1)`, the loader opens `mods\ImmersiveTrenches\ImmersiveTrenchesModLogic.dll` and calls `GoHModInitialize` outside `DllMain`. Engine functions and structures are documented in [GEM_FUNCTIONS.md](GEM_FUNCTIONS.md).

Build output is written to `out\x64\<Configuration>`. To debug, select `Debug|x64` and press F5 in Visual Studio to launch GEM Editor. Set `GoHGameRoot` to use a custom game path for the F5 profile and installation scripts.

`DeployResources` backs up recognized legacy directories to `%LOCALAPPDATA%\ImmersiveTrenches\resource-backups`. To preview the copy plan without changing files, run `scripts\DeployResources.ps1` with `-PlanOnly`, `-SourceRoot`, and `-DestinationRoot`.
