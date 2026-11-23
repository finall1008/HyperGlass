import os
"""Execute the native resolver's relocated bytes against each target's Dart layout."""
from pathlib import Path
import json,math,struct,sys
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
 detector,size,_=target.names['SceneTransitionDetector.detectSceneTransition'][0]
 sender=target.names['SceneAnimationSignalManager.sendSignal'][0][0]
 gateway=address(symbols['material_scene_signal'])
 u=setup()
 call,=[at for at,raw,group in p['patches']
        if group==2 and detector<=at-base<detector+size and len(raw)==8
        and branch_target(int.from_bytes(bytes.fromhex(raw),'little'),at)==gateway]
 assert branch_target(struct.unpack('<I',target.read(call-base,4))[0],call-base)==sender
 signal,scene=heap+0x13001,heap+0x13101
 u.reg_write(regs[2],signal);u.reg_write(regs[3],scene)
 u.emu_start(call,base+sender,count=2000)
 assert u.reg_read(R.UC_ARM64_REG_PC)==base+sender and u.reg_read(regs[30])==call+4
 assert u.reg_read(regs[2])==signal and u.reg_read(regs[3])==scene and u.reg_read(regs[15])==0x2f0000
 print('PASS material scene bridge retains the original Dart call and signal:',Path(planpath).name,flush=True)
 workspace,workspace_end=target.extent(target.names['WorkspaceGetxController._folderClingController'][0])
 scroll_target=target.names['MamlVisibilitySignals.setScrolling'][0][0]
 gateway=address(symbols['material_workspace_scroll'])
 call,=[at for at,raw,group in p['patches']
        if group==2 and workspace<=at-base<workspace_end and len(raw)==8
        and branch_target(int.from_bytes(bytes.fromhex(raw),'little'),at)==gateway]
 assert address(symbols['material_scroll_target'])==base+scroll_target
 assert branch_target(struct.unpack('<I',target.read(call-base,4))[0],call-base)==scroll_target
 for scrolling in (True,False):
  u=setup()
  value=nil+(0x20 if scrolling else 0x30);u.reg_write(regs[2],value)
  u.emu_start(call,base+scroll_target,count=2000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==base+scroll_target and u.reg_read(regs[30])==call+4
  assert u.reg_read(regs[2])==value and u.reg_read(regs[15])==0x2f0000
 print('PASS workspace scroll bridge observes both bools at the original Dart call:',Path(planpath).name,flush=True)
 # The opening flag still reaches the stock reparent loop and keeps the
 # original press-cache guard. Closing runs the stock release path.
 prepare,size,_=target.names['SfWindowAnimImplementor.prepareIconLayers'][0]
 release=target.names['SfWindowAnimImplementor.releaseIconLayers'][0][0]
 precreate=target.names['SfWindowAnimImplementor._tryReusePreCreated'][0][0]
 reparent=target.names['RustLibApiImpl.crateApiRecentsSfAnimAgentSfAnimAgentReparentIconToHome'][0][0]
 def word(pc):return struct.unpack('<I',target.read(pc,4))[0]
 def calls_to(callee):
  return [pc for pc in range(prepare,prepare+size,4) if word(pc)&0xfc000000==0x94000000
          and branch_target(word(pc),pc)==callee]
 reuse,=calls_to(reparent)
 press,=calls_to(precreate)
 cleanup,=[pc for pc in calls_to(release) if reuse<pc<press]
 (entry,code),=[(at-base,struct.unpack('<4I',bytes.fromhex(raw))) for at,raw,group in p['patches']
                if group==2 and len(raw)==32 and prepare<=at-base<reuse]
 assert entry<reuse and word(entry-4)&0xfff8001f==0x37200000
 guard=press-20
 assert code[:2]==(word(guard-8),word(guard-4))
 assert branch_target(code[2],entry+8)==cleanup-8
 resume=branch_target(code[3],entry+12)
 assert entry+16<resume<reuse
 for opening,destination in [(True,resume),(False,cleanup-8)]:
  u=setup();q(u,u.reg_read(regs[29])+immediate(code[0]),nil+(0x20 if opening else 0x30))
  assert struct.unpack('<I',u.mem_read(base+guard,4))[0]==word(guard)
  u.emu_start(base+entry,base+destination,count=4)
  assert u.reg_read(R.UC_ARM64_REG_PC)==base+destination
 print('PASS existing SF return layers use the stock release and recapture path:',Path(planpath).name,flush=True)
 # Execute the optional badge pair with current pool/frame relocations. Only
 # the fresh text Color changes; shared white and unrelated objects stay intact.
 for alpha in [0.0,0.35,1.0]:
  u=setup();text,white,decoration=[heap+x for x in [0x35001,0x35101,0x35201]]
  ds(u,text+7,[alpha,1,1,1]);ds(u,white+7,[1,1,1,1])
  before=bytes(u.mem_read(white-1,48));u.reg_write(regs[0],text)
  saved=[u.reg_read(r)for r in regs];nzcv=u.reg_read(R.UC_ARM64_REG_NZCV)
  u.emu_start(address(0x148af68),address(0x148af6c),count=100)
  assert struct.unpack('<4d',u.mem_read(text+7,32))==(alpha,0,0,0)
  replay=struct.unpack('<I',u.mem_read(address(symbols['badge_text_replay']),4))[0]
  assert qi(u,saved[29]+immediate(replay))==text
  assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs))
  add,load=struct.unpack('<2I',u.mem_read(address(symbols['badge_white_pool']),8))
  q(u,0x300000+immediate(add)+immediate(load),white)
  u.reg_write(regs[1],decoration);saved=[u.reg_read(r)for r in regs]
  u.emu_start(address(0x148b0a4),address(0x148b0a8),count=100)
  assert u.reg_read(regs[0])==white
  assert all(u.reg_read(r)==saved[i]for i,r in enumerate(regs)if i not in [0,16])
  assert bytes(u.mem_read(white-1,48))==before and u.reg_read(R.UC_ARM64_REG_NZCV)==nzcv
 print('PASS optional badge black text alpha, white background, shared constants and current pool/frame relocations:',Path(planpath).name,flush=True)
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
 scene_layer,scene_bounds,scene_round,scene_view=struct.unpack('<4i',u.mem_read(address(0xb33600)+124,16))
 clip,opacity,rrect,color_filter=struct.unpack('<4I',u.mem_read(address(0xb33600)+140,16))
 scene_return=address(0x191a61c)
 def original_word(pc):return struct.unpack('<I',target.read(pc-base,4))[0]
 assert [scene_layer,scene_bounds,scene_round]==[immediate(original_word(scene_return+offset))for offset in (-16,-12,8)]
 def prepare_scene(u,child,root,bounds,floor):
  # RenderView.configuration moved, along with the scene frame slots.
  view,conf,physical=heap+0x10701,heap+0x10801,heap+0x10901
  w(u,root+cfg[12],view);w(u,view+cfg[10],conf);w(u,conf+cfg[11],physical)
  ds(u,physical+cfg[13],[1120]);ds(u,physical+cfg[14],[2436])
  # Poison the reference slots before setting the current launcher layout.
  for offset in range(-0xa8,-0x78,8):q(u,0x2fb000+offset,nil)
  q(u,0x2fb000+scene_layer,child);q(u,0x2fb000+scene_bounds,bounds)
  q(u,0x2fb000+scene_round,nil+(0x20 if floor else 0x30));q(u,0x2fb000+scene_view,0)
  q(u,0x2fa008,scene_return)
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
 for kind,scope,floor in [(1,0x19b0dc8,True),(2,0x1b136bc,False),(3,0x199c894,True)]:
  u=setup();child,root,data,bounds=ns['scene'](u,floor=floor)
  w(u,child-1,(cfg[3]<<12)|0x500);w(u,root-1,(cfg[2]<<12)|0x700)
  picture=heap+0x13001
  w(u,child+0x47,picture);w(u,picture-1,(cfg[15]<<12)|0x600)
  w(u,picture+0x1f,child);w(u,picture+0x37,nil);w(u,picture+0x47,nil)
  w(u,picture+0x23,nil+0x30)
  prepare_scene(u,child,root,bounds,floor)
  q(u,0x2fc008,address(scope))
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
  assert qi(u,0x2fb000+scene_view)==kind<<29
  rounding=math.floor if floor else math.ceil
  assert rounding((b[2]-b[0])*2.85)==1120 and rounding((b[3]-b[1])*2.85)==2436
  # Use the live frame chain and shifted parent slots in the ancestor hook.
  q(u,0x2f9000,0x2fa000);q(u,0x2f9008,address(0xa02928))
  u.reg_write(regs[29],0x2f9000);u.reg_write(regs[30],0x3f0000);u.reg_write(regs[0],child)
  u.emu_start(address(symbols['limit_test']),0x3f0000,count=2000)
  assert u.reg_read(regs[16])==1
  # Both patched returns mark the image, then replay the current Dart store.
  image,native=heap+0x11001,heap+0x11101
  w(u,image+7,native);q(u,image+15,1120);q(u,image+23,2436)
  u.reg_write(regs[29],0x2fb000);u.reg_write(regs[0],image)
  mark=address(0x191a74c if floor else 0x191a8b8)
  u.emu_start(mark,mark+4,count=10000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==mark+4 and qi(u,image+15)==171
  assert qi(u,0x2fb000+immediate(original_word(mark)))==image
 print('PASS relocated scene slots, floor/ceil writeback, material stage and current capture scopes:',Path(planpath).name,flush=True)
 # The relocated identity call must run the current ELF's leaf with the
 # Dart thread, and the detached export must use the new scene/view slots.
 u=setup();child,root,data,bounds=ns['scene'](u)
 w(u,child-1,(cfg[3]<<12)|0x500);w(u,child+3,0)
 w(u,root-1,(cfg[2]<<12)|0x700)
 prepare_scene(u,child,root,bounds,True)
 q(u,0x2fc008,address(0x19b0dc8))
 thread=0x2f8000
 u.reg_write(regs[26],thread);q(u,thread+0x8b0,17)
 assert address(symbols['dart_layer_hash'])==base+target.names['_getHash'][0][0]
 u.emu_start(address(symbols['root_gateway']),address(0xa02918),count=4000000)
 assert qi(u,0x2f9ff8)==root and wi(u,child+3)
 placement=0x2f0000-112+32
 assert qi(u,placement+64)==child and wi(u,placement+72)==wi(u,child+3)
 w(u,child+0x1f,nil);q(u,0x2f9ff8,child)
 q(u,0x2fc008,address(0x1b136bc));q(u,0x2fb000+scene_view,nil)
 ds(u,bounds+7,[0,0,60,60])
 ds(u,data+0x17,[2.85,0,0,0,0,2.85,0,0,0,0,1,0,-69,-802,0,1])
 u.emu_start(address(symbols['root_gateway']),address(0xa02918),count=4000000)
 assert qi(u,0x2f9ff8)==child and qi(u,0x2fb000+scene_view)==2<<29
 assert int(struct.unpack('<d',u.mem_read(bounds+7,8))[0])-1048576==69
 print('PASS relocated Dart identity leaf and detached return geometry:',Path(planpath).name,flush=True)
 # Both SF routes preserve alpha for plain and child-filtered boundaries.
 # A material found after visiting a filtered sibling still needs the viewport.
 open_scopes=(0x19bf198,0x19c08e4)
 return_scopes=(0x1b1495c,0x1b15874)
 cases=[(scope,closing,shape)
        for closing,scopes in ((False,open_scopes),(True,return_scopes))
        for scope in scopes for shape in ('plain','filtered','material')]
 for scope,closing,shape in cases:
  u=setup();child,root,data,bounds=ns['scene'](u)
  w(u,root-1,(cfg[2]<<12)|0x700)
  first,second,paint,fade,tint,clip_rect,art=(heap+0x13001+i*0x100 for i in range(7))
  w(u,child-1,(cfg[3]<<12)|0x500);w(u,child+0x47,first)
  if shape=='plain':
   layers=((first,cfg[15],child,second,nil),(second,cfg[15],child,nil,nil))
  else:
   layers=(
    (first,cfg[4],child,second,paint),(paint,cfg[15],first,nil,nil),
    (second,clip,child,nil,fade),(fade,opacity,second,nil,tint),
    (tint,color_filter,fade,nil,clip_rect),(clip_rect,rrect,tint,nil,art),
    (art,cfg[0] if shape=='material' else cfg[15],clip_rect,nil,nil),
   )
  for layer,tag,parent,sibling,descendant in layers:
   w(u,layer-1,(tag<<12)|0x600)
   w(u,layer+0x1f,parent);w(u,layer+0x37,sibling);w(u,layer+0x47,descendant)
  prepare_scene(u,child,root,bounds,True)
  q(u,0x2fc008,address(scope))
  if closing:q(u,0x2fd008,address(0x1b136bc))
  original_bounds=bytes(u.mem_read(bounds+7,32))
  original_transform=bytes(u.mem_read(data+0x17,128))
  u.emu_start(address(symbols['root_gateway']),address(0xa02918),count=4000000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==address(0xa02918)
  if shape=='material':
   assert qi(u,0x2f9ff8)==root and qi(u,0x2fb000+scene_view)==(2 if closing else 1)<<29
  else:
   assert qi(u,0x2f9ff8)==child and qi(u,0x2fb000+scene_view)==0
   assert bytes(u.mem_read(bounds+7,32))==original_bounds
   assert bytes(u.mem_read(data+0x17,128))==original_transform
 print('PASS local shortcut snapshots with child filters and viewport fallback for nested materials:',Path(planpath).name,flush=True)
 # Execute mark, clone and the actual byte-export gateway after compaction.
 u=setup();child,root,data,bounds=ns['scene'](u)
 w(u,child-1,(cfg[3]<<12)|0x500);w(u,root-1,(cfg[2]<<12)|0x700)
 if cfg[10]!=0x5b:w(u,heap+0x10701+0x5b,nil)
 w(u,heap+0x10701+cfg[10],heap+0x10801)
 prepare_scene(u,child,root,bounds,True);q(u,0x2fc008,address(0x19b0dc8))
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
 for label,site in [('android_text_inline',0x10e5e00),('android_spanned_text',0xc42d84),('android_hint_text',0x10e3798)]:
  store=struct.unpack('<I',target.read(address(site)-base,4))[0];reg=store&31
  u=setup();u.reg_write(regs[reg],0x7f22cc99)
  u.emu_start(address(symbols[label]),address(site)+4,count=100)
  assert qi(u,0x2fa000+immediate(store))==0x7fffffff
 # Both rich-text layouts retain the supplied alpha, including the old
 # renderer's separate combined-alpha path.
 for alpha in (0,0x7f,0xff):
  u=setup();u.reg_write(regs[0],(alpha<<24)|0x22cc99)
  if anchors[0xc43cb8]:
   site=address(0xc43cb8);store=struct.unpack('<I',target.read(site-base,4))[0]
   u.emu_start(site,site+4,count=100)
   assert qi(u,0x2fa000+immediate(store))==(alpha<<24)|0xffffff
  else:
   site=address(0xc43c28);u.emu_start(site,site+4,count=100)
   assert u.reg_read(regs[1])==(alpha<<24)|0xffffff
   u.reg_write(regs[2],alpha<<24);site=address(0xc43cb4);u.emu_start(site,site+4,count=100)
   assert u.reg_read(regs[0])==(alpha<<24)|0xffffff
 for site in (0xfc5fd0,0xfc69cc,0xfc76c4):
  u=setup();at=address(site)
  original=struct.unpack('<I',target.read(at-base,4))[0]
  assert any(s[0]==branch_target(original,at-base)for s in target.names['RenderViewDecorationMixin.paintViewBackground'])
  u.emu_start(at,at+4,count=10)
  assert u.reg_read(regs[0])==nil
 for label,site,reg in [('maml_text_color',0x1642578,4),('maml_filter_color',0x163e208,3)]:
  u=setup();col=heap+0x25001;ds(u,col+7,[0.6,0.2,0.4,0.8]);u.reg_write(regs[0],col)
  u.emu_start(address(symbols[label]),address(site)+4,count=100)
  assert struct.unpack('<4d',u.mem_read(col+7,32))==(0.6,1.0,1.0,1.0) and u.reg_read(regs[reg])==col
 adapted=p.get('adapted_widget_paths',0)
 if adapted&128:
  filter_call=address(0x111bda0);load_child=address(0x111bdbc);allocate_call=load_child+4
  original_call=struct.unpack('<I',target.read(allocate_call-base,4))[0]
  allocator=branch_target(original_call,allocate_call)
  filter_value,child,widget=[heap+off for off in (0x15001,0x16001,0x17001)]
  return_pc=0x3ef000
  entry=address(symbols['wrap_compound_widget'])
  site,= [at for at,raw,group in p['patches'] if group==128 and len(raw)==8
          and branch_target(struct.unpack('<I',bytes.fromhex(raw))[0],at)==entry]
  u=setup();u.reg_write(regs[2],child);u.reg_write(regs[30],return_pc);calls=[]
  frame=u.reg_read(regs[29]);stack=u.reg_read(regs[15]);native_stack=u.reg_read(R.UC_ARM64_REG_SP)
  def wrap_calls(u,pc,size,data):
   if pc==address(0x17458e4):
    assert u.reg_read(regs[30])==filter_call+4
    assert qi(u,u.reg_read(regs[29])-8)==child
    calls.append('filter');u.reg_write(regs[0],filter_value)
   elif pc==allocator:
    assert u.reg_read(regs[30])==allocate_call+4
    assert qi(u,u.reg_read(regs[29])-0x18)==filter_value
    calls.append('allocator');u.reg_write(regs[0],widget)
   else:return
   u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(regs[30]))
  u.hook_add(UC_HOOK_CODE,wrap_calls);u.emu_start(site,return_pc,count=200)
  assert calls==['filter','allocator'] and u.reg_read(regs[0])==widget
  assert wi(u,widget+0xb)==child&0xffffffff and wi(u,widget+0xf)==filter_value&0xffffffff
  assert u.reg_read(regs[29])==frame and u.reg_read(regs[15])==stack and u.reg_read(R.UC_ARM64_REG_SP)==native_stack
  # Stock calls into the shared factory still dispatch to their original
  # setState and replay the original load of the receiver on return.
  u=setup();fp=u.reg_read(regs[29]);q(u,fp-0x10,child);u.reg_write(regs[1],child)
  set_state=branch_target(struct.unpack('<I',target.read(filter_call-base,4))[0],filter_call)
  def state_return(u,pc,size,data):
   assert u.reg_read(regs[1])==child and u.reg_read(regs[30])==filter_call+4
   u.reg_write(regs[0],nil);u.reg_write(R.UC_ARM64_REG_PC,filter_call+4)
  u.hook_add(UC_HOOK_CODE,state_return,begin=set_state,end=set_state)
  u.emu_start(filter_call,filter_call+8,count=50)
  assert u.reg_read(regs[0])==child
  print('PASS original Dart wrapper keeps both call return PCs, original child, canonical filter, and frame:',Path(planpath).name,flush=True)
 if adapted&16:
  site=address(0x10c7180);resume=site+4
  matrix=address(0x17458e4);getter=0x3f0800
  for background_only,callee in [(True,getter),(False,matrix)]:
   u=setup();state=heap+0x12001;receiver=heap+0x14001;result=heap+0x15001
   q(u,u.reg_read(regs[29])-0x10,state);w(u,state+0x3b,nil+(0x20 if background_only else 0x30))
   q(u,0x320000+8*5,getter);u.reg_write(regs[21],0x320000);u.reg_write(regs[30],5);u.reg_write(regs[1],receiver)
   reached=[]
   def filter_return(u,pc,size,data):
    assert u.reg_read(regs[30])==resume and u.reg_read(regs[1])==receiver
    reached.append(pc);u.reg_write(regs[0],result);u.reg_write(R.UC_ARM64_REG_PC,resume)
   u.hook_add(UC_HOOK_CODE,filter_return,begin=callee,end=callee)
   u.emu_start(site-4,resume,count=20)
   assert reached==[callee] and u.reg_read(regs[0])==result
  print('PASS background-only drawables keep a single matrix and the original getter return PC:',Path(planpath).name,flush=True)
 if adapted&64:
  matrix=address(0x17458e4);filter_value=heap+0x15001
  for name in ('_VectorDrawableBoxState._buildLiveCached','_VectorDrawableBoxState._renderScene'):
   symbol,size,_=target.names[name][0]
   sites=[at for at,raw,group in p['patches'] if group==64 and base+symbol<=at<base+symbol+size]
   select,call,resume=sorted(sites)
   u=setup();assert branch_target(struct.unpack('<I',u.mem_read(call,4))[0],call)==matrix
   assert branch_target(struct.unpack('<I',u.mem_read(select,4))[0],select)==select+12
   join=branch_target(struct.unpack('<I',u.mem_read(resume+4,4))[0],resume+4)
   def matrix_return(u,pc,size,data):
    assert u.reg_read(regs[30])==resume
    u.reg_write(regs[0],filter_value);u.reg_write(R.UC_ARM64_REG_PC,resume)
   u.hook_add(UC_HOOK_CODE,matrix_return,begin=matrix,end=matrix);u.emu_start(call,join,count=20)
   assert u.reg_read(regs[3])==filter_value
  print('PASS vector live renderers replace the original filter and retain the original safepoints:',Path(planpath).name,flush=True)
 if target.names['DensitySemantics.layoutDensity']:
  density=base+target.names['DensitySemantics.physicalDensity'][0][0]
  sources={s[0]+base for name in ('DensitySemantics.layoutDensity','DensitySemantics.dimensionDensity') for s in target.names[name]}
  density_sites=[]
  for at,raw,group in p['patches']:
   word=struct.unpack('<I',bytes.fromhex(raw))[0]if len(raw)==8 else 0
   if group==4 and word&0xfc000000==0x94000000 and branch_target(word,at)==density:density_sites.append(at)
  assert density_sites
  for site in density_sites:
   original=struct.unpack('<I',target.read(site-base,4))[0]
   assert branch_target(original,site)in sources
   u=setup()
   for reg,value in [(R.UC_ARM64_REG_D0,0.95),(R.UC_ARM64_REG_D1,3.0)]:
    u.reg_write(reg,struct.unpack('<Q',struct.pack('<d',value))[0])
   u.emu_start(site,site+4,count=60)
   actual=struct.unpack('<d',struct.pack('<Q',u.reg_read(R.UC_ARM64_REG_D0)))[0]
   assert abs(actual-3.0/0.95)<1e-12
  print('PASS Android resource layout and text use the original host density conversion:',Path(planpath).name,flush=True)
 for group,sites in [(16,[0xf18508,0x10c57cc,0x10c6a60,0x10c7180,0x163e2a8,0x185ce78]),(64,[0x12908f0]),(128,[0x10e271c])]:
  if adapted&group:continue
  if p['ready']&group:
   for site in sites:
    word=struct.unpack('<I',u.mem_read(address(site),4))[0]
    assert branch_target(word,address(site))==address(0x17458e4)
  else:
   assert all(not(patch_group&group)for _,_,patch_group in p['patches'])
 assert 7<=cfg[6]<2048 and cfg[6]%4==3
 fp=cfg[9]-(1<<32)if cfg[9]>>31 else cfg[9];assert -2048<=fp<0 and fp%8==0
 print('PASS native relocated viewport/crop, closing marker, vector ABI, compiler store replay:',Path(planpath).name,flush=True)
