import os
"""Build incompatible or ambiguous ELF fixtures to verify feature isolation."""
from pathlib import Path
import lzma,struct,sys
from inspect_pair import Image
from model import branch_target,immediate
image=Image(sys.argv[1]);raw=bytearray(Path(sys.argv[1]).read_bytes());mode=sys.argv[2]
def offset(va):return next(s['p_offset']+va-s['p_vaddr']for s in image.elf.iter_segments()if s['p_type']=='PT_LOAD'and s['p_vaddr']<=va<s['p_vaddr']+s['p_filesz'])
if mode=='core-layout':
 at=offset(0xa03338);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(511<<12))|(0x4b<<12))
elif mode=='scene-view-layout':
 # Only the ceil arm loses the view-ID input. Patching one arm would
 # let the Dart and native snapshot stages disagree.
 pc=0x191a8a0
 at=offset(pc);word=struct.unpack_from('<I',raw,at)[0]
 bounds=immediate(struct.unpack('<I',image.read(0x191a610,4))[0])
 assert word&0xffe00fff==0xf84003a6
 struct.pack_into('<I',raw,at,(word&~(511<<12))|((bounds&511)<<12))
elif mode=='maml-repaint-layout':
 # A changed compact-map cursor must stop the SF adapter before it reads
 # the repaint registry under the old Dart ABI.
 pc=image.names['_HashVMBase._usedData'][0][0]+4
 word=struct.unpack('<I',image.read(pc,4))[0]
 assert word==0xb8413020
 struct.pack_into('<I',raw,offset(pc),word+(4<<12))
elif mode=='sf-return-layout':
 # Losing the call edge makes the existing-layer exit ambiguous.
 pc=0x1b14308
 word=struct.unpack('<I',image.read(pc,4))[0]
 reparent='RustLibApiImpl.crateApiRecentsSfAnimAgentSfAnimAgentReparentIconToHome'
 assert branch_target(word,pc)==image.names[reparent][0][0]
 struct.pack_into('<I',raw,offset(pc),0xd503201f)
elif mode=='detached-capture-identity':
 pc=image.names['_getHash'][0][0]+0x48
 assert struct.unpack('<I',image.read(pc,4))[0]==0xc85f7c02
 struct.pack_into('<I',raw,offset(pc),0xd503201f)
elif mode=='workspace-scroll-layout':
 pc=image.names['MamlVisibilitySignals.setScrolling'][0][0]+20
 assert struct.unpack('<I',image.read(pc,4))[0]==0xf81e83a2
 struct.pack_into('<I',raw,offset(pc),0xd503201f)
elif mode=='material-scene-layout':
 start,size,_=image.names['SceneAnimationSignalType._enumToString'][0]
 pc,=[pc for pc in range(start,start+size,4) if struct.unpack('<I',image.read(pc,4))[0]==0xb840f022]
 struct.pack_into('<I',raw,offset(pc),0xb8413022)
elif mode=='badge-layout':
 at=offset(0x1749cfc);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(511<<12))|(0x2f<<12))
elif mode=='widget-layout':
 pc=0xc1e528;target=branch_target(struct.unpack('<I',image.read(pc,4))[0],pc);at=offset(target);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,w^(0x10<<5))
elif mode=='icon-layout':
 frame=image.names['ImageFilterGroup.processAll'][0][0]+8
 at=offset(frame);word=struct.unpack_from('<I',raw,at)[0]
 struct.pack_into('<I',raw,at,(word&~(4095<<10))|(0x168<<10))
elif mode=='engine-layout':
 at=offset(0xc2fc28);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(4095<<10))|(4<<10))
elif mode=='frame-info-layout':
 at=offset(0xd5dca0);word=struct.unpack_from('<I',raw,at)[0]
 assert immediate(word)==192
 struct.pack_into('<I',raw,at,word+(16<<5))
elif mode=='glass-cache-layout':
 at=offset(0xd557e0);word=struct.unpack_from('<I',raw,at)[0]
 assert word&0xffc003ff==0xf9400049 and immediate(word)==8
 struct.pack_into('<I',raw,at,(word&~(4095<<10))|(2<<10))
elif mode=='ambiguous':
 old=0xa030a4;new=0xb32838;raw[offset(new):offset(new)+36]=image.read(old,36)
 debug=image.elf.get_section_by_name('.gnu_debugdata');inner=bytearray(lzma.decompress(debug.data()))
 shoff=struct.unpack_from('<Q',inner,40)[0];count=struct.unpack_from('<H',inner,60)[0]
 for i in range(count):
  at=shoff+i*64;s=struct.unpack_from('<IIQQQQIIQQ',inner,at)
  if s[1]!=2:continue
  table=bytearray(inner[s[4]:s[4]+s[5]])
  entry=next(bytearray(table[n:n+24])for n in range(0,len(table),24)if struct.unpack_from('<Q',table,n+8)[0]==old)
  struct.pack_into('<Q',entry,8,new);table.extend(entry);new_offset=(len(inner)+7)&-8;inner.extend(b'\0'*(new_offset-len(inner)));inner.extend(table);struct.pack_into('<QQ',inner,at+24,new_offset,len(table));break
 compressed=lzma.compress(inner);new_offset=(len(raw)+7)&-8;raw.extend(b'\0'*(new_offset-len(raw)));raw.extend(compressed)
 outer_shoff=image.elf.header['e_shoff'];index=image.elf.get_section_index('.gnu_debugdata');struct.pack_into('<QQ',raw,outer_shoff+index*64+24,new_offset,len(compressed))
else:raise ValueError(mode)
Path(sys.argv[3]).write_bytes(raw);print('Created',mode,'feature isolation fixture',flush=True)
