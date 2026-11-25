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
reference=Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'))/'launcher-801030308/libapp.so'
sys.path.insert(0,str(ROOT/'module/adaptive'))
from model import Image,branch_target
reference_symbols=Image(reference).names
hash_stub=reference_symbols['_getHash'][0][0]
scroll_target=reference_symbols['MamlVisibilitySignals.setScrolling'][0][0]
subprocess.run([str(CC),*flags,f'-Wl,--defsym,dart_layer_hash={hash_stub}',f'-Wl,--defsym,material_scroll_target={scroll_target}', '-Oz','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-fno-pic','-fno-pie','-ffunction-sections','-nostdlib','-static',f'-Wl,-T,{H/"layout.ld"},-e,capture_bridge,--build-id=none',str(H/'bridge.S'),str(H/'hooks.S'),str(H/'capture.c'),'-o',str(H/'capture.elf')],check=True)
source=ELFFile(open(reference,'rb'))
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
def taken(va):
 # Replace a local conditional branch with an unconditional branch to the
 # same destination in the reference image.
 word=struct.unpack('<I',read(va,4))[0];target=branch_target(word,va)
 if target is None or word&0x7c000000==0x14000000:raise ValueError(('not a conditional branch',hex(va),hex(word)))
 return branch(va,target)
patch(0xeb9300,taken(0xeb9300),'Validate complete shortcut geometry without requiring the legacy bitmap eligibility flag')
patch(0xeb93a4,struct.pack('<I',0x54000000|(((0xeb940c-0xeb93a4)//4)<<5)),'Capture the attached boundary of one-cell shortcuts, including live layered artwork')
boundary=ELFFile(open(H/'boundary.elf','rb'))
s=boundary.get_section_by_name('.message');patch(s['sh_addr'],s.data(),'Complete keyed icon boundary; preserve allocation PCs and notification wrapping')
patch(0x15f1510,branch(0x15f1510,0x2006360),'Set the icon capture key through the existing boundary allocator')
for va in [0x1b482c0,0x1c89c3c]:patch(va,bytes.fromhex('1f2003d5'),'Use the complete icon boundary for native SF animations')
linked=ELFFile(open(H/'capture.elf','rb'))
# Private adapter sections are linked above the reference image and mapped
# into a private range at runtime. The limits bound each section's link slot.
limits={'.capture_root':0xac8,'.widget_helpers':0x300,'.capture_meta':0x25c,'.hooks':0xe0,'.capture_small':0x1d8,'.limit_test':0xc0,'.bridge':0x140,'.text_color':0xe4}
limits['.badge']=64
if flags:limits.update({'.capture_config':156,'.widget_spans':12,'.widget_wrap':96,'.widget_image_filter':20,'.maml_repaint':64,'.material_scene':64,'.material_scroll':64,'.detached_capture':4096,'.icon_retake':64,'.icon_register':64})
for sec in linked.iter_sections():
 if sec['sh_type']in ['SHT_REL','SHT_RELA']:assert sec['sh_size']==0
 if sec['sh_flags']&2 and sec['sh_size']:assert sec.name in limits,sec.name
for name,limit in limits.items():
 sec=linked.get_section_by_name(name);code=sec.data();assert len(code)<=limit,(name,len(code),limit)
 assert sec['sh_addr']>=0x2000000,(name,hex(sec['sh_addr']))
 print(name,len(code),'bytes')
symbols={s.name:s['st_value']for s in linked.get_section_by_name('.symtab').iter_symbols()}
for va,name in [(0x1592d58,'badge_text'),(0x1592e94,'badge_background')]:patch(va,branch(va,symbols[name]),'Optional white notification badges with black text')
sites=[(0x16a3c2c,'capture_transform'),(0x9e2740,'root_gateway'),(0x9e2754,'refresh_path'),(0x16a387c,'force_path'),(0x16a3930,'stop_after_path'),(0x16a3620,'capture_image_filter'),(0x16a4c6c,'capture_color_filter'),(0x16a4e74,'capture_shader_mask'),(0x16a3ec0,'capture_opacity'),(0x1a7f0d4,'mark_floor'),(0x1a7f244,'mark_ceil'),(0x1b493ac,'result_gateway'),(0x1c8a790,'bytes_gateway'),(0xa1055c,'clone_pack'),(0xa10588,'clone_unpack')]
for va,name in sites:patch(va,branch(va,symbols[name]),'Keep wallpaper sampling in the full viewport; crop pixels before native SF drawing')
patch(0x1c89bc8,branch(0x1c89bc8,symbols['source_controller_tag']),'Resolve recommendation icon controllers using the exact flight source identity')
for va,name in [(0x105ca20,'android_text_inline'),(0xd0ebb0,'android_spanned_text'),(0x11b854c,'android_hint_text'),(0x180b6e8,'maml_text_color'),(0x18072a8,'maml_filter_color'),(0x18885a0,'android_background'),(0x11971e4,'android_image_plate'),(0x1196cfc,'android_image_opacity')]:patch(va,branch(va,symbols[name]),'Common Android widget content: monochrome text and structural background painting with provider outlines intact')
patch(0xd0ec58,struct.pack('<I',0x92800021)+branch(0xd0ec5c,0xd0eba0),'Default uncolored SpannedString roots to monochrome white')
for va,opcode in [(0xd0fa54,0x32005c01),(0xd0fae0,0x32005c40)]:patch(va,struct.pack('<I',opcode),'Normalize SpannedString ForegroundColorSpan RGB to white while retaining span alpha')
if flags:patch(0xd0fae4,branch(0xd0fae4,symbols['span_direct']),'Normalize a directly unpacked ForegroundColorSpan')
for va in [0xc5c000,0x119800c,0x1199580,0x1199b64,0x137a9fc,0x1807348,0x19b08dc]:
 reason='Apply grayscale color-matrix filtering to Android widget vectors' if va==0x137a9fc else 'Apply grayscale color-matrix filtering to widget ImageViews, Drawables and MAML canvas paints'
 patch(va,branch(va,0x18a95d8,True),reason)
patch(0x19b0f04,branch(0x19b0f04,0x19b0f48),'Retain transparent interior on stroke-only GradientDrawables under grayscale matrix filter')
patch(0x11b72c0,branch(0x11b72c0,0x18a95d8,True)+struct.pack('<I',0xf81c83a0)+branch(0x11b72c8,0x11b743c),'Apply grayscale color-matrix filtering to TextView compound drawables')
for va in [0x10b5a98,0x10b64d4,0x10b7588]:patch(va,bytes.fromhex('e00316aa'),'Omit the Frame/Linear/Relative container paint call, retain provider geometry and foreground controls')
patch(0x1b49114,branch(0x1b49114,0x1b49284),'Preserve captured rounded edges; native SF supplies the animated clip')
for va in [0x1588534,0x158853c]:patch(va,bytes.fromhex('1f2003d5'),'Enter the existing native glass host for Android widgets')
patch(0x1588638,bytes.fromhex('1f2003d5'),'Use the folder glass presets at static fields 0x2090/0x2088 for Android hosts')
for va in [0x1588730,0x15888a8]:patch(va,taken(va),'Clip animated and resting Android glass hosts using their computed widget corner radius')
previous_end=0
for p in sorted(patches,key=lambda p:int(p['vaddr'],16)):
 va=int(p['vaddr'],16);data=bytes.fromhex(p['replacement']);assert len(data)<=256 and va>=previous_end,(p['vaddr'],previous_end);previous_end=va+len(data)
assert len(patches)<=128,len(patches)
profile['scope']='Native glass icons, common MAML and Android widget rendering, full-view SF snapshots; Launcher 801030308'
(H/'profile.json').write_text(json.dumps(profile,indent=2)+'\n')
(H/'patches.txt').write_text(''.join(f"{p['vaddr']} {p['expected']} {p['replacement']}\n"for p in patches))
print('PASS:',len(patches),'nonoverlapping bounded sites')
