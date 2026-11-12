"""Uninstaller verification on a disposable fake game, never on the real install."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def run():
    with tempfile.TemporaryDirectory() as directory:
        game=Path(directory)/'Game with spaces'
        miles=game/'binaries/x64/mss';miles.mkdir(parents=True)
        for name in ('call_to_arms.exe','mss64.dll'):(miles.parent/name).write_bytes(b'game')
        other=miles/'OtherMod.asi';other.write_bytes(b'other')
        loader=miles/'ImmersiveTrenchesMod.asi';loader.write_bytes(b'loader')
        pdb=miles/'ImmersiveTrenchesMod.pdb';pdb.write_bytes(b'symbols')
        mod=game/'mods/ImmersiveTrenches';(mod/'resource/entity/old').mkdir(parents=True)
        (mod/'resource/entity/old/legacy.def').write_text('legacy')
        (mod/'ImmersiveTrenchesModLogic.dll').write_bytes(b'logic')
        (mod/'ImmersiveTrenchesMod.ini').write_text('old ini')
        custom=game/'mods/OtherMod/keep.def';custom.parent.mkdir();custom.write_text('keep')
        backup=game.parent/'backup';backup.mkdir();(backup/'keep').write_text('backup')
        # Isolate process discovery so the user's running game is left alone.
        wrapper=Path(directory)/'invoke-uninstall.ps1'
        wrapper.write_text('''param([string]$Script,[string]$GameRoot,[switch]$WhatIf,[switch]$Running)
function Get-Process {
    if ($Running) { [pscustomobject]@{ Name='call_to_arms'; Id=1234 } }
}
& $Script -GameRoot $GameRoot -WhatIf:$WhatIf
''')
        args=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(wrapper),'-Script',str(ROOT/'uninstall.ps1'),'-GameRoot',str(game)]
        subprocess.run(args+['-WhatIf'],capture_output=True,text=True,check=True)
        assert loader.is_file() and mod.is_dir() and pdb.is_file()
        blocked=subprocess.run(args+['-Running'],capture_output=True,text=True)
        assert blocked.returncode!=0 and 'Close the game' in blocked.stderr
        assert loader.is_file() and mod.is_dir() and pdb.is_file()
        subprocess.run(args,capture_output=True,text=True,check=True)
        assert not loader.exists() and not mod.exists() and not pdb.exists()
        assert other.read_bytes()==b'other' and custom.read_text()=='keep'
        assert (miles.parent/'mss64.dll').read_bytes()==b'game'
        assert (backup/'keep').read_text()=='backup'
        subprocess.run(args,capture_output=True,text=True,check=True)
        invalid=args[:-1]+[str(game.parent)]
        assert subprocess.run(invalid,capture_output=True,text=True).returncode!=0
    print('Uninstall checks passed: WhatIf, running game rejection, exact installed targets, legacy resources, preserved game/other mods/backups, idempotence, invalid root rejection')

if __name__=='__main__':run()
