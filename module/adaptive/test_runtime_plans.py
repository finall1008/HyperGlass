import os
"""Execute the native resolver's relocated bytes against each target's Dart layout."""
from pathlib import Path
import json,struct,sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
import unicorn.arm64_const as R
from model import Image,branch_target,immediate
H=Path(__file__).resolve().parent;EX=H.parents[1]/'adapters'
# Shared synthetic Dart objects from the complete reference adapter suite.
fixture=(EX/'transitions/test_adapters.py').read_text().split('for fw,fh,floor in ')[0]
ns={'__file__':str(EX/'transitions/test_adapters.py')};exec(fixture,ns)
regs,heap,nil=ns['regs'],ns['heap'],ns['nil'];w,q,ds,qi,wi=(ns[x]for x in ['w','q','ds','qi','wi'])
e=ELFFile(open(EX/'transitions/capture.elf','rb'));symbols={s.name:s['st_value']for s in e.get_section_by_name('.symtab').iter_symbols()}
for planpath,imagepath in zip(sys.argv[1::2],sys.argv[2::2]):
 p=json.loads(Path(planpath).read_text());anchors=dict(p['anchors']);base=p['base'];shadow=p['shadow']
 def address(old):
  for source,at,data,group in p['adapters']:
   if source<=old<source+len(bytes.fromhex(data)):return at+old-source
  if old in anchors:return base+anchors[old]
  raise ValueError(hex(old))
 def setup():
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x200000,0x200000);u.mem_map(heap,0x40000)
  image=ELFFile(open(imagepath,'rb'))
  # Map one bounded original-image range; data comes from the original ELF.
  span=max(s['p_vaddr']+s['p_memsz']for s in image.iter_segments()if s['p_type']=='PT_LOAD')
  u.mem_map(base,(span+4095)&-4096)
  for s in image.iter_segments():
   if s['p_type']=='PT_LOAD':u.mem_write(base+s['p_vaddr'],s.data())
  mapped=set()
  for source,at,data,group in p['adapters']:
   raw=bytes.fromhex(data)
   for page in range(at&-4096,(at+len(raw)+4095)&-4096,4096):
    if page not in mapped:u.mem_map(page,4096);mapped.add(page)
   u.mem_write(at,raw)
  for at,data,group in p['patches']:u.mem_write(at,bytes.fromhex(data))
  for i,r in enumerate(regs):u.reg_write(r,0x90000+i*16)
  for reg,value in [(28,1),(22,nil),(15,0x2f0000),(29,0x2fa000),(27,0x300000)]:u.reg_write(regs[reg],value)
  u.reg_write(R.UC_ARM64_REG_SP,0x2ee000);q(u,0x300070,0x12345)
  return u
 target=Image(Path(imagepath))
 # The flight source distinguishes duplicate icons in different containers.
 # Reuse the original component/user interpolation and its GC return PCs.
 tail=address(0x11f8168);start,size,_=target.names['AppInfoModel.getcomponentNameString'][0]
 calls=[(pc,branch_target(int.from_bytes(target.read(pc,4),'little'),pc))for pc in range(tail-base,start+size,4)if int.from_bytes(target.read(pc,4),'little')&0xfc000000==0x94000000]
 allocate=calls[0];interpolate=next(call for call in calls if call[1]==target.names['_StringBase._interpolate'][0][0])
 separator_load=next(int.from_bytes(target.read(pc,4),'little')for pc in range(tail-base,interpolate[0],4)if int.from_bytes(target.read(pc,4),'little')&0xffc003ff==0xf9400370)
 for recommended,identity,user in [(True,-1,0),(True,128,10),(False,-1,0),(False,128,10)]:
  u=setup();fp=u.reg_read(regs[29]);sp=u.reg_read(regs[15])
  model,hero,package,component,separator,prefix,result,array=[heap+x for x in [0x35001,0x36001,0x37001,0x37101,0x37201,0x37301,0x38001,0x39001]]
  u.mem_write(model+7,struct.pack('<q',identity));q(u,model+0x73,user)
  w(u,hero+7,package);w(u,hero+11,component)
  for obj,data in [(package,b'recent_example'if recommended else b'ordinary_example'),(prefix,b'recent_')]:
   w(u,obj+7,len(data)*2);u.mem_write(obj+15,data)
  add,load=struct.unpack('<2I',u.mem_read(address(symbols['source_tag_prefix']),8))
  prefix_offset=immediate(add)+immediate(load);q(u,0x300000+prefix_offset,prefix)
  # Leave every other frame slot null: the current compiler's exact source
  # slot must be replayed, including the shift between the two launchers.
  for off in range(-0x200,-0x100,8):q(u,fp+off,nil)
  hero_word=struct.unpack('<I',u.mem_read(address(symbols['source_tag_hero']),4))[0]
  q(u,fp+immediate(hero_word),hero)
  q(u,0x300000+((separator_load>>10)&4095)*8,separator)
  u.reg_write(regs[1],model);u.reg_write(regs[30],0x3f0000);seen=[]
  def source_call(u,pc,size,data):
   if pc==base+allocate[1]:
    assert u.reg_read(regs[30])==base+allocate[0]+4
    assert u.reg_read(regs[2])==10
    frame=u.reg_read(regs[29]);assert qi(u,frame-0x18)==model and qi(u,frame-0x10)==hero
    assert qi(u,frame-8)==package
    seen.append('allocate');u.reg_write(regs[0],array)
   elif pc==base+interpolate[1]:
    assert u.reg_read(regs[30])==base+interpolate[0]+4
    assert qi(u,u.reg_read(regs[15]))==array
    assert [wi(u,array+off)for off in [15,19,23,27,31]]==[int(v)&0xffffffff for v in [package,separator,component,separator,user*2]]
    seen.append('interpolate');u.reg_write(regs[0],result)
   elif pc==0x3f0000:
    assert u.reg_read(regs[1])==model
    assert u.reg_read(regs[30])==address(0x1b14738)
    seen.append('canonical');u.reg_write(regs[0],result)
   else:return
   u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(regs[30]))
  u.hook_add(UC_HOOK_CODE,source_call)
  u.emu_start(address(0x1b14734),address(0x1b14738),count=10000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==address(0x1b14738)
  assert u.reg_read(regs[0])==result and u.reg_read(regs[29])==fp and u.reg_read(regs[15])==sp
  assert seen==(['allocate','interpolate']if recommended else['canonical'])
 print('PASS recommendation source/user tags; ordinary/desktop dispatch and original Dart safepoints:',Path(planpath).name,flush=True)
 caches=target.names.get('BigIconGetUtil.loadFlightImage',[])
 if caches:
  cache=caches[0][0];capture=target.names['RenderRepaintBoundary.toImage'][0][0]
  for name in ['SfAnimIconCapture._buildLayersFromIconInfo','SfWindowAnimImplementor.prepareIconLayers']:
   start,size,_=target.names[name][0];code=target.read(start,size);calls={}
   for off in range(0,len(code),4):
    word=struct.unpack_from('<I',code,off)[0]
    if word&0xfc000000==0x94000000:calls.setdefault(branch_target(word,start+off),[]).append(start+off)
   assert len(calls[cache])==1 and calls[capture]
   live_capture=min(at for at in calls[capture] if at>calls[cache][0])
   for cached in [nil,heap+0x35001]:
    u=setup();boundary=heap+0x36001;fp=u.reg_read(regs[29])
    for off in range(-1024,0,8):q(u,fp+off,boundary)
    u.reg_write(regs[0],cached)
    u.emu_start(base+calls[cache][0]+4,base+live_capture,count=100)
    assert u.reg_read(R.UC_ARM64_REG_PC)==base+live_capture
    assert u.reg_read(regs[1])==boundary
    assert any(qi(u,fp+off)==nil+0x30 for off in range(-1024,0,8))
  print('PASS current attached boundaries replace cached/static flight images on both animation paths:',Path(planpath).name,flush=True)
 u=setup();cfg=struct.unpack('<31I',u.mem_read(address(0xb33600),124))
 # Match only the first full-size, non-interactive shape in the root group.
 for case in ['plate','color','cloned-view','layout-child','photo','vector','button','sized','nested','later-child','single-child']:
  u=setup();state,view,control,layout,parent,owner,children,items,drawable=[heap+0x20001+i*0x1000 for i in range(9)]
  w(u,state+cfg[25],view);w(u,view+cfg[25],control);w(u,control-1,(cfg[16]<<12)|0x600)
  w(u,control+cfg[19],layout);ds(u,layout+cfg[23],[-1]);ds(u,layout+cfg[24],[-1])
  w(u,control+cfg[22],nil);w(u,control+cfg[20],parent);w(u,parent+cfg[25],owner)
  w(u,owner+cfg[20],nil);w(u,owner+cfg[21],children);w(u,children+cfg[26],6);w(u,children+cfg[27],items);w(u,items+cfg[28],view)
  w(u,drawable-1,((cfg[18] if case=='color' else cfg[17])<<12)|0x600)
  if case in ['photo','vector']:w(u,drawable-1,((cfg[17]+(0x10 if case=='photo'else 0x11))<<12)|0x600)
  if case=='button':w(u,control+cfg[22],heap+0x30001)
  if case=='sized':ds(u,layout+cfg[24],[24])
  if case=='nested':w(u,owner+cfg[20],heap+0x30001)
  if case=='later-child':w(u,items+cfg[28],heap+0x30001)
  if case=='cloned-view':w(u,items+cfg[28],heap+0x30001);w(u,heap+0x30001+cfg[25],control)
  if case=='layout-child':w(u,items+cfg[28],heap+0x30001);w(u,heap+0x30000,(cfg[30]<<12)|0x600);w(u,heap+0x30001+cfg[29],view)
  if case=='single-child':w(u,children+cfg[26],2)
  before=bytes(u.mem_read(heap+0x20000,0x9000));u.reg_write(regs[1],state);u.reg_write(regs[2],drawable);u.reg_write(regs[30],0x400000)
  # Seed the current binary's canonical empty Widget pool slot. A null return
  # crashes FlutterImageViewController.cacheDrawableWidget's non-null map.
  empty=address(symbols['android_empty_widget']);add,ldr=struct.unpack('<2I',u.mem_read(empty,8))
  pool_offset=((add>>10)&4095)*(4096 if add&(1<<22)else 1)+((ldr>>10)&4095)*8
  q(u,0x300000+pool_offset,heap+0x3e001)
  hidden=case in ['plate','color','cloned-view','layout-child'];end=0x400000 if hidden else address(0x10d4bc0)+4
  u.emu_start(address(symbols['android_image_plate']),end,count=10000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==end,case
  if hidden:assert u.reg_read(regs[0])==heap+0x3e001
  assert bytes(u.mem_read(heap+0x20000,0x9000))==before,case
 print('PASS native relocated shape background detection; photos, controls, nested panels and provider state retained:',Path(planpath).name,flush=True)
 for kind,scope in [(1,0x19b0dc8),(2,0x1b136bc),(3,0x199c894)]:
  u=setup();child,root,data,bounds=ns['scene'](u)
  w(u,child-1,(cfg[3]<<12)|0x500);w(u,root-1,(cfg[2]<<12)|0x700)
  picture=heap+0x13001
  w(u,child+0x47,picture);w(u,picture-1,(cfg[15]<<12)|0x600)
  w(u,picture+0x1f,child);w(u,picture+0x37,nil);w(u,picture+0x47,nil)
  w(u,picture+0x23,nil+0x30)
  # The real RenderView.configuration field moved from 0x5b to 0x67.
  qview=heap+0x10701;qconf=heap+0x10801;qphysical=heap+0x10901
  if cfg[10]!=0x5b:w(u,qview+0x5b,nil)
  if cfg[11]!=0xb:w(u,qconf+0xb,nil)
  if cfg[12]!=0x2b:w(u,root+0x2b,nil)
  w(u,root+cfg[12],qview);w(u,qview+cfg[10],qconf);w(u,qconf+cfg[11],qphysical)
  ds(u,qphysical+cfg[13],[1120]);ds(u,qphysical+cfg[14],[2436])
  for at,old in [(0x2fa008,0x191a61c),(0x2fc008,scope)]:q(u,at,address(old))
  # Compiler changed the image local slot in 9226; the gateway must replay it.
  for i in range(32):u.reg_write(getattr(R,f'UC_ARM64_REG_Q{i}'),0x112233445566778899aabbccddeeff00+i)
  vectors=[u.reg_read(getattr(R,f'UC_ARM64_REG_Q{i}'))for i in range(32)]
  u.reg_write(R.UC_ARM64_REG_NZCV,0xa0000000)
  u.emu_start(address(symbols['root_gateway']),address(0xa02918),count=4000000)
  assert u.reg_read(R.UC_ARM64_REG_NZCV)==0xa0000000
  assert qi(u,0x2f9ff8)==root
  assert wi(u,picture+0x23)==(nil+0x20)&0xffffffff
  b=struct.unpack('<4d',u.mem_read(bounds+7,32));assert int(b[0])-1048576==69 and int(b[1])-1048576==802
  assert vectors==[u.reg_read(getattr(R,f'UC_ARM64_REG_Q{i}'))for i in range(32)]
  assert qi(u,0x2faf68)==kind<<29
 # Execute mark, clone and the actual byte-export gateway after compaction.
 u=setup();child,root,data,bounds=ns['scene'](u)
 w(u,child-1,(cfg[3]<<12)|0x500);w(u,root-1,(cfg[2]<<12)|0x700)
 if cfg[10]!=0x5b:w(u,heap+0x10701+0x5b,nil)
 w(u,heap+0x10701+cfg[10],heap+0x10801)
 q(u,0x2fa008,address(0x191a61c));q(u,0x2fc008,address(0x19b0dc8))
 u.emu_start(address(symbols['root_gateway']),address(0xa02918),count=4000000)
 image,native,clone=heap+0x11001,heap+0x11101,heap+0x11201
 w(u,image+7,native);q(u,image+15,1120);q(u,image+23,2436);u.reg_write(regs[29],0x2fb000);u.reg_write(regs[0],image)
 u.emu_start(address(symbols['mark_floor']),address(0x191a750),count=10000)
 assert qi(u,image+15)==171 and wi(u,image+11)>>25==0x15
 u.reg_write(regs[29],0x2fd000);u.reg_write(regs[1],image);u.reg_write(regs[2],171)
 u.emu_start(address(symbols['clone_pack']),address(0xa1e60c),count=1000);packed=qi(u,0x2fcff0)
 w(u,clone+7,native);q(u,clone+23,171);u.reg_write(regs[1],packed);u.reg_write(regs[3],clone)
 u.emu_start(address(symbols['clone_unpack']),address(0xa1e638),count=1000)
 assert wi(u,clone+11)==wi(u,image+11)
 buffer=0x50000000;u.mem_map(buffer,0x1000000);raw=bytearray(1120*2436*4)
 for row in range(802,973):
  for col in range(69,240):struct.pack_into('<I',raw,(row*1120+col)*4,0xff000000|(row<<12)|col)
 u.mem_write(buffer,bytes(raw));array=heap+0x12001;w(u,array-1,(0x75<<12)|0x200);q(u,array+7,buffer);w(u,array+0x13,len(raw)*2)
 u.reg_write(regs[29],0x2fe000);fp_offset=cfg[9]-(1<<32)if cfg[9]>>31 else cfg[9];q(u,0x2fe000+fp_offset,clone);u.reg_write(regs[0],array)
 u.emu_start(address(symbols['bytes_gateway']),address(0x1b15198)+4,count=4000000)
 expected=b''.join(bytes(raw[((802+row)*1120+69)*4:((802+row)*1120+240)*4])for row in range(171))
 assert bytes(u.mem_read(buffer,len(expected)))==expected and wi(u,array+0x13)==len(expected)*2
 # SF's result builder exports directly through mode 2 without bytes_gateway.
 u.mem_write(buffer,bytes(raw));w(u,array+0x13,len(raw)*2)
 u.reg_write(regs[29],0x2ff000);q(u,0x2feff0,clone);u.reg_write(regs[1],array)
 u.emu_start(address(symbols['result_gateway']),address(0x19c0058),count=4000000)
 assert bytes(u.mem_read(buffer,len(expected)))==expected and wi(u,array+0x13)==len(expected)*2
 assert qi(u,0x2fefe8)==array and qi(u,0x2fefd0)==171 and qi(u,0x2fefd8)==171
 # Relocated inline foreground hooks replay the actual compiler's store slots.
 image=ELFFile(open(imagepath,'rb'))
 for label,site,reg in [('android_text_inline',0x10e5e00,4),('android_spanned_text',0xc42d84,3),('android_hint_text',0x10e3798,0)]:
  u=setup();u.reg_write(regs[reg],0x7f22cc99)
  u.emu_start(address(symbols[label]),address(site)+4,count=100)
  va=address(site)-base
  segment=next(s for s in image.iter_segments()if s['p_type']=='PT_LOAD'and s['p_vaddr']<=va<va+4<=s['p_vaddr']+s['p_filesz'])
  image.stream.seek(segment['p_offset']+va-segment['p_vaddr']);store=struct.unpack('<I',image.stream.read(4))[0]
  slot=(store>>12)&511;slot=slot-512 if slot&256 else slot
  assert qi(u,0x2fa000+slot)==0x7fffffff
 for label,site,reg in [('maml_text_color',0x1642578,4),('maml_filter_color',0x163e208,3)]:
  u=setup();col=heap+0x25001;ds(u,col+7,[0.6,0.2,0.4,0.8]);u.reg_write(regs[0],col)
  u.emu_start(address(symbols[label]),address(site)+4,count=100)
  assert struct.unpack('<4d',u.mem_read(col+7,32))==(0.6,1.0,1.0,1.0) and u.reg_read(regs[reg])==col
 for site in [0xf18508,0x10c57cc,0x10c6a60,0x10c7180,0x12908f0,0x163e2a8,0x185ce78,0x10e271c]:
  word=struct.unpack('<I',u.mem_read(address(site),4))[0]
  assert branch_target(word,address(site))==address(0x17458e4)
 assert 7<=cfg[6]<2048 and cfg[6]%4==3
 fp=cfg[9]-(1<<32)if cfg[9]>>31 else cfg[9];assert -2048<=fp<0 and fp%8==0
 print('PASS native relocated viewport/crop, closing marker, vector ABI, compiler store replay:',Path(planpath).name,flush=True)
