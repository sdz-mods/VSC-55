from pathlib import Path
import hashlib,json,struct,zipfile
root=Path(__file__).resolve().parents[1];pkg=root/'dist/VSC55-A00'
for row in json.loads((pkg/'SHA256.JSON').read_text()):
 assert hashlib.sha256((pkg/row['File']).read_bytes()).hexdigest()==row['SHA256'],row['File']
for name in ['VSC55.EXE','VSCCFG.EXE','MIDIREG.EXE']:
 b=(pkg/name).read_bytes();pe=struct.unpack_from('<I',b,0x3c)[0]
 assert b[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',b,pe+4)[0]==0x14c
 assert struct.unpack_from('<H',b,pe+24+48)[0]==4
for name in ['INSTALL.BAT','UNINSTALL.BAT','REMOVE.BAT']:
 b=(pkg/name).read_bytes();assert max(b)<128 and b'\n' not in b.replace(b'\r\n',b'')
b=(pkg/'VSC55.DRV').read_bytes();off=struct.unpack_from('<I',b,0x3c)[0];assert b[off:off+2]==b'NE' and b'VSC-55\0' in b
with zipfile.ZipFile(pkg/'SOURCE.ZIP') as z:
 names=z.namelist();assert 'upstream/Nuked-SC55/src/backend/emu.cpp' in names
 for n in names:
  assert not n.lower().startswith(('roms/','build/','dist/','tools/ow/','tools/mingw-lite/'))
  assert '/.git/' not in n and not n.endswith('.rom')
  if n.endswith(('.c','.cpp','.h','.ps1','.py','.md')):assert b'OneDrive'+b'\\Desktop' not in z.read(n)
print('PASS manifest, PE/NE formats, Win98 subsystem, DOS batches and source hygiene')
