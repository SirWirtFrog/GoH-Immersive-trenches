"""Exercise DeployResources on disposable directories; never touches installed game."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
REL=Path('resource/entity/landscape/cut_entity/IT/dirt')

def run():
    with tempfile.TemporaryDirectory() as directory:
        base=Path(directory);source=base/'source';target=base/'target';backup=base/'backup'
        source.mkdir();target.mkdir()
        (source/'mod.info').write_text('{mod}')
        current=source/REL/'trench_dirt_straight';current.mkdir(parents=True)
        (current/'it_trench_dirt_straight.def').write_text('{extension "new.mdl"}')
        (current/'new.mdl').write_text('new cut')
        old=target/REL/'trench_straight';old.mkdir(parents=True)
        (old/'it_trench_straight.def').write_text('old definition')
        (old/'old.mdl').write_text('old cut')
        legacy=target/'resource/entity/landscape/cut_entity/immersiveTrenches/dirt/trench_dirt_bridge'
        legacy.mkdir(parents=True)
        (legacy/'it_trench_bridge.def').write_text('legacy namespace')
        custom=target/REL/'custom';custom.mkdir()
        (custom/'keep.def').write_text('custom')
        runtime=target/'ImmersiveTrenchesModLogic.dll';runtime.write_bytes(b'keep runtime')
        mg=source/REL/'trench_dirt_mg_nest';mg.mkdir()
        (mg/'it_trench_dirt_mg_nest.def').write_text('new mg')
        broken=target/REL/'trench_dirt_mg-nest';broken.mkdir()
        (broken/'it_trench_dirt_mg-nest.def').write_text('old mg')
        source_hash={p.relative_to(source):hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*') if p.is_file()}
        args=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'scripts/DeployResources.ps1'),'-SourceRoot',str(source),'-DestinationRoot',str(target),'-BackupRoot',str(backup)]
        plan=subprocess.run(args+['-PlanOnly'],capture_output=True,text=True,check=True)
        assert len(json.loads(plan.stdout)['retired'])==3
        assert old.exists() and not backup.exists()
        subprocess.run(args,capture_output=True,text=True,check=True)
        assert not old.exists() and not legacy.exists() and not broken.exists()
        assert len(list(backup.rglob('it_trench_dirt_mg-nest.def')))==1
        assert len(list(backup.rglob('it_trench_bridge.def')))==1
        assert (target/REL/'trench_dirt_straight/new.mdl').read_text()=='new cut'
        assert (custom/'keep.def').read_text()=='custom'
        assert runtime.read_bytes()==b'keep runtime'
        copies=list(backup.rglob('old.mdl'));assert len(copies)==1 and copies[0].read_text()=='old cut'
        assert source_hash=={p.relative_to(source):hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*') if p.is_file()}
        plan=subprocess.run(args+['-PlanOnly'],capture_output=True,text=True,check=True)
        assert not json.loads(plan.stdout)['retired']
        subprocess.run(args,capture_output=True,text=True,check=True)
        assert len(list(backup.rglob('old.mdl')))==1
        refused=subprocess.run(args[:args.index('-DestinationRoot')]+['-DestinationRoot',str(source),'-BackupRoot',str(backup)],capture_output=True,text=True)
        assert refused.returncode!=0
    print('Resource deployment checks passed: dry run, legacy backup, copied data, preserved runtime/custom files, source integrity, idempotence, overlap refusal')

if __name__=='__main__':run()
