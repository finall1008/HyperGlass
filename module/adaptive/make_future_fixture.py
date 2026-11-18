import os
"""Generate an unversioned layout-shift fixture, never installed on a device."""
from pathlib import Path
import lzma,struct,sys
from catalog import catalogs,branch_target
from model import immediate
source=Path(sys.argv[1]);out=Path(sys.argv[2]);shift=0x40000
raw=bytearray(source.read_bytes())
def sections(data):
 shoff=struct.unpack_from('<Q',data,40)[0];count=struct.unpack_from('<H',data,60)[0]
 return [(shoff+i*64,struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*64))for i in range(count)]
def shift_elf(data):
 entry=struct.unpack_from('<Q',data,24)[0]
 if entry:struct.pack_into('<Q',data,24,entry+shift)
 phoff=struct.unpack_from('<Q',data,32)[0];phnum=struct.unpack_from('<H',data,56)[0]
 for i in range(phnum):
  at=phoff+i*56
  for field in [16,24]:
   value=struct.unpack_from('<Q',data,at+field)[0]
   if value or struct.unpack_from('<I',data,at)[0]==1:struct.pack_into('<Q',data,at+field,value+shift)
 for at,s in sections(data):
  if s[3]:struct.pack_into('<Q',data,at+16,s[3]+shift)
  if s[1] in [2,11]:
   for p in range(s[4],s[4]+s[5],s[9]):
    index=struct.unpack_from('<H',data,p+6)[0];value=struct.unpack_from('<Q',data,p+8)[0]
    if value and index not in [0,0xfff1,0xfff2]:struct.pack_into('<Q',data,p+8,value+shift)
 return data
# Change allocation tags and two independent compiler-selected layout values.
catalog=catalogs()[0];image=catalog.model.image
def file_offset(va):
 return next(s['p_offset']+va-s['p_vaddr']for s in image.elf.iter_segments()if s['p_type']=='PT_LOAD'and s['p_vaddr']<=va<s['p_vaddr']+s['p_filesz'])

def put_words(va,values):
 struct.pack_into(f'<{len(values)}I',raw,file_offset(va),*values)

# Enlarge a Dart frame while retaining the adapter's saved slots.
frame=image.names['ImageFilterGroup.processAll'][0][0]+8
word=struct.unpack('<I',image.read(frame,4))[0]
put_words(frame,[(word&~(4095<<10))|((immediate(word)+8)<<10)])

# Reassign the hint ARGB and temporaries together, as a compiler can do.
store=0x10e3798;registers={0:3,3:1,5:4,6:5}
operands=((0,5),(0,),(0,5),(0,),(0,5),(0,5),(0,),(0,5),(0,))
changed=[]
for word,shifts in zip(struct.unpack('<9I',image.read(store-12,36)),operands):
 for bit in shifts:
  old=(word>>bit)&31;word=(word&~(31<<bit))|(registers.get(old,old)<<bit)
 changed.append(word)
put_words(store-12,changed)

# Use an existing channel-unpack sequence immediately after the span getter.
put_words(0xc43c28,struct.unpack('<6I',image.read(0xc43cb8,24)))
for rule in catalog.config:
 pc=catalog.model.anchors[rule.anchor].va
 if rule.kind==1:
  target=branch_target(struct.unpack('<I',image.read(pc,4))[0],pc);words=list(struct.unpack('<3I',image.read(target,12)))
  tag=immediate(words[0])|(((words[1]>>5)&65535)<<(((words[1]>>21)&3)*16))
  tag+=(0x120 if rule.slot<5 or rule.slot==15 else 0x200 if rule.slot==5 else 0x300)<<12
  for i in range(2):words[i]=(words[i]&~(65535<<5))|(((tag>>(((words[i]>>21)&3)*16))&65535)<<5)
  put_words(target,words)
 elif rule.slot in [6,9]:
  value=0x153 if rule.slot==6 else -0x130
  word=struct.unpack('<I',image.read(pc,4))[0];encoded=(~value if word&0x60000000==0 else value)&65535
  word=(word&~(65535<<5))|(encoded<<5)
  put_words(pc,[word])
outer=sections(raw);shstr=outer[struct.unpack_from('<H',raw,62)[0]][1];names=raw[shstr[4]:shstr[4]+shstr[5]]
for at,s in outer:
 name=bytes(names[s[0]:]).split(b'\0',1)[0]
 if name==b'.gnu_debugdata':
  debug=shift_elf(bytearray(lzma.decompress(raw[s[4]:s[4]+s[5]])));compressed=lzma.compress(debug)
  offset=(len(raw)+7)&-8;raw.extend(b'\0'*(offset-len(raw)));raw.extend(compressed)
  struct.pack_into('<QQ',raw,at+24,offset,len(compressed))
shift_elf(raw);out.write_bytes(raw)
print('Created layout fixture: moved code, changed Dart CIDs, frame size and slots, text registers and span layout',flush=True)
