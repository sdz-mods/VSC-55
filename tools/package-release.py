"""Build a ROM-free binary package and matching source archive."""
from pathlib import Path
import hashlib,json,shutil,subprocess,zipfile
root=Path(__file__).resolve().parents[1]
out=root/'dist/VSC55-A00'
files={'build/panel/vsc55.exe':'VSC55.EXE','build/panel/vsccfg.exe':'VSCCFG.EXE',
       'build/panel/midireg.exe':'MIDIREG.EXE','build/driver16/VSC55.DRV':'VSC55.DRV',
       'installer/INSTALL.BAT':'INSTALL.BAT','installer/UNINSTALL.BAT':'UNINSTALL.BAT',
       'installer/REMOVE.BAT':'REMOVE.BAT','installer/VSC55.INI':'VSC55.INI',
       'installer/README.TXT':'README.TXT','LICENSE':'LICENSE.TXT',
       'LICENSES.md':'LICENSES.MD','CREDITS.md':'CREDITS.TXT','BUILDING.md':'BUILDING.MD',
       'README.md':'README.md',
       'assets/vsc55-icon-preview.png':'assets/vsc55-icon-preview.png',
       'assets/screenshots/vsc55-windows98-desktop.png':'assets/screenshots/vsc55-windows98-desktop.png',
       'assets/frontpanel-float/PANEL.BMP':'PANEL.BMP',
       'assets/frontpanel-float/LICENSE.txt':'PANELART.TXT'}
for source in files:
    if not (root/source).is_file():
        raise SystemExit(f'Missing {source}; build first')

# Recreate only this generated staging directory; reject redirected paths.
if out.resolve() != out:
    raise SystemExit(f'Refusing to clean redirected staging directory: {out}')
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True)

for source,dest in files.items():
 p=root/source
 (out/dest).parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(p,out/dest)
for name in ['INSTALL.BAT','UNINSTALL.BAT','REMOVE.BAT','README.TXT','VSC55.INI']:
 p=out/name;p.write_bytes(p.read_text(encoding='ascii').replace('\n','\r\n').encode('ascii'))
shutil.copytree(root/'LICENSES',out/'LICENSES',dirs_exist_ok=True)
# The installer uses a TXT credit file; Markdown links in the guide target it.
p=out/'README.md';p.write_text(p.read_text(encoding='utf-8').replace('(CREDITS.md)','(CREDITS.TXT)').replace('(LICENSES.md)','(LICENSES.MD)').replace('(BUILDING.md)','(BUILDING.MD)'),encoding='utf-8')
p=out/'LICENSES.MD';p.write_text(p.read_text(encoding='utf-8').replace('CREDITS.md','CREDITS.TXT'),encoding='utf-8')
# Whitelist source trees rather than traversing local work/build folders.
source_files=[]
for name in ['src','tests','tools','installer','assets','LICENSES']:
 for p in (root/name).rglob('*'):
  rel=p.relative_to(root).as_posix()
  if not p.is_file() or rel.startswith(('tools/ow/','tools/mingw-lite/','tools/downloads/','docs/results/')):continue
  if '__pycache__' in p.parts or p.suffix.lower() in ['.pyc','.log','.rom','.wav','.obj','.map']:continue
  source_files.append(p)
source_files.extend(root/n for n in ['README.md','BUILDING.md','CREDITS.md','LICENSES.md','LICENSE','CMakeLists.txt','.gitignore','.gitattributes','.gitmodules','.astylerc'])
core=root/'upstream/Nuked-SC55'
if not (core/'src/backend/emu.cpp').exists():raise SystemExit('Initialize the pinned core submodule first')
# Explicit Git file inventory avoids build outputs or submodule metadata.
if (core/'.git').exists():
 raw=subprocess.check_output(['git','ls-files','-z'],cwd=core)
 core_files=[core/n.decode() for n in raw.split(b'\0') if n]
else:
 core_files=[p for p in core.rglob('*') if p.is_file() and '.git' not in p.parts]
source_files.extend(core_files)
with zipfile.ZipFile(out/'SOURCE.ZIP','w',zipfile.ZIP_DEFLATED) as z:
 for p in sorted(set(source_files)):z.write(p,p.relative_to(root).as_posix())
manifest=[]
for p in sorted(out.rglob('*')):
 if p.is_file() and p.name!='SHA256.JSON':manifest.append({'File':p.relative_to(out).as_posix(),'SHA256':hashlib.sha256(p.read_bytes()).hexdigest()})
(out/'SHA256.JSON').write_text(json.dumps(manifest,indent=2)+'\n')
with zipfile.ZipFile(root/'dist/VSC55-A00.zip','w',zipfile.ZIP_DEFLATED) as z:
 for p in sorted(out.rglob('*')):
  if p.is_file():z.write(p,'VSC55-A00/'+p.relative_to(out).as_posix())
print(out)
