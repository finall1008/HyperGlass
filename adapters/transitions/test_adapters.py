import os
"""Execute the linked AArch64 capture bridge, metadata and crop under Unicorn."""
from pathlib import Path
import json,struct,math
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
import unicorn.arm64_const as R
H=Path(__file__).resolve().parent
patches=json.loads((H/'profile.json').read_text())['patches']
linked=ELFFile(open(H/'capture.elf','rb'));symbols={s.name:s['st_value']for s in linked.get_section_by_name('.symtab').iter_symbols()}
regs=[getattr(R,f'UC_ARM64_REG_X{i}')for i in range(31)]
heap=0x100000000;nil=heap+0x8081

def setup():
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x200000,0x200000);u.mem_map(heap,0x40000);mapped=set()
 def put(va,data):
  for page in range(va&-4096,(va+len(data)+4095)&-4096,4096):
   if page not in mapped:u.mem_map(page,4096);mapped.add(page)
  u.mem_write(va,data)
 for p in patches:put(int(p['vaddr'],16),bytes.fromhex(p['replacement']))
 for i,r in enumerate(regs):u.reg_write(r,0x90000+i*16)
 u.reg_write(regs[28],1);u.reg_write(regs[22],nil);u.reg_write(regs[15],0x2f0000);u.reg_write(regs[29],0x2fa000);u.reg_write(regs[27],0x300000);u.reg_write(R.UC_ARM64_REG_SP,0x2ee000)
 u.mem_write(0x300070,struct.pack('<Q',0x12345))
 return u,put

def w(u,p,v):u.mem_write(p,struct.pack('<I',v&0xffffffff))
def q(u,p,v):u.mem_write(p,struct.pack('<Q',v))
def ds(u,p,v):u.mem_write(p,struct.pack('<'+'d'*len(v),*v))
def qi(u,p):return struct.unpack('<Q',u.mem_read(p,8))[0]
def wi(u,p):return struct.unpack('<I',u.mem_read(p,4))[0]
def execute(u,label,end):u.emu_start(symbols[label],end,count=4000000);assert u.reg_read(R.UC_ARM64_REG_PC)==end

def scene(u,fw=1120,fh=2436,floor=True,known=True,scale=1.0,shift=(0,0)):
 child,root,off,mat,arr,data,bounds,view,conf,physical=[heap+x for x in range(0x10001,0x10a01,0x100)]
 w(u,child-1,(0x141b<<12)|0x500);w(u,child+0x1f,root);w(u,child+0x4f,off);w(u,child+0x47,nil);ds(u,off+7,[24.2105263158,281.4210526316])
 w(u,root-1,(0x141e<<12)|0x700);w(u,root+0x1f,nil);w(u,root+0x57,mat);w(u,mat+7,arr);w(u,root+0x2b,view)
 ds(u,arr+0x17,[2.85*scale,0,0,0,0,2.85*scale,0,0,0,0,1,0,shift[0],shift[1],0,1]);ds(u,data+0x17,[2.85,0,0,0,0,2.85,0,0,0,0,1,0,0,0,0,1]);ds(u,bounds+7,[0,0,60,60])
 w(u,view+0x5b,conf);w(u,conf+0xb,physical);ds(u,physical+7,[fw,fw,fh,fh])
 q(u,0x2f9ff8,child);q(u,0x2f9ff0,bounds);q(u,0x2fa008,0x191a61c);q(u,0x2fa000,0x2fb000)
 q(u,0x2fb008,0xa82b94);q(u,0x2fb000,0x2fc000);q(u,0x2fc008,0x19b0dc8 if known else 0x111111);q(u,0x2fc000,0x2fd000);q(u,0x2fd000,0)
 q(u,0x2faf70,nil+(0x20 if floor else 0x30));q(u,0x2faf78,bounds);q(u,0x2faf80,child)
 u.reg_write(regs[2],data)
 return child,root,data,bounds

for fw,fh,floor in [(1120,2436,True),(1600,2560,False)]:
 u,put=setup();child,root,data,bounds=scene(u,fw,fh,floor)
 for i in range(32):u.reg_write(getattr(R,f'UC_ARM64_REG_Q{i}'),0x112233445566778899aabbccddeeff00+i)
 saved=[u.reg_read(r)for r in regs];vectors=[u.reg_read(getattr(R,f'UC_ARM64_REG_Q{i}'))for i in range(32)]
 execute(u,'root_gateway',0xa02918)
 assert qi(u,0x2f9ff8)==root
 v=struct.unpack('<16d',u.mem_read(data+0x17,128));assert v==tuple(1.0 if i%5==0 else 0.0 for i in range(16))
 b=struct.unpack('<4d',u.mem_read(bounds+7,32));assert int(b[0])-1048576==69 and int(b[1])-1048576==802
 rounding=math.floor if floor else math.ceil
 assert rounding((b[2]-b[0])*2.85)==fw and rounding((b[3]-b[1])*2.85)==fh
 assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i not in [4,16,30])
 assert vectors==[u.reg_read(getattr(R,f'UC_ARM64_REG_Q{i}'))for i in range(32)]
 assert u.reg_read(R.UC_ARM64_REG_SP)==0x2ee000
 image,native,clone=heap+0x11001,heap+0x11101,heap+0x11201
 w(u,image+7,native);q(u,image+15,fw);q(u,image+23,fh);u.reg_write(regs[29],0x2fb000);u.reg_write(regs[0],image)
 execute(u,'mark_floor'if floor else'mark_ceil',0x191a750 if floor else 0x191a8bc)
 assert qi(u,image+15)==171 and qi(u,image+23)==171
 assert wi(u,image+11)&1==0 and wi(u,image+11)>>25==0x15
 assert wi(u,native+3)==(1<<28)|fw|(fh<<14)
 u.reg_write(regs[29],0x2fd000);u.reg_write(regs[1],image);u.reg_write(regs[2],171)
 execute(u,'clone_pack',0xa1e60c);packed=qi(u,0x2fcff0);assert packed>>32==wi(u,image+11)
 w(u,clone+7,native);q(u,clone+23,171);u.reg_write(regs[1],packed);u.reg_write(regs[3],clone)
 execute(u,'clone_unpack',0xa1e638);assert qi(u,clone+15)==171 and wi(u,clone+11)==wi(u,image+11)
 # Cropping uses the real external typed-array layout observed on the device.
 buffer=0x50000000;u.mem_map(buffer,0x1000000);raw=bytearray(fw*fh*4)
 for row in range(802,973):
  for col in range(69,240):struct.pack_into('<I',raw,(row*fw+col)*4,0xff000000|(row<<12)|col)
 u.mem_write(buffer,bytes(raw));array=heap+0x12001;w(u,array-1,(0x75<<12)|0x200);q(u,array+7,buffer);w(u,array+0x13,len(raw)*2)
 u.reg_write(regs[29],0x2fe000);q(u,0x2fdee0,clone);u.reg_write(regs[0],array)
 execute(u,'bytes_gateway',0x1b1519c)
 expected=b''.join(bytes(raw[((802+row)*fw+69)*4:((802+row)*fw+240)*4])for row in range(171))
 assert bytes(u.mem_read(buffer,len(expected)))==expected and wi(u,array+0x13)==len(expected)*2
 # An optional buffer reused by the result builder must not be cropped twice.
 u.reg_write(regs[29],0x2ff000);q(u,0x2feff0,clone);u.reg_write(regs[1],array)
 execute(u,'result_gateway',0x19c0058)
 assert qi(u,0x2fefe8)==array and qi(u,0x2fefd0)==171 and qi(u,0x2fefd8)==171
 assert bytes(u.mem_read(buffer,len(expected)))==expected
print('PASS: dynamic viewport, floor/ceil sizing, exact RGBA crop, clone metadata, saved CPU registers and original result-allocation PC')
# Gesture return captures while the workspace is still zoomed out: the scene
# matrix stays identity and the crop covers the target's scaled on-screen extent.
for scale in [0.9,1.08]:
 u,put=setup();child,root,data,bounds=scene(u,scale=scale,shift=(56,120));execute(u,'root_gateway',0xa02918)
 fx=2.85*scale*24.2105263158+56;fy=2.85*scale*281.4210526316+120
 v=struct.unpack('<16d',u.mem_read(data+0x17,128))
 assert all(v[i]==(1 if i%5==0 else 0) for i in range(16))
 b=struct.unpack('<4d',u.mem_read(bounds+7,32));assert int(b[0])-1048576==round(fx) and int(b[1])-1048576==round(fy)
 size=round(60*2.85*scale)
 assert round((b[0]-int(b[0])-0.5)*131072)==size and round((b[1]-int(b[1])-0.5)*131072)==size
print('PASS: scaled ancestors keep the identity scene; crop follows the on-screen target extent')
u,put=setup();child,root,data,bounds=scene(u,known=False);before=bytes(u.mem_read(bounds+7,32));execute(u,'root_gateway',0xa02918)
assert qi(u,0x2f9ff8)==child and bytes(u.mem_read(bounds+7,32))==before
print('PASS: unrelated image exports keep their existing scene and dimensions')

# Execute the real wrapper through both existing allocation return PCs.
source=ELFFile(open(Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',H.parents[1]/'fixtures/reference'))/'launcher-801027722/libapp.so','rb'))
def original(va,n):
 for s in source.iter_segments():
  if s['p_type']=='PT_LOAD'and s['p_vaddr']<=va<va+n<=s['p_vaddr']+s['p_filesz']:
   source.stream.seek(s['p_offset']+va-s['p_vaddr']);return source.stream.read(n)
for show in [False,True]:
 u,put=setup();put(0x14ebf5c,original(0x14ebf5c,48))
 p=next(p for p in patches if p['vaddr']=='14ebf7c');put(0x14ebf7c,bytes.fromhex(p['replacement']))
 for va in [0x1925784,0x1720f90,0x1b56978,0x400000]:put(va,bytes.fromhex('c0035fd6'))
 this,child,controller,key,boundary,message=[heap+x for x in [0x10001,0x10101,0x10201,0x10301,0x10401,0x10501]]
 w(u,this+0xb,child);w(u,this+0x2b,controller);w(u,controller+0x14b,key);w(u,child-1,0x1e10<<12)
 w(u,this+0x23,nil+(0x30 if show else 0x20));w(u,this+0x1f,nil+0x30)
 w(u,boundary-1,0x1e11<<12);w(u,message-1,0x1e12<<12)
 u.reg_write(regs[1],this);u.reg_write(regs[30],0x400000)
 calls=[]
 def hook(u,pc,size,opaque):
  if pc==0x1925784:
   assert u.reg_read(regs[30])==0x14ebf78;u.reg_write(regs[0],boundary);calls.append('allocate');u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(regs[30]))
  elif pc==0x1720f90:
   assert u.reg_read(regs[30])==0x1722088
   assert u.reg_read(regs[1])==this and u.reg_read(regs[2])==boundary
   u.reg_write(regs[0],message);calls.append('badge');u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(regs[30]))
 u.hook_add(UC_HOOK_CODE,hook)
 u.emu_start(0x1722030,0x400000,count=1000)
 assert u.reg_read(R.UC_ARM64_REG_PC)==0x400000
 assert struct.unpack('<I',u.mem_read(boundary+7,4))[0]==key&0xffffffff
 assert struct.unpack('<I',u.mem_read(boundary+0xb,4))[0]==child&0xffffffff
 assert struct.unpack('<I',u.mem_read(this+0xb,4))[0]==(message if show else boundary)&0xffffffff
 assert calls==(['allocate','badge']if show else['allocate'])
print('PASS: complete boundary and badge wrapping retain original allocation return PCs, key and child')

# One-cell icons use their attached complete boundary. Large geometry and
# unrelated widget types retain their original eligibility decisions.
for item_type in [0,14,2,9]:
 for sx,sy in [(1,1),(1,2),(2,1),(2,2)]:
  u,put=setup();put(0xd0fdc4,original(0xd0fdc4,264))
  for p in patches:
   if 0xd0fdc4<=int(p['vaddr'],16)<0xd0fecc:put(int(p['vaddr'],16),bytes.fromhex(p['replacement']))
  model=heap+0x10001;tag=0x7f5;w(u,model-1,tag<<12)
  q(u,model+0xf,item_type);q(u,model+0x1f,sx);q(u,model+0x27,sy);w(u,model+0xdf,nil+0x30)
  table=0x600000;put(table+(tag-0xabf)*8,struct.pack('<Q',0x500000))
  put(0x400000,bytes.fromhex('c0035fd6'));put(0x500000,bytes.fromhex('c0035fd6'))
  u.reg_write(regs[21],table);u.reg_write(regs[1],model);u.reg_write(regs[30],0x400000)
  def legacy(u,pc,size,data):
   if pc==0x500000:u.reg_write(regs[0],nil+0x20);u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(regs[30]))
  u.hook_add(UC_HOOK_CODE,legacy);u.emu_start(0xd0fdc4,0x400000,count=200)
  eligible=item_type in [0,14] and sx==sy==1
  assert u.reg_read(regs[0])==nil+(0x20 if eligible else 0x30),(item_type,sx,sy)
print('PASS: attached one-cell boundaries are eligible; large geometry and widget decisions remain scoped')

path=[heap+0x10001+0x100*i for i in range(6)];other=heap+0x10801
def capture_frames(u,selected,marked=True):
 u.reg_write(R.UC_ARM64_REG_SP,0x2ee000)
 for layer,parent in zip(path,path[1:]+[nil]):w(u,layer+0x1f,parent)
 for fp,parent,ret in [(0x2fa000,0x2fa100,0x15628a4),(0x2fa100,0x2fa200,0xa029a4),(0x2fa200,0x2fa300,0xa02928),(0x2fa300,0x2fa400,0x191a61c),(0x2fa400,0x2fa500,0xa82b94)]:q(u,fp,parent);q(u,fp+8,ret)
 bounds=heap+0x10a01;q(u,0x2fa378,bounds);ds(u,bounds+7,[1048576.5 if marked else 0,0,60,60])
 q(u,0x2fa380,path[0]);u.reg_write(regs[29],0x2fa000);u.reg_write(regs[30],0x400000)
for depth,selected,marked in [*((i,p,True)for i,p in enumerate(path)),(None,other,True),*((None,p,False)for p in path)]:
 u,put=setup();put(0x400000,bytes.fromhex('c0035fd6'));capture_frames(u,selected,marked);u.reg_write(regs[0],selected)
 u.emu_start(symbols['limit_test'],0x400000,count=2000)
 assert u.reg_read(regs[16])==(0 if depth is None else depth+1)
 u.reg_write(regs[2],0x10301)
 u.emu_start(symbols['stop_after_path'],0x1562438,count=2000)
 assert u.reg_read(regs[2])==(nil if depth is not None else heap+0x10301)
 if depth is not None:
  u.emu_start(symbols['force_path'],0x1562384,count=2000)
  assert struct.unpack('<I',u.mem_read(selected+0x23,4))[0]==(nil+0x20)&0xffffffff
print('PASS: only marked SF exports rebuild target ancestors and exclude later siblings; ordinary exports replay')

# Color filters, shader masks and translucent opacity on the capture path are
# replaced by their children (opacity keeps its offset push); elsewhere they replay.
for on_path,selected,marked in [(True,path[3],True),(False,other,True),(False,path[3],False)]:
 for label,resume in [('capture_color_filter',0x156376c),('capture_shader_mask',0x1563974)]:
  u,put=setup();capture_frames(u,selected,marked);builder=heap+0x10901
  u.reg_write(regs[1],selected);u.reg_write(regs[2],builder);u.reg_write(regs[30],0x1562400)
  end=0x1562368 if on_path else resume;u.emu_start(symbols[label],end,count=2000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==end and u.reg_read(regs[1])==selected and u.reg_read(regs[2])==builder
  if on_path:assert u.reg_read(regs[30])==0x1562400 and u.reg_read(regs[15])==0x2f0000
  else:assert u.reg_read(regs[15])==0x2f0000-16 and qi(u,0x2f0000-16)==0x2fa000 and qi(u,0x2f0000-8)==0x1562400
 for alpha in [128,255]:
  u,put=setup();capture_frames(u,selected,marked);q(u,0x2f9ff8,selected);u.reg_write(regs[3],selected)
  u.reg_write(R.UC_ARM64_REG_NZCV,0 if alpha>=255 else 1<<31)
  end=0x1562a44 if alpha>=255 or on_path else 0x15629c8;u.emu_start(symbols['capture_opacity'],end,count=2000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==end and u.reg_read(regs[3])==selected
print('PASS: offscreen color filter, shader mask and opacity layers on the capture path draw their children directly')

# Android text colors retain opacity without allocating or changing live locals.
for color in [0xff000000, 0xff808080, 0x7f22cc99, 0x00ffffff]:
 u,put=setup();u.reg_write(regs[4],color);saved=[u.reg_read(r)for r in regs]
 execute(u,'android_text_inline',0x10e5e04)
 expected=color|0xffffff
 assert u.reg_read(regs[4])==expected and qi(u,0x2f9fd0)==expected
 assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i!=4)
 u,put=setup();u.reg_write(regs[3],color);execute(u,'android_spanned_text',0xc42d88)
 assert u.reg_read(regs[3])==expected and qi(u,0x2f9fd8)==expected
 u,put=setup();u.reg_write(regs[0],color);execute(u,'android_hint_text',0x10e379c)
 assert u.reg_read(regs[0])==expected and qi(u,0x2f9fa8)==expected
for label,resume,reg in [('maml_text_color',0x164257c,4),('maml_filter_color',0x163e20c,3)]:
 u,put=setup();col=heap+0x25001;ds(u,col+7,[0.6,0.2,0.4,0.8]);u.reg_write(regs[0],col)
 execute(u,label,resume)
 assert struct.unpack('<4d',u.mem_read(col+7,32))==(0.6,1.0,1.0,1.0) and u.reg_read(regs[reg])==col
for cid in [0x21f9,*range(0x21fa,0x2208),0x2208,0x2209]:
 for caller in [0x10b85cc,0x10b85e8,0x10b86e0,0x10b8be0,0x10ba404,0x400000]:
  u,put=setup();view=heap+0x20001;controller=heap+0x21001
  q(u,view-1,(cid<<12)|0x500);w(u,view+0xb,controller)
  u.reg_write(regs[1],view);u.reg_write(regs[30],caller)
  for offset in [0xcf,0xd7,0xdb,0xdf,0xe3,0xe7]:w(u,controller+offset,heap+0x22001+offset)
  before=bytes(u.mem_read(controller,0x200));saved=[u.reg_read(r)for r in regs]
  hidden=0x21fa<=cid<=0x2207 and 0x10b85d0<=caller<0x10b9534
  execute(u,'android_background',caller if hidden else 0x173d858)
  assert bytes(u.mem_read(controller,0x200))==before
  if hidden:assert u.reg_read(regs[0])==nil
  else:assert u.reg_read(regs[2])==controller&0xffffffff
  assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i not in [0,2,16,17])
  assert u.reg_read(R.UC_ARM64_REG_SP)==0x2ee000
# Provider pictures take the existing Opacity path at 0.6 of their own alpha;
# resource drawables and hidden or opaque-by-default resources replay.
for opacity,caller,outer,end,value in [(None,0x10c69f0,0,0x10c5940,0.6),(None,0x10c5778,0,0x10c5940,0.6),(None,0x10cfff0,0,0x10c5940,0.6),
                                       (None,0x10d367c,0x10cdac0,0x10c5940,0.6),(0.5,0x10d367c,0x10cdac0,0x10c5940,0.3),(1.0,0x10cfff0,0,0x10c5940,0.6),(0.0,0x10c69f0,0,0x10c58dc,0.0),
                                       (None,0x10d367c,0x10c7838,0x10c58cc,0.0),(None,0x10cbc68,0,0x10c58cc,0.0),(1.0,0x10d367c,0x10c7838,0x10c58dc,0.0)]:
 u,put=setup();q(u,0x2fa008,caller);q(u,0x2fa000,0x2fb000);q(u,0x2fb008,outer);q(u,0x2f9ff0,0);boxed=heap+0x26001
 if opacity is not None:ds(u,boxed+7,[opacity])
 u.reg_write(regs[0],nil if opacity is None else boxed)
 saved=[u.reg_read(r)for r in regs];execute(u,'android_image_opacity',end)
 assert abs(struct.unpack('<d',u.mem_read(0x2f9ff0,8))[0]-value)<1e-12
 assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i not in [16,17])
print('PASS: Android foreground alpha, paint-phase-only container background suppression, intact provider outlines and leaf decorations, translucent provider bitmaps')

# Both host branches enter the existing Radius allocator with the live radius.
for site,start,end,context_reg in [(0x1480a00,0x1480a20,0x1480a30,2),(0x1480b78,0x1480b98,0x1480ba8,0)]:
 for radius in [20.0,24.0,32.0,21.5]:
  u,put=setup();put(start,original(start,end-start))
  context,boxed=heap+0x20001,heap+0x21001
  w(u,context+0xf,boxed);ds(u,boxed+7,[radius]);u.reg_write(regs[context_reg],context)
  u.emu_start(site,end,count=100)
  assert u.reg_read(R.UC_ARM64_REG_PC)==end
  assert qi(u,0x2f9f88)==struct.unpack('<Q',struct.pack('<d',radius))[0]
  assert u.reg_read(R.UC_ARM64_REG_D0)==struct.unpack('<Q',struct.pack('<d',radius))[0]
  assert u.reg_read(R.UC_ARM64_REG_SP)==0x2ee000
for site in [0xfc6748,0xfc7638,0xfc84a8]:
 u,put=setup();u.reg_write(regs[30],0x400000);saved=[u.reg_read(r)for r in regs]
 u.emu_start(site,0x400000,count=10)
 assert u.reg_read(regs[0])==nil
 assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i!=0)
 assert u.reg_read(R.UC_ARM64_REG_SP)==0x2ee000
print('PASS: resting/animated hosts retain computed rounded corners and original allocation PCs; native container painters preserve CPU state')

# Preserve the icon's resting wallpaper coordinates while the workspace zooms.
u,put=setup();child,root,data,bounds=scene(u)
zoom,matrix,values=heap+0x13001,heap+0x13101,heap+0x13201
w(u,child+0x1f,zoom);w(u,zoom-1,(0x141e<<12)|0x700);w(u,zoom+0x1f,root)
w(u,zoom+0x53,matrix);w(u,zoom+0x57,matrix);w(u,matrix+7,values)
ds(u,values+0x17,[.85,0,0,0,0,.85,0,0,0,0,1,0,29.5,64,0,1])
execute(u,'root_gateway',0xa02918)
b=struct.unpack('<4d',u.mem_read(bounds+7,32))
assert int(b[0])-1048576==69 and int(b[1])-1048576==802
assert round((b[0]-int(b[0])-0.5)*131072)==171
assert struct.unpack('<d',u.mem_read(values+0x17,8))[0]==.85
assert qi(u,0x2faf68)==1<<29
print('PASS: centered workspace zoom preserves resting crop coordinates and leaves the live matrix intact')
for marked,on_path,scale in [(True,True,.85),(True,True,1),(False,True,.85),(True,False,.85),(True,True,2.85)]:
 u,put=setup();selected=path[3] if on_path else other;capture_frames(u,selected,marked)
 matrix,values=heap+0x13101,heap+0x13201;w(u,selected+0x53,matrix);w(u,matrix+7,values)
 ds(u,values+0x17,[scale,0,0,0,0,scale,0,0,0,0,1,0,29.5,64,0,1])
 builder=heap+0x10901;u.reg_write(regs[1],selected);u.reg_write(regs[2],builder);u.reg_write(regs[30],0x1562400)
 end=0x1562368 if marked and on_path and scale==.85 else 0x1562734
 u.emu_start(symbols['capture_transform'],end,count=2000)
 assert u.reg_read(regs[1])==selected and u.reg_read(regs[2])==builder and u.reg_read(regs[30])==0x1562400
 assert u.reg_read(regs[15])==0x2f0000-(16 if end==0x1562734 else 0)
print('PASS: transform bypass is limited to marked capture ancestors; original builder, return address and stack survive')

# Closing capture keeps its material marker even while its Dart layout still
# carries the resting matrix from the preceding frame.
u,put=setup();child,root,data,bounds=scene(u);q(u,0x2fc008,0x1b136bc)
execute(u,'root_gateway',0xa02918);assert qi(u,0x2faf68)==1<<30
print('PASS: closing exports identify the saved backdrop before the workspace zoom updates')

# Background provider snapshots must not start a new app flight. A surrounding
# app open/close still owns the export when it asks for the provider image.
for enclosing,kind in [(None,3),(0x19b0dc8,1),(0x1b136bc,2)]:
 u,put=setup();child,root,data,bounds=scene(u);q(u,0x2fc008,0x199c894)
 if enclosing is not None:q(u,0x2fd008,enclosing)
 execute(u,'root_gateway',0xa02918)
 assert qi(u,0x2faf68)==kind<<29 and qi(u,0x2f9ff8)==root
print('PASS: provider refresh exports keep their own stage; enclosing app flights retain priority')
