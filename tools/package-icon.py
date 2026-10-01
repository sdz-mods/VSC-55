"""Render the editable 16px SVG and package uncompressed Win98 16-colour icons."""
from pathlib import Path
from PIL import Image
import struct,xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
svg=ET.parse(root/'assets/vsc55-icon.svg').getroot()
assert svg.attrib['viewBox']=='0 0 16 16'
colors=list(dict.fromkeys(r.attrib['fill'] for r in svg));assert len(colors)<=16
im=Image.new('P',(16,16),0)
palette=[int(color[i:i+2],16) for color in colors for i in (1,3,5)]
im.putpalette(palette+[0]*(768-len(palette)))
for r in svg:
 x,y,w,h=(int(r.attrib[k]) for k in ('x','y','width','height'))
 for yy in range(y,y+h):
  for xx in range(x,x+w):im.putpixel((xx,yy),colors.index(r.attrib['fill']))
im.save(root/'assets/vsc55-icon-16.png')
im.resize((128,128),Image.Resampling.NEAREST).save(root/'assets/vsc55-icon-preview.png')
frames=[]
for n in (16,32,48):
 s=im.resize((n,n),Image.Resampling.NEAREST);stride=((n*4+31)//32)*4;maskstride=((n+31)//32)*4
 pixels=bytearray();mask=bytearray()
 for y in range(n-1,-1,-1):
  row=bytes((s.getpixel((x,y))<<4)|s.getpixel((x+1,y)) for x in range(0,n,2))
  pixels+=row+bytes(stride-len(row));mask+=bytes(maskstride)
 pal=b''.join(bytes((palette[i+2],palette[i+1],palette[i],0)) for i in range(0,len(palette),3))+bytes((16-len(colors))*4)
 dib=struct.pack('<IiiHHIIiiII',40,n,n*2,1,4,0,len(pixels)+len(mask),0,0,16,0)+pal+pixels+mask
 frames.append((n,dib))
offset=6+16*len(frames);result=bytearray(struct.pack('<HHH',0,1,len(frames)))
for n,dib in frames:
 result+=struct.pack('<BBBBHHII',n,n,16,0,1,4,len(dib),offset);offset+=len(dib)
for n,dib in frames:result+=dib
(root/'src/gui/vsc55.ico').write_bytes(result)
print('Packed orange LCD 55 icon at 16, 32 and 48 pixels')
