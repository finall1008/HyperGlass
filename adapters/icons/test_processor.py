import os
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
import unicorn.arm64_const as R
from PIL import Image,ImageDraw
HERE=Path(__file__).resolve().parent
e=ELFFile((HERE/'processor.elf').open('rb'));code=e.get_section_by_name('.text').data()
def execute(data):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x2007000,0x2000);u.mem_write(0x20078c4,code)
 u.mem_map(0x2000000,4096);u.mem_write(0x2000120,e.get_section_by_name('.text.helper').data())
 u.mem_map(0x200000,0x20000);u.mem_map(0x300000,0x110000);u.mem_map(0x500000,4096)
 obj=0x300001;u.mem_write(obj+0x13,struct.pack('<I',len(data)*2));u.mem_write(obj+0x17,data)
 regs=[getattr(R,f'UC_ARM64_REG_X{i}')for i in range(31)]
 for i,r in enumerate(regs):u.reg_write(r,0x600000+i*16)
 u.reg_write(regs[29],0x21f800);u.reg_write(regs[1],obj);u.reg_write(regs[15],0x21f000);u.reg_write(regs[30],0x500000);u.reg_write(R.UC_ARM64_REG_SP,0x21e000)
 saved=[u.reg_read(r)for r in regs]
 u.emu_start(0x20078c4,0x500000,count=max(4000000,len(data)*400))
 assert u.reg_read(R.UC_ARM64_REG_PC)==0x500000
 assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i!=0)
 assert u.reg_read(R.UC_ARM64_REG_SP)==0x21e000
 for offset in range(0x130,0x178,8):assert bytes(u.mem_read(0x21f800-offset,8))==struct.pack('<Q',saved[22])
 return bytes(u.mem_read(obj+0x17,len(data)))
outputs=[]
for bg,fg in [((255,255,255,255),(0,0,0,255)),((0,0,0,255),(255,255,255,255)),((240,74,50,255),(255,255,255,255))]:
 im=Image.new('RGBA',(64,64),bg);ImageDraw.Draw(im).ellipse((20,20,44,44),fill=fg)
 out=execute(im.tobytes());outputs.append(out)
 assert out[3]==0 and out[(32*64+32)*4+3]==222
 assert out[(32*64+32)*4:(32*64+32)*4+3]==bytes([255]*3)
assert outputs[0]==outputs[1]==outputs[2]
im=Image.new('RGBA',(64,64));ImageDraw.Draw(im).line((20,32,44,32),fill=(128,128,128,128),width=1)
out=execute(im.tobytes());assert out[(32*64+32)*4+3]==120
assert execute(bytes([1,2,3,4]*30))==bytes([1,2,3,4]*30)
print('PASS: identical dark/light/color plates, preserved transparent thin strokes, bounded fallback, all 30 non-result registers and SP')
# Smooth plates must disappear between the glyph and the card edge.
im=Image.new('RGBA',(64,64))
for y in range(64):
 for x in range(64):
  v=210+y//2;im.putpixel((x,y),(v,v,v,255))
ImageDraw.Draw(im).rectangle((25,20,38,43),fill=(0,0,0,255))
out=execute(im.tobytes());assert out[(32*64+10)*4+3]==18
assert out[(32*64+32)*4+3]==222
# Distinct colored details in the same glyph retain monochrome shading.
im=Image.new('RGBA',(64,64),(240,74,50,255));d=ImageDraw.Draw(im)
d.rectangle((18,18,46,46),fill='white');d.rectangle((26,26,38,38),fill=(17,64,207,255))
out=execute(im.tobytes());assert out[(32*64+32)*4]<out[(22*64+22)*4]
print('PASS: gradient plate removal and internal monochrome detail contrast')
# Full-bleed artwork can fail plate detection. Its foreground must obey the
# same rounded outline as the glass, including partial corner coverage.
im=Image.new('RGBA',(64,64),(15,60,220,255))
for x,y in [(32,4),(32,59),(4,32),(59,32)]:im.putpixel((x,y),(0,0,0,0))
out=execute(im.tobytes())
for x,y in [(0,0),(63,0),(0,63),(63,63),(2,2),(61,61)]:assert out[(y*64+x)*4+3]==0
assert 0<out[(4*64+4)*4+3]<220
assert out[(32*64+32)*4+3]==222
print('PASS: full-bleed foreground clipped to antialiased rounded corners without changing center artwork')
