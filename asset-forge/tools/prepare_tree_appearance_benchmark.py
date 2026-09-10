"""Isolated engine benchmark project: no Voxelsim gameplay/world subsystems."""
import json
import shutil
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
BENCH=ROOT/'.scratch/appearance-benchmark'
BENCH.mkdir(parents=True,exist_ok=True)
(BENCH/'AppearanceBenchmark.uproject').write_text(json.dumps(dict(FileVersion=3,EngineAssociation='5.8',Plugins=[dict(Name='PythonScriptPlugin',Enabled=True),dict(Name='EditorScriptingUtilities',Enabled=True)]),indent=2))
(BENCH/'Config').mkdir(exist_ok=True)
lines=(ROOT/'ue-project/Config/DefaultEngine.ini').read_text(encoding='utf8').splitlines()
section=[];keep=False
for line in lines:
    if line.startswith('['):keep=line in ('[/Script/Engine.RendererSettings]','[/Script/WindowsTargetPlatform.WindowsTargetSettings]')
    if keep:section.append(line)
(BENCH/'Config/DefaultEngine.ini').write_text('[/Script/EngineSettings.GameMapsSettings]\nGameDefaultMap=/Engine/Maps/Entry\nGlobalDefaultGameMode=/Script/Engine.GameModeBase\n\n'+'\n'.join(section)+'\n')
shutil.copytree(ROOT/'ue-project/Content/Voxel/AppearancePilot/V2',BENCH/'Content/Voxel/AppearancePilot/V2',dirs_exist_ok=True)
print(BENCH/'AppearanceBenchmark.uproject')
