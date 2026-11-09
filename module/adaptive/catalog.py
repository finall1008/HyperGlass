import os
"""Compile reference transformations into runtime semantic recipes and relocations."""
from pathlib import Path
from dataclasses import dataclass,field
import json,struct
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from model import Model,Image,branch_target,immediate,words,arm_mask
H=Path(__file__).resolve().parent;ROOT=H.parents[1];EX=ROOT/'adapters';REFS=Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'))
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
  if va==0x1b1495c:
   index=self.model.add(va,frames=False,control=True);anchor=self.model.anchors[index]
   anchor.mode=5;anchor.dependency=self.model.add(0x1b147a4,frames=False,control=True)
   anchor.callee='RenderRepaintBoundary.toImage';anchor.delta=4
   return Target(va,index,0)
  control=va in [0x199c894,0x10b85d0,0x10b9534,0x19bf198,0x19c08e4,0x1b15874]
  return Target(va,self.model.add(va,frames=False if control else None,control=control),0)
 def add_rows(self,rows,style_count=0):
  skip={0x19bfc68,0x19bfc74,0x181131c,0xb32828,0xce275c,0x11f308c}
  for i,r in enumerate(rows):
   va=int(r['vaddr'],16);data=bytes.fromhex(r['replacement'])
   if va in skip or self.unit_at(va):continue
   group=1 if i<style_count else 2
   if va in [0x10e5e00,0xc42d84,0xc42e2c,0xc43c28,0xc43cb4,0x10e3798,0x1642578,0x163e208,0x163e2a8,0xf18508,0x10c57cc,0x10c6a60,0x10c7180,0x10e271c,0x12908f0,0x185ce78,0x185d4a0,0x173d854,0x10d4bc0,0xfc6748,0xfc7638,0xfc84a8,0x148083c,0x1480844,0x1480914,0x1480a00,0x1480b78]:group=4
   control=va in [0x18c3a6c,0x1b147a4,0x148083c,0x1480844,0x1480a00,0x1480b78,0x185d4a0]
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
     if any(u.shadow and (u.source&-4096)<=page<((u.source+len(u.data)+4095)&-4096)for u in self.units):
      unit.relocations.append(Reloc(off,5,Target(page,-1,1),w));continue
     for q in range(pc+4,min(unit.source+len(unit.data),pc+52),4):
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
        _,writes=ins[0].regs_access();name='x'+str(reg)
        if any(ins[0].reg_name(x)==name for x in writes):break
     if not consumers:raise ValueError(('ADRP without address consumer',unit.label,hex(pc)))
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
  for symbol,reference,opcode in [('android_text_inline',0x10e5e00,0xf81d03a4),('android_spanned_text',0xc42d84,0),('android_hint_text',0x10e3798,0),('bytes_gateway',0x1b15198,0x92802cf1),('android_empty_widget',0x113976c,0),('android_empty_widget',0x1139770,0),('source_tag_hero',0x1b13fa4,0),('source_tag_prefix',0x9ebd4c,0),('source_tag_prefix',0x9ebd50,0)]:
   pc=syms[symbol];unit=self.unit_at(pc);assert unit
   old=struct.unpack('<I',self.model.image.read(reference,4))[0]
   positions=[]
   for q in range(pc,min(pc+40,unit.source+len(unit.data)),4):
    if struct.unpack_from('<I',unit.data,q-unit.source)[0]==old:positions.append(q)
   if len(positions)!=1:raise ValueError(('missing replay',symbol,positions,hex(old)))
   q=positions[0];unit.relocations.append(Reloc(q-unit.source,8,Target(reference,self.model.add(reference,frames=False if symbol.startswith('source_tag_') else None),0),old))
 def add_config(self):
  self.config_source=0xb33600
  specs=[(0,0xfb6228),(1,0xfc55cc),(2,0xa07d04),(3,0x1038114),(4,0x1037d80),(5,0x14ebf74),(7,0xc1e528),(8,0xc19da0),(15,0xfaa65c),(17,0x18d99a4),(18,0x18d994c),(30,0xc2e648)]
  for slot,pc in specs:
   w=struct.unpack('<I',self.model.image.read(pc,4))[0];callee=branch_target(w,pc);first,second,_=struct.unpack('<3I',self.model.image.read(callee,12));tag=immediate(first)|(((second>>5)&65535)<<(((second>>21)&3)*16))
   anchor=self.model.add(pc,frames=False,control=pc==0xfc55cc);self.config.append(Config(slot,1,7 if slot==8 else -1,anchor,tag))
  for slot,pc in [(6,0x19bf008),(9,0x1b151a8),(10,0xa07b58),(11,0xade798),(12,0x15c7ddc),(13,0xade994),(14,0xade920),(16,0xbf6c08),(19,0xe02fcc),(20,0xe036e0),(21,0xc2e138),(22,0xc5cf50),(23,0xc0f434),(24,0xc0f4ac),(25,0xbf6bdc),(26,0xc2e148),(27,0xc2e14c),(28,0xc2e198)]:self.config.append(Config(slot,2,-1,self.model.add(pc,frames=False)))
  self.config.append(Config(29,2,-1,self.model.add(0x15e183c,frames=False)))
 def report(self,target):
  cache={};missing=[]
  for i,a in enumerate(self.model.anchors):
   if self.model.resolve(target,i,cache)is None:missing.append((i,hex(a.va),a.owner,a.mode))
  return cache,missing
 def emit(self,name):
  out=[]
  def array(label,values,ctype):out.append('static const '+ctype+' '+label+'[]={'+(','.join(values)if values else'0')+'};')
  for i,a in enumerate(self.model.anchors):
   array(f'{name}_v{i}',[hex(w)for w in a.values],'uint32_t');array(f'{name}_m{i}',[hex(w)for w in a.masks],'uint32_t')
  array(name+'_anchors',['{'+','.join([json.dumps(a.owner),hex(a.va),str(a.before),str(len(a.values)),str(a.mode),str(a.dependency),str(a.qualifier),json.dumps(a.callee),str(a.delta),f'{name}_v{i}',f'{name}_m{i}'])+'}'for i,a in enumerate(self.model.anchors)],'hg::Anchor')
  for i,u in enumerate(self.units):
   array(f'{name}_b{i}',[hex(b)for b in u.data],'uint8_t')
   if u.relocations:array(f'{name}_r{i}',['{'+','.join([str(r.offset),str(r.kind),'{'+','.join([hex(r.target.source),str(r.target.anchor),str(r.target.kind)])+'}',hex(r.original)])+'}'for r in u.relocations],'hg::Relocation')
  array(name+'_units',['{'+','.join([json.dumps(u.label),hex(u.source),str(len(u.data)),str(u.anchor),str(u.group),str(int(u.shadow)),f'{name}_b{i}',f'{name}_r{i}'if u.relocations else'nullptr',str(len(u.relocations))])+'}'for i,u in enumerate(self.units)],'hg::Unit')
  if self.config:array(name+'_config',['{'+','.join([str(x.slot),str(x.kind),str(x.subtract),str(x.anchor),hex(x.source_tag)])+'}'for x in self.config],'hg::Config')
  groups=sorted({u.group for u in self.units if u.group&(u.group-1)==0});array(name+'_groups',[str(g)for g in groups],'uint32_t')
  if self.constraints:array(name+'_constraints',['{'+','.join(map(str,c))+'}'for c in self.constraints],'hg::Constraint')
  image=self.model.image.path.name
  out.append('static const hg::Program '+name+'={'+','.join([json.dumps(image),name+'_anchors',str(len(self.model.anchors)),name+'_units',str(len(self.units)),name+'_config'if self.config else'nullptr',str(len(self.config)),hex(self.config_source),name+'_groups',str(len(groups)),name+'_constraints'if self.constraints else'nullptr',str(len(self.constraints))])+'};')
  return '\n'.join(out)

def catalogs():
 a=Catalog(REFS/'launcher-801027722/libapp.so')
 a.add_elf(EX/'icons/processor.elf',['.text.helper','.text'],1)
 a.add_elf(EX/'transitions/boundary.elf',['.message','.selector','.initialize'],2,physical=(0x1722030,))
 a.add_elf(EX/'transitions/capture.elf',['.capture_root','.hooks','.capture_small','.limit_test'],2)
 a.add_elf(EX/'transitions/capture.elf',['.text_color','.widget_helpers'],4)
 a.add_elf(EX/'transitions/capture.elf',['.capture_config','.capture_meta','.bridge'],6)
 rows=json.loads((EX/'transitions/profile.json').read_text())['patches'];a.add_rows(rows,len(json.loads((EX/'icons/profile.json').read_text())['patches']))
 a.add_config()
 for name,group in [('ImageFilterGroup.processAll',1),('IconRenderer.renderThemed',1),('OffsetLayer.toImage',2),('OffsetLayer._createSceneForImage',2),('IconCaptureUtil._imageToResult',2),('ShortcutIconWidget._addMessage',2),('AppInfoModel.getcomponentNameString',2)]:
  function=a.model.image.names[name][0];pc=function[0]+8;anchor=a.model.add(pc,frames=True)
  a.units.append(Unit('Dart frame ABI: '+name,pc,b'',group,False,anchor))
 a.constraints.append((2,a.model.add(0x1722088),a.model.add(0x1722030),0x58))
 # Core Dart object offsets used by allocation-free adapters must retain their
 # ABI. Resolve their accessor instructions semantically, then check offsets.
 for pc in [0x15edb70,0x15edba0,0x11f8168,0x11f81ac,0xa030a4,0xa03338,0xa03350,0xfa9b7c,0xfa9cb0,0xfb4d50,0x1562774,0x191a5f4,0x191a5f8]:
  value=immediate(struct.unpack('<I',a.model.image.read(pc,4))[0])
  a.constraints.append((2,a.model.add(pc,frames=False),-1,value))
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
 for pc in [0xdf1e94,0xdf3a14,0xdf3a18,0xdf1dd4,0xdf1dd8,0xddda40,0xddda90,0xd5e260]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0]);e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 # Native object and virtual-call ABI used by the GPU copy. A changed ABI
 # removes the complete snapshot feature before its marker can reach a queue.
 for pc in [0xeaf184,0xeaf2dc,0xc39d4c,0xc39db4,0xc2fc28,0xc2fb68,0xc2fc54,0xc39724,0xc39740,0xc39750,0xc39768,0xc53f20,0x6d4d18,0xb44858,0xb44010,0xb44028,0xb4d8d0,0xc3992c]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0])
  e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 for pc in [0xeaf1a4,0xe81f34,0xc5e134,0xc5e10c,0xc5dde0,0xe9dbd8,0xe9dbf4,0xb44038]:
  value=immediate(struct.unpack('<I',e.model.image.read(pc,4))[0]);e.constraints.append((2,e.model.add(pc,frames=False),-1,value))
 a.constraints.append((2,a.model.add(0x18c643c,frames=False),-1,7))
 for pc,value in [(0x8f1ca0,7),(0x8f1cb0,7),(0x8f1cb4,0)]:a.constraints.append((2,a.model.add(pc,frames=False),-1,value))
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
   for family in ['801027722','801029226']:
    path=REFS/f'launcher-{family}/libapp.so'
    if path.exists():
     target=Image(path);cache,missing=c.report(target);print(family,'missing',missing,flush=True);assert not missing,(family,missing)
