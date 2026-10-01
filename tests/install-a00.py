"""Exercise the real registration code against isolated INI/files, never the host OS."""
from pathlib import Path
import subprocess,os,json,shutil
root=Path(__file__).resolve().parents[1]
work=root/'build/install-a00-test';work.mkdir(exist_ok=True)
cc=Path(os.environ.get('VSC_CC',str(root/'tools/mingw-lite/mingw32_686-msvcrt_win98-16+emutls/bin/gcc.exe')))
subprocess.run([str(cc),str(root/'tests/midisetup-fixture.c'),'-o',str(work/'fixture.exe'),'-ladvapi32','-luser32','-static'],check=True)
results={}
for case in ['fresh','unregistered','registered_old','registered_same','no_slots','remove']:
 base=work/case
 if base.exists():shutil.rmtree(base)
 system=base/'WINDOWS/SYSTEM';system.mkdir(parents=True)
 shutil.copy2(work/'fixture.exe',base/'fixture.exe')
 (base/'VSC55.DRV').write_bytes(b'new driver')
 old=b'new driver' if case=='registered_same' else b'old driver'
 if case!='fresh':(system/'VSC55.DRV').write_bytes(old)
 ini=base/'WINDOWS/SYSTEM.INI'
 content='[drivers]\nwave=other.drv\nmidi=unrelated.drv\n'
 if case in ['registered_old','registered_same','remove']:content+='midi1=VSC55.DRV\n'
 if case=='no_slots':content+=''.join(f'midi{i}=other{i}.drv\n' for i in range(1,10))
 ini.write_text(content)
 env=dict(os.environ,VSC_INSTALL_FIXTURE=str(base))
 def run(action):
  p=subprocess.run([str(base/'fixture.exe'),action],env=env,capture_output=True,text=True)
  return p.returncode,p.stdout
 if case=='remove':
  code,log=run('remove');assert code==0 and 'VSC55.DRV' not in ini.read_text() and (system/'VSC55.DRV').read_bytes()==old
 else:
  before=ini.read_bytes();code,preflight=run('check')
  assert ini.read_bytes()==before
  assert code==(1 if case=='registered_old' else 0),(case,preflight)
  code,log=run('install')
  if case in ['registered_old','no_slots']:
   assert code==1 and ini.read_bytes()==before and (system/'VSC55.DRV').read_bytes()==old,(case,log)
  else:
   assert code==0 and (system/'VSC55.DRV').read_bytes()==b'new driver' and 'VSC55.DRV' in ini.read_text(),(case,log)
   assert 'midi=unrelated.drv' in ini.read_text()
   assert not list(system.glob('VSC55.[0-9][0-9][0-9]'))
 results[case]={'passed':True,'log':log}
out=root/'build/install-a00-results';out.mkdir(parents=True,exist_ok=True)
(out/'registration-fixture.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS isolated registration: fresh install, unregistered driver replacement, registered replacement refused, repeat install, full slots, uninstall')
