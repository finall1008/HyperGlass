import os
"""Build incompatible or ambiguous ELF fixtures to verify feature isolation."""
from pathlib import Path
import lzma,struct,sys
from inspect_pair import Image
from model import branch_target
image=Image(sys.argv[1]);raw=bytearray(Path(sys.argv[1]).read_bytes());mode=sys.argv[2]
def offset(va):return next(s['p_offset']+va-s['p_vaddr']for s in image.elf.iter_segments()if s['p_type']=='PT_LOAD'and s['p_vaddr']<=va<s['p_vaddr']+s['p_filesz'])
if mode=='core-layout':
 at=offset(0xa03338);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(511<<12))|(0x4b<<12))
elif mode=='badge-layout':
 at=offset(0x1749cfc);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(511<<12))|(0x2f<<12))
elif mode=='widget-layout':
 pc=0xc1e528;target=branch_target(struct.unpack('<I',image.read(pc,4))[0],pc);at=offset(target);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,w^(0x10<<5))
elif mode=='engine-layout':
 at=offset(0xc2fc28);w=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,(w&~(4095<<10))|(4<<10))
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
