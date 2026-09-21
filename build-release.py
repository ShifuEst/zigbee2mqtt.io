from pathlib import Path
import hashlib, json, shutil, zipfile, os

root=Path(__file__).resolve().parent
out=root/'release'
out.mkdir(exist_ok=True)
with zipfile.ZipFile(root/'release-template.zip') as z:
    for name in z.namelist():
        target=(out/name).resolve()
        if not target.is_relative_to(out.resolve()):
            raise RuntimeError('Invalid template path')
    z.extractall(out)
source=out/'source/MD_GATE_ZB1'
source.mkdir(parents=True,exist_ok=True)
for name in ['MD_GATE_ZB1.ino','RelayLevels.h']:
    shutil.copy2(root/name,source/name)
binary=root/'build/MD_GATE_ZB1.ino.merged.bin'
if binary.stat().st_size != 4194304:
    raise RuntimeError('Expected a complete 4 MB merged image')
shutil.copy2(binary,out/'MD-GATE-ZB1-LOW.bin')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'version':'1.7.0-rc2','chip':'esp32h2','sourceSha256':sha(root/'MD_GATE_ZB1.ino'),'levelsSha256':sha(root/'RelayLevels.h'),'gitCommit':os.environ.get('GITHUB_SHA'),'variants':{'LOW':{'relay_type':'LOW','file':'MD-GATE-ZB1-LOW.bin','sha256':sha(binary),'hardware_verified':False}}}
(out/'release.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8')
(out/'SHA256SUMS.txt').write_text(''.join(f'{sha(p)}  {p.relative_to(out).as_posix()}\n' for p in sorted(out.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.txt'),encoding='utf8')
with zipfile.ZipFile(root/'MD-GATE-ZB1-1.7.0-rc2-LOW.zip','w',zipfile.ZIP_DEFLATED) as z:
    for p in out.rglob('*'):
        if p.is_file(): z.write(p,'MD-GATE-ZB1-1.7.0-rc2-LOW/'+p.relative_to(out).as_posix())
print(json.dumps(manifest,indent=2))
