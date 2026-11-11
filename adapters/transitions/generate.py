import os
"""Link full-view SF captures while retaining the original Dart safepoints."""
import json, struct, subprocess, os
from pathlib import Path
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent;ROOT=H.parents[1]
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.build_env import CLANG
CC=CLANG
flags=['-DHG_ADAPTIVE','-Wl,--no-relax']if os.environ.get('HG_ADAPTIVE')else[]
subprocess.run([str(CC),*flags,'-nostdlib','-static',f'-Wl,-T,{H/"boundary.ld"},-e,message,--build-id=none',str(H/'boundary.S'),'-o',str(H/'boundary.elf')],check=True)
subprocess.run([str(CC),*flags,'-Oz','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-fno-pic','-fno-pie','-ffunction-sections','-nostdlib','-static',f'-Wl,-T,{H/"layout.ld"},-e,capture_bridge,--build-id=none',str(H/'bridge.S'),str(H/'hooks.S'),str(H/'capture.c'),'-o',str(H/'capture.elf')],check=True)
source=ELFFile(open(Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'))/'launcher-801027722/libapp.so','rb'))
profile=json.loads((H.parent/'icons/profile.json').read_text());patches=profile['patches']
def read(va,n):
 for s in source.iter_segments():
  if s['p_type']=='PT_LOAD' and s['p_vaddr']<=va and va+n<=s['p_vaddr']+s['p_filesz']:
   source.stream.seek(s['p_offset']+va-s['p_vaddr']);return source.stream.read(n)
 raise ValueError(hex(va))
def patch(va,data,reason):patches.append(dict(vaddr=f'{va:x}',expected=read(va,len(data)).hex(),replacement=data.hex(),reason=reason))
def branch(at,to,link=False):
 delta=to-at;assert not delta%4 and -(1<<27)<=delta<(1<<27)
 return struct.pack('<I',(0x94000000 if link else 0x14000000)|((delta//4)&0x3ffffff))
patch(0xd0fddc,branch(0xd0fddc,0xd0fdfc),'Validate complete shortcut geometry without requiring the legacy bitmap eligibility flag')
patch(0xd0fe80,struct.pack('<I',0x54000000|(((0xd0febc-0xd0fe80)//4)<<5)),'Capture the attached boundary of one-cell shortcuts, including live layered artwork')
boundary=ELFFile(open(H/'boundary.elf','rb'))
for name in ['.message','.selector','.initialize']:
 s=boundary.get_section_by_name(name);patch(s['sh_addr'],s.data(),'Complete keyed icon boundary; preserve allocation PCs and notification wrapping')
patch(0x181131c,branch(0x181131c,0x1811460),'Omit completion diagnostics; retain all state synchronization and releases')
patch(0x14ebf7c,branch(0x14ebf7c,0x1811360),'Set the icon capture key through the existing boundary allocator')
for va in [0x19bf004,0x1b147a4]:patch(va,bytes.fromhex('1f2003d5'),'Use the complete icon boundary for native SF animations')
linked=ELFFile(open(H/'capture.elf','rb'))
limits={'.capture_root':0xb33300-0xb32838,'.widget_helpers':0xb33600-0xb33300,'.capture_meta':0xce29cc-0xce2770,'.hooks':0x11f3180-0x11f30a0,'.capture_small':0x11f3358-0x11f3180,'.limit_test':0x1811460-0x18113a0,'.bridge':0x19bfef0-0x19bfdb0,'.text_color':0x98bea4-0x98bdc0}
if flags:limits['.capture_config']=124
for sec in linked.iter_sections():
 if sec['sh_type']in ['SHT_REL','SHT_RELA']:assert sec['sh_size']==0
 if sec['sh_flags']&2 and sec['sh_size']:assert sec.name in limits,sec.name
for name,limit in limits.items():
 sec=linked.get_section_by_name(name);code=sec.data();assert len(code)<=limit,(name,len(code),limit)
 for off in range(0,len(code),256):patch(sec['sh_addr']+off,code[off:off+256],'Allocation-free full-view capture and per-image crop metadata')
 print(name,len(code),'bytes')
for va in [0xb32828,0xce275c,0x11f308c]:patch(va,bytes.fromhex('e00316aac0035fd6'),'Omit diagnostic string formatting to reserve relocation-free adapters')
symbols={s.name:s['st_value']for s in linked.get_section_by_name('.symtab').iter_symbols()}
sites=[(0x1562730,'capture_transform'),(0xa02914,'root_gateway'),(0xa02928,'refresh_path'),(0x1562380,'force_path'),(0x1562434,'stop_after_path'),(0x1562124,'capture_image_filter'),(0x1563768,'capture_color_filter'),(0x1563970,'capture_shader_mask'),(0x15629c4,'capture_opacity'),(0x191a74c,'mark_floor'),(0x191a8b8,'mark_ceil'),(0x19c0040,'result_gateway'),(0x1b15198,'bytes_gateway'),(0xa1e608,'clone_pack'),(0xa1e634,'clone_unpack')]
for va,name in sites:patch(va,branch(va,symbols[name]),'Keep wallpaper sampling in the full viewport; crop pixels before native SF drawing')
patch(0x1b14734,branch(0x1b14734,symbols['source_controller_tag']),'Resolve recommendation icon controllers using the exact flight source identity')
for va,name in [(0x10e5e00,'android_text_inline'),(0xc42d84,'android_spanned_text'),(0x10e3798,'android_hint_text'),(0x1642578,'maml_text_color'),(0x163e208,'maml_filter_color'),(0x173d854,'android_background'),(0x10d4bc0,'android_image_plate'),(0x10c58c8,'android_image_opacity')]:patch(va,branch(va,symbols[name]),'Common Android widget content: monochrome text and structural background painting with provider outlines intact')
patch(0xc42e2c,struct.pack('<I',0x92800021)+branch(0xc42e30,0xc42d74),'Default uncolored SpannedString roots to monochrome white')
for va,opcode in [(0xc43c28,0x32185c01),(0xc43cb4,0x32185c40)]:patch(va,struct.pack('<I',opcode),'Normalize SpannedString ForegroundColorSpan RGB to white while retaining span alpha')
for va in [0xf18508,0x10c57cc,0x10c6a60,0x10c7180,0x12908f0,0x163e2a8,0x185ce78]:patch(va,branch(va,0x17458e4,True),'Apply grayscale color-matrix filtering to widget ImageViews, Drawables and MAML canvas paints')
patch(0x185d4a0,branch(0x185d4a0,0x185d4e4),'Retain transparent interior on stroke-only GradientDrawables under grayscale matrix filter')
patch(0x10e271c,branch(0x10e271c,0x17458e4,True)+struct.pack('<I',0xf81c83a0)+branch(0x10e2724,0x10e2898),'Apply grayscale color-matrix filtering to TextView compound drawables')
for va in [0xfc6748,0xfc7638,0xfc84a8]:patch(va,bytes.fromhex('e00316aac0035fd6'),'Omit native Frame/Linear/Relative container background painting, retain provider geometry and foreground controls')
patch(0x19bfda8,branch(0x19bfda8,0x19bff18),'Preserve captured rounded edges; native SF supplies the animated clip')
for va in [0x148083c,0x1480844]:patch(va,bytes.fromhex('1f2003d5'),'Enter the existing native glass host for Android widgets')
patch(0x1480914,bytes.fromhex('1f2003d5'),'Use the folder glass presets at static fields 0x2090/0x2088 for Android hosts')
for va,target in [(0x1480a00,0x1480a20),(0x1480b78,0x1480b98)]:patch(va,branch(va,target),'Clip animated and resting Android glass hosts using their computed widget corner radius')
previous_end=0
for p in sorted(patches,key=lambda p:int(p['vaddr'],16)):
 va=int(p['vaddr'],16);data=bytes.fromhex(p['replacement']);assert len(data)<=256 and va>=previous_end,(p['vaddr'],previous_end);previous_end=va+len(data)
assert len(patches)<=128,len(patches)
profile['scope']='Native glass icons, common MAML and Android widget rendering, full-view SF snapshots; Launcher 7722'
(H/'profile.json').write_text(json.dumps(profile,indent=2)+'\n')
(H/'patches.txt').write_text(''.join(f"{p['vaddr']} {p['expected']} {p['replacement']}\n"for p in patches))
print('PASS:',len(patches),'nonoverlapping bounded sites')
