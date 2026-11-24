import os
"""Compile reference transformations into runtime semantic recipes and relocations."""
from pathlib import Path
from dataclasses import dataclass,field
import json,struct
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from model import Model,Image,branch_target,immediate,words,arm_mask,SCENE_FLOOR,SCENE_CEIL
H=Path(__file__).resolve().parent;ROOT=H.parents[1];EX=ROOT/'adapters';REFS=Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'))
# Minimum allocation size class (16-byte units) of config classes whose
# reference layout is larger than the fields used by the adapters.
ADAPTER_MIN_SIZE={1:7,17:8,18:5,37:6}
@dataclass
class Target:
 source:int=0
 anchor:int=-1
 kind:int=0
@dataclass
class Reloc:
 offset:int
 kind:int
 target:Target
 original:int
@dataclass
class Unit:
 label:str
 source:int
 data:bytes
 group:int
 shadow:bool
 anchor:int=-1
 relocations:list=field(default_factory=list)
 code_ranges:list=field(default_factory=list)
 guard:int=-1
 when_guard_matches:bool=True
@dataclass
class Config:
 slot:int
 kind:int
 subtract:int
 anchor:int
 source_tag:int=0

class Catalog:
 def __init__(self,image):
  self.model=Model(image);self.units=[];self.config=[];self.config_source=0;self.constraints=[]
  self.widget_wrap=None
  self.md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);self.md.detail=True
 def add_elf(self,path,sections,group,physical=()):
  e=ELFFile(open(path,'rb'));symbols=list(e.get_section_by_name('.symtab').iter_symbols())
  for name in sections:
   sec=e.get_section_by_name(name);start=sec['sh_addr'];data=sec.data();unit=Unit(name,start,data,group,start not in physical)
   if not unit.shadow:unit.anchor=self.model.add(start)
   markers=sorted((s['st_value'],s.name.startswith('$x'))for s in symbols if s.name.startswith(('$x','$d'))and s['st_shndx']==e.get_section_index(name))
   for i,(at,code)in enumerate(markers):
    end=markers[i+1][0]if i+1<len(markers)else start+len(data)
    if code:unit.code_ranges.append((at,end))
   if not markers:unit.code_ranges=[(start,start+len(data))]
   self.units.append(unit)
 def unit_at(self,va):return next((u for u in self.units if u.source<=va<u.source+len(u.data)),None)
 def target(self,va,shadow_page=False):
  if shadow_page:return Target(va,-1,1)
  unit=self.unit_at(va)
  if unit:
   if unit.shadow:return Target(va,-1,1)
   return Target(va-unit.source,unit.anchor,3)
  if self.widget_wrap and va==0xb113c0:return Target(0,self.widget_wrap[1],2)
  if va==0x1c89e44:
   index=self.model.add(va,frames=False,control=True);anchor=self.model.anchors[index]
   anchor.mode=5;anchor.dependency=self.model.add(0x1c89c3c,frames=False,control=True)
   anchor.callee='RenderRepaintBoundary.toImage';anchor.delta=4
   return Target(va,index,0)
  control=va in [0x1b18ac0,0x1184418,0x118526c,0x1b49f2c,0x1c8ae94]
  return Target(va,self.model.add(va,frames=False if control else None,control=control),0)
 def add_rows(self,rows,style_count=0):
  for i,r in enumerate(rows):
   va=int(r['vaddr'],16);data=bytes.fromhex(r['replacement'])
   if self.unit_at(va):continue
   group=1 if i<style_count else 2
   if va in [0x1592d58,0x1592e94]:group=32
   if va in [0x105ca20,0xd0ebb0,0xd0ec58,0xd0fa54,0xd0fae0,0xd0fae4,0x11b854c,0x180b6e8,0x1196cfc,0x18885a0,0x11971e4,0x10b5a98,0x10b64d4,0x10b7588,0x1588534,0x158853c,0x1588638,0x1588730,0x15888a8]:group=4
   # Optional artwork paths share the setting and the widget adapters, with
   # separate ABI checks for vector and compound drawables.
   if va in [0x18072a8,0x1807348,0xc5c000,0x119800c,0x1199580,0x1199b64,0x19b08dc,0x19b0f04]:group=16
   if va==0x137a9fc:group=64
   if va==0x11b72c0:group=128
   control=va in [0x1a2518c,0x1c89c3c,0x1588534,0x158853c,0x1588730,0x15888a8,0x19b0f04]
   anchor=self.model.add(va,frames=False if control else None,control=control)
   unit=Unit(r.get('reason',hex(va)),va,data,group,False,anchor,code_ranges=[(va,va+len(data))]);self.units.append(unit)
 def relocate(self):
  for unit in self.units:
   code={pc for a,b in unit.code_ranges for pc in range(a,b,4)};done=set()
   for pc in sorted(code):
    off=pc-unit.source
    if off+4>len(unit.data):continue
    w=struct.unpack_from('<I',unit.data,off)[0];destination=branch_target(w,pc)
    if destination is not None:
     kind=1 if w&0x7c000000==0x14000000 else 3 if w&0x7e000000==0x36000000 else 2
     target=None
     if not unit.shadow and len(unit.data)==4:
      old=struct.unpack('<I',self.model.image.read(unit.source,4))[0]
      if branch_target(old,unit.source)==destination:target=Target(0,unit.anchor,2)
     if target is None:target=self.target(destination)
     unit.relocations.append(Reloc(off,kind,target,w));continue
    if w&0x3b000000==0x18000000:
     delta=(w>>5)&0x7ffff;delta=delta-(1<<19)if delta&(1<<18)else delta
     unit.relocations.append(Reloc(off,2,self.target(pc+delta*4),w));continue
    if w&0x1f000000==0x10000000:
     delta=((w>>29)&3)|(((w>>5)&0x7ffff)<<2);delta=delta-(1<<21)if delta&(1<<20)else delta
     if not w&(1<<31):unit.relocations.append(Reloc(off,4,self.target(pc+delta),w));continue
     page=(pc&-4096)+delta*4096;reg=w&31;consumers=[]
     for q in range(pc+4,unit.source+len(unit.data),4):
      if q not in code:break
      v=struct.unpack_from('<I',unit.data,q-unit.source)[0];rn=(v>>5)&31
      is_add=v&0x1f000000==0x11000000 and not(v&(1<<30))and not(v&(1<<22))
      is_load=v&0x3b000000==0x39000000
      if rn==reg and(is_add or is_load):
       scale=4 if(v&(1<<26)and v&(1<<23))else v>>30
       low=((v>>10)&4095)*(1 if is_add else 1<<scale);target=page+low
       consumers.append((q,6 if is_add else 7,target,v))
       if(v&31)==reg:break
      else:
       ins=list(self.md.disasm(struct.pack('<I',v),q))
       if ins:
        _,writes=ins[0].regs_access();names=(f'x{reg}',f'w{reg}')
        if any(ins[0].reg_name(x) in names for x in writes):break
     if not consumers:raise ValueError(('ADRP without address consumer',unit.label,hex(pc)))
     # A reference page can contain both adapter code and an original Dart
     # return PC. Classify the address used by ADD/LDR, not the whole page.
     all_shadow=all((self.unit_at(t)and self.unit_at(t).shadow)for _,_,t,_ in consumers)
     if all_shadow:
      unit.relocations.append(Reloc(off,5,Target(page,-1,1),w))
     else:
      if len(consumers)!=1:raise ValueError(('multiple external page consumers',unit.label,hex(pc)))
      q,kind,t,v=consumers[0];target=self.target(t)
      unit.relocations.append(Reloc(off,5,target,w));unit.relocations.append(Reloc(q-unit.source,kind,target,v));done.add(q)
  # Replay current compiler frame slots and canonical empty-widget pool loads.
  if self.model.image.path.name!='libapp.so':return
  linked=ELFFile(open(EX/'transitions/capture.elf','rb'));syms={s.name:s['st_value']for s in linked.get_section_by_name('.symtab').iter_symbols()}
  for symbol,reference in [('android_text_inline',0x105ca20),('android_spanned_text',0xd0ebb0),('android_hint_text',0x11b854c),('bytes_gateway',0x1c8a790),('android_empty_widget',0x1219d58),('android_empty_widget',0x1219d5c),('source_tag_hero',0x1c89438),('source_tag_prefix',0xa1f258),('source_tag_prefix',0xa1f25c),('mark_floor_store',0x1a7f0d4),('mark_ceil_store',0x1a7f244)]:
   pc=syms[symbol];unit=self.unit_at(pc);assert unit
   old=struct.unpack('<I',self.model.image.read(reference,4))[0]
   positions=[]
   for q in range(pc,min(pc+40,unit.source+len(unit.data)),4):
    if struct.unpack_from('<I',unit.data,q-unit.source)[0]==old:positions.append(q)
   if len(positions)!=1:raise ValueError(('missing replay',symbol,positions,hex(old)))
   q=positions[0];unit.relocations.append(Reloc(q-unit.source,8,Target(reference,self.model.add(reference,frames=False if symbol.startswith('source_tag_') else None),0),old))
   if symbol in ('android_text_inline','android_spanned_text','android_hint_text'):
    color_mask=struct.unpack_from('<I',unit.data,q-unit.source-4)[0]
    unit.relocations.append(Reloc(q-unit.source-4,9,Target(reference,self.model.add(reference),0),color_mask))
  default_text=self.unit_at(0xd0ec58)
  default_text.relocations.append(Reloc(0,10,Target(0xd0eba8,self.model.add(0xd0eba8),0),0))
  span_store=0xd0fae4;span=self.unit_at(syms['span_direct'])
  span.relocations.append(Reloc(4,8,Target(span_store,self.model.add(span_store),0),0))
  for symbol,reference in [('badge_text_replay',0x1592d58),('badge_white_pool',0x1592d4c)]:
   pc=syms[symbol];unit=self.unit_at(pc);assert unit
   for offset in range(0,8 if symbol=='badge_white_pool'else 4,4):
    old=struct.unpack('<I',self.model.image.read(reference+offset,4))[0]
    unit.relocations.append(Reloc(pc+offset-unit.source,8,Target(reference+offset,self.model.add(reference+offset),0),old))
 def add_config(self):
  self.config_source=0x2003600
  specs=[(0,0x10a2e04),(1,0x10b4db8),(2,0xab6638),(3,0x112c4f0),(4,0x112c0a8),(5,0x15f1508),(7,0xcd39a4),(8,0xccf124),(15,0x1096e9c),(17,0x1a3c118),(18,0x1a3c0c0),(30,0xcdf434)]
  # Clip and child-only filter classes come from their PaintingContext
  # allocators. The same layer links are checked by the snapshot ABI rules.
  specs.extend([(35,0x109f828),(36,0x10a83ec),(37,0x10a052c),(38,0x10a31f0)])
  for slot,pc in specs:
   w=struct.unpack('<I',self.model.image.read(pc,4))[0];callee=branch_target(w,pc);first,second,_=struct.unpack('<3I',self.model.image.read(callee,12));tag=immediate(first)|(((second>>5)&65535)<<(((second>>21)&3)*16))
   # The ClipRRect constructor can initialize additional fields after BL.
   # Its reuse guard and the allocation stub establish the class.
   window=(8,1) if pc==0x10a052c else None
   anchor=self.model.add(pc,frames=False,control=pc==0x10b4db8,window=window)
   # The tag check requires at least the instance size the adapters access.
   # Some reference classes appended fields that the adapters never read.
   if slot in ADAPTER_MIN_SIZE:tag=(tag&~0xf00)|(ADAPTER_MIN_SIZE[slot]<<8)
   self.config.append(Config(slot,1,7 if slot==8 else -1,anchor,tag))
  for slot,pc in [(6,0x1b482c4),(9,0x1c8a7a0),(10,0xab648c),(11,0xb5eca0),(12,0x16df240),(13,0xb5ee9c),(14,0xb5ee28),(16,0xca6590),(19,0xebf414),(20,0x18575dc),(21,0x1838808),(22,0xd2c464),(23,0xcc4658),(24,0xcc46d0),(25,0xca6564),(26,0x1838818),(27,0x183881c),(28,0x1838868)]:self.config.append(Config(slot,2,-1,self.model.add(pc,frames=False)))
  self.config.append(Config(29,2,-1,self.model.add(0x16f6b04,frames=False)))
 def report(self,target):
  cache={}
  for i in range(len(self.model.anchors)):self.model.resolve(target,i,cache)
  required={c.anchor for c in self.config}
  for _,left,right,*_ in self.constraints:required.update((left,right))
  for unit in self.units:
   if unit.guard>=0 and (cache[unit.guard] is not None)!=unit.when_guard_matches:continue
   required.add(unit.anchor)
   required.update(r.target.anchor for r in unit.relocations)
  missing=[(i,hex(a.va),a.owner,a.mode)for i,a in enumerate(self.model.anchors)if i in required and cache[i] is None]
  return cache,missing
 def emit(self,name):
  out=[]
  def array(label,values,ctype):out.append('static const '+ctype+' '+label+'[]={'+(','.join(values)if values else'0')+'};')
  for i,a in enumerate(self.model.anchors):
   array(f'{name}_v{i}',[hex(w)for w in a.values],'uint32_t');array(f'{name}_m{i}',[hex(w)for w in a.masks],'uint32_t')
   if a.relations:array(f'{name}_rr{i}',['{'+','.join(map(str,r))+'}'for r in a.relations],'hg::RegisterRelation')
  array(name+'_anchors',['{'+','.join([json.dumps(a.owner),hex(a.va),str(a.before),str(len(a.values)),str(a.mode),str(a.dependency),str(a.qualifier),json.dumps(a.callee),str(a.delta),f'{name}_v{i}',f'{name}_m{i}',f'{name}_rr{i}'if a.relations else'nullptr',str(len(a.relations)),str(int(a.follow_helpers))])+'}'for i,a in enumerate(self.model.anchors)],'hg::Anchor')
  for i,u in enumerate(self.units):
   array(f'{name}_b{i}',[hex(b)for b in u.data],'uint8_t')
   if u.relocations:array(f'{name}_r{i}',['{'+','.join([str(r.offset),str(r.kind),'{'+','.join([hex(r.target.source),str(r.target.anchor),str(r.target.kind)])+'}',hex(r.original)])+'}'for r in u.relocations],'hg::Relocation')
  array(name+'_units',['{'+','.join([json.dumps(u.label),hex(u.source),str(len(u.data)),str(u.anchor),str(u.group),str(int(u.shadow)),f'{name}_b{i}',f'{name}_r{i}'if u.relocations else'nullptr',str(len(u.relocations)),str(u.guard),str(int(u.when_guard_matches))])+'}'for i,u in enumerate(self.units)],'hg::Unit')
  if self.config:array(name+'_config',['{'+','.join([str(x.slot),str(x.kind),str(x.subtract),str(x.anchor),hex(x.source_tag)])+'}'for x in self.config],'hg::Config')
  groups=sorted({u.group for u in self.units if u.group&(u.group-1)==0});array(name+'_groups',[str(g)for g in groups],'uint32_t')
  if self.constraints:array(name+'_constraints',['{'+','.join(map(str,c))+'}'for c in self.constraints],'hg::Constraint')
  image=self.model.image.path.name
  if self.widget_wrap:out.append('static const hg::WidgetWrap '+name+'_widget_wrap={'+','.join(map(str,self.widget_wrap))+'};')
  out.append('static const hg::Program '+name+'={'+','.join([json.dumps(image),name+'_anchors',str(len(self.model.anchors)),name+'_units',str(len(self.units)),name+'_config'if self.config else'nullptr',str(len(self.config)),hex(self.config_source),name+'_groups',str(len(groups)),name+'_constraints'if self.constraints else'nullptr',str(len(self.constraints)),'&'+name+'_widget_wrap'if self.widget_wrap else'nullptr'])+'};')
  return '\n'.join(out)

def widget_anchors(model):
 # Validate the Dart tagged-int unbox and three channel shifts as one
 # sequence. Register assignments may move together, including into a helper.
 argb_masks=(0xfffffc00,0xffffffe0,0xfffffc00,0xffe00fe0,0xfffffc00,0xfffffc00,0xffe00fe0,0xfffffc00,0xffe00fe0)
 argb_relations=((0,5,1,0),(0,5,2,5),(0,0,2,0),(0,0,3,0),(0,0,4,5),(0,0,5,5),(0,0,7,5),(5,0,6,0),(7,0,8,0))
 for store,prologue,helper in [(0x105ca20,False,True),(0xd0ebb0,True,False),(0x11b854c,False,False)]:
  masks=argb_masks;relations=argb_relations;start=store-12
  if prologue:
   start-=12
   masks=(0xfffffc1f,0xff00001f,0xffc003ff)+masks
   relations=tuple((i+3,a,j+3,b)for i,a,j,b in relations)+((0,5,3,5),)
  for pc in (store,store+4):model.operand_sequence(pc,start,masks,relations,follow_helpers=helper)
  if prologue:
   for pc in (store-16,store-8):model.operand_sequence(pc,start,masks,relations)
 index=model.branch_destination(0xd0ec58,0xd0ebb0,-20,(0,2));a=model.anchors[index]
 a.masks=(0xffc003e0,0xffc00000);a.relations=((0,0,1,0),(0,0,1,5))
 for pc in (0x18885a0,0x18885a4):model.add(pc,window=((pc-0x18885a0)//4,(0x18885b4-pc)//4))
 for pc in [0x10b5a98,0x10b64d4,0x10b7588]:
  model.named_call(pc,'RenderViewDecorationMixin.paintViewBackground')
 model.add(0xccf124,frames=False,window=(5,4))
 model.add(0xcdf434,frames=False,window=(5,5))
 # addChild can gain an insertion path before the original append. Match
 # the owner-to-list load and both array accesses by their shared register.
 start=0x1838808
 list_masks=(0xffe0001f,0xfffffc00,0xfffffc00,0xffe00fe0,0xffe00c1f,0xffe00c1f,0xfffffc00)
 list_relations=((1,0,2,0),(1,0,2,5),(1,0,3,0),(1,0,4,5),(1,0,5,5),(5,0,6,0),(5,0,6,5))
 for pc in (start,0x1838818,0x183881c):model.operand_sequence(pc,start,list_masks,list_relations)
 for pc in (0x119800c,0x1199580):model.add(pc,frames=False,window=(4,4))
 model.named_call(0xc5c000,'Drawable.updateBlendModeFilter',follow_helpers=True)
 call=model.named_call(0xd0fa50,'ForegroundColorSpanImpl.color')
 model.after_call(0xd0fae4,call,4,(0,6))
 model.after_call(0xd0fae8,call,8,(1,5))

def snapshot_anchors(model):
 # The early class guard and the legacy bitmap flag both reject to the
 # geometry checks. Only register roles and the local branch shape matter.
 model.operand_sequence(0xeb9300,0xeb92f0,
  (0xfffffc00,0xfffffc00,0xff800000,0xffc0001f,0xffffffff,
   0xffe00c00,0xfffffc00,0xffffffe0,*([0xffffffff]*4),0xfffffc00),
  ((0,0,1,5),(1,0,2,5),(2,0,3,5),(0,5,5,5),(5,0,6,5),(6,0,7,0),(0,5,12,5)))
 # Find the one-cell decision and the positive exit from the final
 # dispatcher separately. Other geometry checks can appear between them.
 model.operand_sequence(0xeb93a4,0xeb9384,
  (0xffffffe0,0xfffffc1f,0xffffffff,0xfffffc00,0xfffffc1f,
   0xffffffff,0xfffffc00,0xfffffc1f,*([0xffffffff]*5)),
  ((0,0,1,5),(3,0,4,5),(3,5,6,5),(6,0,7,5)))
 model.operand_sequence(0xeb940c,0xeb93e4,
  (0xfffffc00,0xfffffc00,0xff80001f,*([0xffffffff]*11)),
  ((0,0,1,5),(1,0,2,5)))
 for pc in (0x1b18ac0,0x1b48494,0x1b49f2c,0x1c8ae94):
  model.named_call(pc,'RenderRepaintBoundary.toImage',delta=4)
 model.named_call(0x1a7efa0,'OffsetLayer._createSceneForImage',delta=4)
 for pc,mode in ((0x1a7f0d4,SCENE_FLOOR),(0x1a7f244,SCENE_CEIL)):
  model.scene_mark(pc,mode,0)
  model.scene_mark(pc+4,mode,4)

def catalogs():
 a=Catalog(REFS/'launcher-801030308/libapp.so')
 widget_anchors(a.model)
 snapshot_anchors(a.model)
 a.add_elf(EX/'icons/processor.elf',['.text.helper','.text'],1)
 a.add_elf(EX/'transitions/boundary.elf',['.message','.selector','.initialize'],2,physical=(0x1868e9c,))
 a.add_elf(EX/'transitions/capture.elf',['.capture_root','.hooks','.capture_small','.limit_test','.maml_repaint'],2)
 a.add_elf(EX/'transitions/capture.elf',['.material_scene','.material_scroll','.detached_capture'],2)
 a.add_elf(EX/'transitions/capture.elf',['.text_color','.widget_helpers'],4)
 a.add_elf(EX/'transitions/capture.elf',['.widget_spans'],4)
 a.add_elf(EX/'transitions/capture.elf',['.badge'],32)
 # The allocation-free black text adapter writes only a fresh Color's RGB.
 for pc in [0x188cde8,0x188ce08,0x188ce24,0x188ce40]:
  value=immediate(struct.unpack('<I',a.model.image.read(pc,4))[0])
  a.constraints.append((32,a.model.add(pc),-1,value))
 a.add_elf(EX/'transitions/capture.elf',['.capture_config','.capture_meta','.bridge'],6)
 rows=json.loads((EX/'transitions/profile.json').read_text())['patches'];a.add_rows(rows,len(json.loads((EX/'icons/profile.json').read_text())['patches']))
 span_guard=a.model.add(0xd0fae4)
 for pc,when in [(0xd0fa54,False),(0xd0fae0,False),(0xd0fae4,True),(0x2009040,True)]:
  unit=a.unit_at(pc);unit.guard=span_guard;unit.when_guard_matches=when
 a.add_config()
 # The opacity adapter compares actual return PCs, even when its callers
 # rearrange the instructions that consume the result.
 for pc in [0x1199524,0x1197fc4,0x11a2c40,0x11a5f0c]:
  a.model.named_call(pc,'_ImageViewState._applyImageAlphaIfNeeded',delta=4)
 for name,group in [('ImageFilterGroup.processAll',1),('IconRenderer.renderThemed',1),('OffsetLayer._createSceneForImage',2),('IconCaptureUtil._imageToResult',2),('ShortcutIconWidget._addMessage',2),('AppInfoModel.getcomponentNameString',2)]:
  function=a.model.image.names[name][0];pc=function[0]+8
  grows=name=='ImageFilterGroup.processAll'
  anchor=a.model.add(pc,frames=not grows)
  if grows:
   recipe=a.model.anchors[anchor]
   # Keep all operand and saved-slot checks except the frame allocation size.
   recipe.masks=tuple(mask if i==recipe.before else arm_mask(word,True)for i,(word,mask)in enumerate(zip(recipe.values,recipe.masks)))
   a.constraints.append((group,anchor,-1,0x170,'hg::ConstraintKind::Minimum'))
  a.units.append(Unit('Dart frame ABI: '+name,pc,b'',group,False,anchor))
 a.constraints.append((2,a.model.add(0x1868ef4),a.model.add(0x1868e9c),0x58))
 # Core Dart object offsets used by allocation-free adapters must retain their
 # ABI. Resolve their accessor instructions semantically, then check offsets.
 for pc in [0x1707008,0x1707038,0x942184,0x9421c8,0xab1878,0xab1b94,0xab1bac,0x10963bc,0x10964f0,0x10a192c,0x16a3c70,0x16a3670]:
  value=immediate(struct.unpack('<I',a.model.image.read(pc,4))[0])
  a.constraints.append((2,a.model.add(pc,frames=False),-1,value))
 # The wrap adapter uses the proven two-call ColorFiltered allocation route.
 # Preserve its Dart frame and validate the full success path and its stores.
 factory=0x11f8f68;call=0x11f8fa4
 masks=tuple(arm_mask(w)for w in words(a.model.image.read(factory,0x7c)))
 for pc in (call,call+8,0x11f8fc0):a.model.operand_sequence(pc,factory,masks,())
 capture=EX/'transitions/capture.elf'
 with capture.open('rb') as stream:
  symbols={s.name:s['st_value']for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()}
 a.widget_wrap=(len(a.units),a.model.by_va[call],a.model.add(0x18a95d8),a.model.by_va[0x1199b64],*(symbols['wrap_'+name]for name in ('compound_widget','filter_dispatch','filter_resume')),symbols['widget_image_filter'])
 a.add_elf(capture,['.widget_wrap'],128)
 a.add_elf(capture,['.widget_image_filter'],16)
 a.relocate()
 e=Catalog(REFS/'launcher-801027722/libhyper_os_flutter.so')
 for line in (EX/'engine-patches.txt').read_text().splitlines():
  va,old,new=line.split();v=int(va,16);data=bytes.fromhex(new);e.units.append(Unit('Native glass backdrop and marked frame scheduling',v,data,2,False,e.model.add(v),code_ranges=[(v,v+len(data))]))
 for label,pc in [('.material-raster',0xeaf160),('.material-acquire',0xc39460),('.material-create-texture',0xb4d834),('.material-create-blit',0xb45578),('.material-add-copy',0xb43fd8),('.material-enqueue',0xb4608c),('.material-texture-size-abi',0xda3ccc),('.material-state-pair-abi',0xc39dc8)]:
  e.units.append(Unit(label,pc,b'',2,False,e.model.add(pc,frames=True)))
 e.units.append(Unit('.material-make-image',0xc5dd7c,b'',2,False,e.model.add(0xc5dd7c,frames=True)))
 e.units.append(Unit('.material-image-ready',0xeaf710,b'',2,False,e.model.add(0xeaf710,frames=True)))
 e.units.append(Unit('.material-draw',0xc2ff38,b'',2,False,e.model.add(0xc2ff38,frames=True)))
 e.units.append(Unit('.material-bind',0xdf1e60,b'',2,False,e.model.add(0xdf1e60,frames=True)))
 for label,pc in [('.material-uniform',0xdf39ec),('.material-buffer-contents',0xddda34),('.material-copy-host',0xdddc40)]:e.units.append(Unit(label,pc,b'',2,False,e.model.add(pc,frames=True)))
 for label,pc in [('.material-cache-try-get',0xd54f84),('.material-cache-try-get-tinted',0xd55958),('.material-cache-remove',0xd5628c)]:
  e.units.append(Unit(label,pc,b'',2,False,e.model.add(pc,frames=True)))
 for pc,value in [(0xd557e0,8),(0xd557f8,0x10),(0xd55810,0x14)]:
  e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 for pc in [0xdf1e94,0xdf3a14,0xdf3a18,0xdf1dd4,0xdf1dd8,0xddda40,0xddda90,0xd5e260]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0]);e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 # Both F16 vertex paths copy the contents' screen matrix into FrameInfo.
 # The resolver validates both uploads through their shared binding call.
 for pc in (0xd5cd40,0xd5d6d4):
  value=0x188
  e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 # Native object and virtual-call ABI used by the GPU copy. A changed ABI
 # removes the complete snapshot feature before its marker can reach a queue.
 for pc in [0xeaf184,0xeaf2dc,0xc39d4c,0xc39db4,0xc2fc28,0xc2fb68,0xc2fc54,0xc39724,0xc39740,0xc39750,0xc39768,0xc53f20,0x6d4d18,0xb44858,0xb44010,0xb44028,0xb4d8d0,0xc3992c]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0])
  e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 for pc in [0xeaf1a4,0xe81f34,0xc5e134,0xc5e10c,0xc5dde0,0xe9dbd8,0xe9dbf4,0xb44038]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0]);e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 a.constraints.append((2,a.model.add(0x1a27cac,frames=False),-1,7))
 for pc,value in [(0x928130,7),(0x928140,7),(0x928144,0)]:a.constraints.append((2,a.model.add(pc,frames=False),-1,value))
 # TransformLayer.addToScene composes this offset with its raw matrix.
 for pc,value in [(0x16a3c68,0x53),(0x16a3c70,0x57),(0x16a3c8c,0x4f)]:
  a.constraints.append((2,a.model.add(pc,frames=False),-1,value))
 # Engine has no Dart replay sites.
 # The same relocation pass is shared, with replay injection limited to AOT.
 Catalog.relocate(e)
 m=Catalog(REFS/'launcher-801027722/librust_maml_sdk.so')
 m.add_elf(EX/'widgets/loader.elf',['.xml_helpers','.xml_attrs','.xml_transform','.xml_data'],8)
 rows=json.loads((EX/'widgets/profile.json').read_text())['patches']
 for r in rows:
  va=int(r['vaddr'],16)
  if va!=0x5182e4:continue
  data=bytes.fromhex(r['replacement']);m.units.append(Unit('Shared MAML DOM parser',va,data,8,False,m.model.add(va),code_ranges=[(va,va+len(data))]))
 Catalog.relocate(m)
 return[a,e,m]
if __name__=='__main__':
 import sys
 result=catalogs();header='#pragma once\n#include "resolver.hpp"\n'+'\n'.join(c.emit('program'+str(i))for i,c in enumerate(result))+'\nstatic const hg::Program* hg_programs[]={&program0,&program1,&program2};\n'
 destination=H.parent/'.build/adaptive_profiles.hpp';destination.parent.mkdir(exist_ok=True);destination.write_text(header)
 for c in result:
  print(c.model.image.path.name,'anchors',len(c.model.anchors),'units',len(c.units),'relocations',sum(len(u.relocations)for u in c.units),flush=True)
  if c.model.image.path.name=='libapp.so':
   # The reference launcher must resolve completely. module/test_host.py
   # reports older launchers with the native resolver.
   cache,missing=c.report(c.model.image);print('reference missing',missing,flush=True);assert not missing,missing
