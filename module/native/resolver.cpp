#include "resolver.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
namespace hg {
static unsigned distance(const uint32_t *values,const uint32_t *masks,size_t count,const std::vector<uint32_t>&target){
 std::array<unsigned,48>previous{},current{};
 if(count>=48||target.size()>=48)return 99;
 for(size_t i=0;i<count;i++){
  current[0]=unsigned(i+1);
  for(size_t j=1;j<=target.size();j++)current[j]=std::min({previous[j]+1,current[j-1]+1,previous[j-1]+((values[i]&masks[i])==(target[j-1]&masks[i])?0u:3u)});
  previous=current;
 }
 return previous[target.size()];
}
void Locator::scan(const Anchor&a,uint64_t begin,uint64_t end,std::vector<uint32_t>&exact,std::vector<uint32_t>&elastic){
 if(end<begin||end-begin>0x4000000||!image_.at(begin,size_t(end-begin)))return;
 for(uint64_t pc=begin;pc+4<=end;pc+=4){
  if((image_.word(pc)&a.masks[a.before])!=(a.values[a.before]&a.masks[a.before]))continue;
  size_t left=std::min<size_t>((pc-begin)/4,a.before+2),right=std::min<size_t>((end-pc-4)/4,a.count-a.before-1+2);
  std::vector<uint32_t>before,after;
  for(size_t n=left;n;n--)before.push_back(image_.word(pc-n*4));
  for(size_t n=right;n;n--)after.push_back(image_.word(pc+n*4));
  std::array<uint32_t,32>rv{},rm{};size_t nr=a.count-a.before-1;
  for(size_t n=0;n<nr;n++){rv[n]=a.values[a.count-n-1];rm[n]=a.masks[a.count-n-1];}
  unsigned score=distance(a.values,a.masks,a.before,before)+distance(rv.data(),rm.data(),nr,after);
  if(!score)exact.push_back(uint32_t(pc));else if(score<=2)elastic.push_back(uint32_t(pc));
 }
}
uint32_t Locator::resolve(size_t index){
 if(index>=program_.anchor_count)return 0;
 if(resolved_[index]>=0)return uint32_t(resolved_[index]);
 resolved_[index]=0;const auto&a=program_.anchors[index];std::vector<uint32_t>exact,elastic;
 if(a.mode==5){
  uint32_t marker=resolve(size_t(a.dependency));if(!marker)return 0;std::set<uint64_t>destinations;
  for(const auto&s:image_.named(a.callee))destinations.insert(s.value);
  auto owners=image_.named(a.owner);
  for(const auto&owner:owners){
   uint64_t end=image_.symbol_end(owner);if(marker<owner.value||marker>=end)continue;
   for(uint64_t pc=marker;pc+4<=end;pc+=4){uint32_t word=image_.word(pc);
    if((word&0xfc000000)==0x94000000&&destinations.count(branch_target(word,pc))){
     uint64_t result=pc+a.delta;if((image_.word(result)&a.masks[a.before])==(a.values[a.before]&a.masks[a.before]))exact.push_back(uint32_t(result));break;
    }
   }
  }
  if(owners.empty()||destinations.empty())for(uint64_t pc=marker,end=marker+0x200;pc+4<=end&&image_.executable(pc,4);pc+=4){
   uint32_t word=image_.word(pc);if((word&0xfc000000)!=0x94000000||!image_.executable(branch_target(word,pc),4))continue;
   uint64_t result=pc+a.delta;if((image_.word(result)&a.masks[a.before])==(a.values[a.before]&a.masks[a.before]))scan(a,result>=uint64_t(a.before)*4?result-uint64_t(a.before)*4:0,result+uint64_t(a.count-a.before)*4,exact,elastic);
  }
 }else if(a.mode==4){uint64_t pc=image_.import_stub(a.callee);if(pc)exact.push_back(uint32_t(pc));
 }else if(a.mode==1){
  uint32_t caller=resolve(size_t(a.dependency));if(!caller)return 0;
  uint64_t target=branch_target(image_.word(caller),caller);if(!target||!image_.executable(target,12))return 0;
  bool valid=true;for(size_t n=0;n<std::min<size_t>(3,a.count-a.before);n++){
   uint32_t old=a.values[a.before+n],mask=instruction_mask(old,false);if((image_.word(target+n*4)&mask)!=(old&mask))valid=false;
  }
  if(valid)exact.push_back(uint32_t(target));
 }else if(a.mode==2||a.mode==3){
  std::set<uint64_t>destinations;for(const auto&s:image_.named(a.callee))destinations.insert(s.value);
  auto owners=image_.named(a.owner);
  auto check_range=[&](uint64_t begin,uint64_t end,bool require_neighborhood){
   for(uint64_t pc=begin;pc+4<=end;pc+=4){
    uint32_t w=image_.word(pc);if((w&0xfc000000)!=0x94000000)continue;uint64_t target=branch_target(w,pc);
    if(a.mode==2){
     if((!destinations.empty()&&!destinations.count(target))||pc<4||!image_.at(pc-4,12)||(image_.word(pc-4)&0xfff8001f)!=0x37200000||(image_.word(pc+4)&0xfff8001f)!=0x37200000)continue;
     uint64_t result=pc+a.delta;if((image_.word(result)&a.masks[a.before])!=(a.values[a.before]&a.masks[a.before]))continue;
     if(require_neighborhood)scan(a,result>=uint64_t(a.before)*4?result-uint64_t(a.before)*4:0,result+uint64_t(a.count-a.before)*4,exact,elastic);
     else exact.push_back(uint32_t(result));
    }else{
     if(!image_.at(target,12))continue;uint32_t first=image_.word(target),second=image_.word(target+4),third=image_.word(target+8);
     if((first&0xff80001f)!=0xd2800002||(second&0xff80001f)!=0xf2800002||(third&0xfc000000)!=0x14000000)continue;
     uint64_t tag=uint64_t(instruction_immediate(first))|(uint64_t((second>>5)&65535)<<(((second>>21)&3)*16));
     if((tag&255)==0x1c&&int(((tag>>8)&15)*16)>=a.delta){
      if(require_neighborhood)scan(a,pc>=uint64_t(a.before)*4?pc-uint64_t(a.before)*4:0,pc+uint64_t(a.count-a.before)*4,exact,elastic);
      else exact.push_back(uint32_t(pc));
     }
    }
   }
  };
  for(const auto&symbol:owners)check_range(symbol.value,image_.symbol_end(symbol),false);
  if(owners.empty()||(a.mode==2&&destinations.empty()))for(const auto&segment:image_.segments())if(segment.p_flags&PF_X)check_range(segment.p_vaddr,segment.p_vaddr+segment.p_filesz,true);
 }else{
  uint64_t qualified=0;if(a.qualifier>=0){uint32_t pc=resolve(size_t(a.qualifier));if(!pc)return 0;qualified=branch_target(image_.word(pc),pc);}
  auto functions=image_.named(a.owner);std::set<uint64_t>seen;for(const auto&s:functions)seen.insert(s.value);
  std::string owner=a.owner;size_t dot=owner.find_last_of('.');std::string prefix=dot==std::string::npos?owner+".":owner.substr(0,dot+1);
  for(unsigned depth=0;depth<2;depth++){
   auto previous=functions;size_t added=0;
   for(const auto&entry:previous)for(uint64_t pc=entry.value,end=image_.symbol_end(entry);pc+4<=end;pc+=4){
    uint32_t w=image_.word(pc);if((w&0xfc000000)!=0x94000000)continue;uint64_t target=branch_target(w,pc);
    if(seen.count(target))continue;
    const auto&all=image_.symbols();auto candidate=std::lower_bound(all.begin(),all.end(),target,[](const Symbol&s,uint64_t address){return s.value<address;});
    for(;candidate!=all.end()&&candidate->value==target;++candidate)if(candidate->name.compare(0,prefix.size(),prefix)==0){functions.push_back(*candidate);seen.insert(target);added++;break;}
   }
   if(!added||functions.size()>32)break;
  }
  for(const auto&s:functions)if(!qualified||s.value==qualified)scan(a,s.value,image_.symbol_end(s),exact,elastic);
  if(functions.empty())for(const auto&segment:image_.segments())if(segment.p_flags&PF_X)scan(a,segment.p_vaddr,segment.p_vaddr+segment.p_filesz,exact,elastic);
 }
 auto &choices=exact.empty()?elastic:exact;std::sort(choices.begin(),choices.end());choices.erase(std::unique(choices.begin(),choices.end()),choices.end());candidates_[index]=unsigned(choices.size());
 if(choices.size()==1)resolved_[index]=choices.front();return uint32_t(resolved_[index]);
}
uintptr_t adapter_address(const Resolved&result,uint32_t source){
 uintptr_t page=size_t(sysconf(_SC_PAGESIZE));uint32_t key=source&~uint32_t(page-1);
 auto it=std::lower_bound(result.adapter_pages.begin(),result.adapter_pages.end(),key,[](const auto&item,uint32_t at){return item.first<at;});
 return it!=result.adapter_pages.end()&&it->first==key?it->second+(source&(page-1)):0;
}
static uintptr_t reserve_near(uintptr_t base,size_t span){
 size_t page=size_t(sysconf(_SC_PAGESIZE));span=(span+page-1)&~(page-1);
 uintptr_t low=base>0x6000000?base-0x6000000:page,high=base+0x6000000;
 FILE*f=fopen("/proc/self/maps","re");if(!f)return 0;
 std::vector<uintptr_t>choices;uintptr_t cursor=low;char line[1536];unsigned long begin,end;
 while(fgets(line,sizeof(line),f)){
  if(sscanf(line,"%lx-%lx",&begin,&end)!=2||end<=low)continue;
  if(begin>high)begin=high;
  if(begin>cursor&&begin-cursor>=span)choices.push_back(cursor);
  cursor=std::max<uintptr_t>(cursor,end);if(cursor>=high)break;
 }
 fclose(f);if(cursor<high&&high-cursor>=span)choices.push_back(cursor);
 std::sort(choices.begin(),choices.end(),[&](uintptr_t a,uintptr_t b){return(a>base?a-base:base-a)<(b>base?b-base:base-b);});
 for(uintptr_t at:choices){
  // Linux MAP_FIXED_NOREPLACE reserves only an observed free gap.
  void*p=mmap((void*)at,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|0x100000,-1,0);
  if(p==MAP_FAILED)continue;if((uintptr_t)p==at)return at;munmap(p,span);
 }
 return 0;
}
static bool encode_relative(uint32_t old,uint64_t pc,uint64_t destination,uint8_t kind,uint32_t &out){
 int64_t delta=int64_t(destination)-int64_t(pc);out=old;
 if(kind==1||kind==2||kind==3){
  unsigned bits=kind==1?26:kind==2?19:14,shift=kind==1?0:5;
  if(delta%4||delta<-(int64_t(1)<<(bits+1))||delta>=(int64_t(1)<<(bits+1)))return false;
  uint32_t mask=((1u<<bits)-1)<<shift;out=(old&~mask)|((uint32_t(delta/4)&((1u<<bits)-1))<<shift);return true;
 }
 if(kind==4||kind==5){
  if(kind==5)delta=(int64_t(destination&~uint64_t(4095))-int64_t(pc&~uint64_t(4095)))/4096;
  if(delta<-(int64_t(1)<<20)||delta>=(int64_t(1)<<20))return false;
  uint32_t n=uint32_t(delta)&0x1fffff;out=(old&0x9f00001f)|((n&3)<<29)|((n>>2)<<5);return true;
 }
 if(kind==6){out=(old&~(4095u<<10))|(uint32_t(destination&4095)<<10);return true;}
 if(kind==7){unsigned scale=((old&(1u<<26))&&(old&(1u<<23)))?4:old>>30;uint32_t low=uint32_t(destination&4095);if(low&((1u<<scale)-1))return false;out=(old&~(4095u<<10))|((low>>scale)<<10);return true;}
 return false;
}
// Some launchers added a bitmap cache ahead of the attached boundary. Its
// image can contain an old calendar/clock frame and lacks the current glass.
// Discover the cache guard from the current control flow, including its
// cached-image flag, so both hits and misses use the existing live capture.
static bool prefer_live_boundary(const ElfImage&image,uintptr_t live,const char*name,std::vector<Patch>&patches,std::string&diagnostic){
 auto owners=image.named(name),captures=image.named("RenderRepaintBoundary.toImage");
 if(owners.size()!=1||captures.empty())return false;
 const auto&owner=owners.front();uint64_t end=image.symbol_end(owner);std::vector<uint64_t>calls;
 for(uint64_t pc=owner.value;pc+4<=end;pc+=4)if((image.word(pc)&0xfc000000)==0x94000000){
  uint64_t target=branch_target(image.word(pc),pc);for(const auto&s:captures)if(target==s.value)calls.push_back(pc);
 }
 if(calls.empty())return false;std::vector<std::pair<uint64_t,uint64_t>>guards;
 for(uint64_t capture:calls){
 for(uint64_t pc=std::max(owner.value,capture>128?capture-128:0);pc+4<capture;pc+=4){
  uint32_t branch=image.word(pc);if((branch&0xff00001f)!=0x54000001||image.word(pc-4)!=0x6b16001f)continue;
  uint64_t target=branch_target(branch,pc);if(target<=capture||target>=end)continue;
  for(uint64_t at=std::max(owner.value,pc-32);at+8<=pc;at+=4){
   uint32_t select=image.word(at),store=image.word(at+4);
   if((select&0xffe00c00)!=0x9a800000||((select>>12)&15)!=1)continue;
   unsigned result=select&31,yes=(select>>5)&31,no=(select>>16)&31;
   bool stored=(store&0xffe00c00)==0xf8000000&&(store&31)==result&&((store>>5)&31)==29;
   if(!stored&&(store&0xff800000)==0x92800000){
    uint32_t indexed=image.word(at+8);bool decoded=false;int64_t offset=instruction_immediate(store,&decoded);
    stored=decoded&&offset<0&&offset>=-65536&&(indexed&0xffe0fc00)==0xf8206800&&(indexed&31)==result&&((indexed>>5)&31)==29&&((indexed>>16)&31)==(store&31);
   }
   if(!stored)continue;
   bool true_value=false,false_value=false;
   for(uint64_t before=std::max(owner.value,at-16);before<at;before+=4){uint32_t add=image.word(before);
    if((add&0xffc003e0)!=0x910002c0)continue;
    unsigned reg=add&31,immediate=(add>>10)&4095;
    true_value|=reg==yes&&immediate==0x20;false_value|=reg==no&&immediate==0x30;
   }
   if(true_value&&false_value)guards.emplace_back(at,pc);
  }
 }
 }
 if(guards.empty()){
  // An unused helper elsewhere in the ELF does not imply a cache here.
  for(const auto&cache:image.named("BigIconGetUtil.loadFlightImage"))for(uint64_t pc=owner.value;pc+4<=end;pc+=4)
   if((image.word(pc)&0xfc000000)==0x94000000&&branch_target(image.word(pc),pc)==cache.value)return false;
  return true;
 }
 if(guards.size()!=1)return false;
 auto add=[&](uint64_t pc,uint32_t replacement){Patch patch{};patch.address=live+pc;patch.size=4;patch.group=2;memcpy(patch.original,image.at(pc,4),4);memcpy(patch.bytes,&replacement,4);patches.push_back(patch);};
 uint32_t select=image.word(guards.front().first);unsigned no=(select>>16)&31;
 add(guards.front().first,0xaa0003e0|(no<<16)|(select&31));add(guards.front().second,0xd503201f);
 diagnostic+=std::string(name)+": current boundary selected over flight bitmap cache; ";return true;
}
bool resolve_program(const char*path,uintptr_t live,const Program&program,Resolved&out,bool allocate){
 ElfImage image;std::string error;if(!image.load(path,error)){out.diagnostic=error;return false;}
 Locator locator(image,program);out.addresses.resize(program.anchor_count);
 for(size_t i=0;i<program.anchor_count;i++)out.addresses[i]=locator.resolve(i);
 size_t page=size_t(sysconf(_SC_PAGESIZE));std::set<uint32_t>pages;
 for(size_t i=0;i<program.unit_count;i++)if(program.units[i].shadow){const auto&u=program.units[i];for(uint32_t at=u.source&~uint32_t(page-1),end=(u.source+u.size+page-1)&~uint32_t(page-1);at<end;at+=uint32_t(page))pages.insert(at);}
 size_t span=pages.size()*page;out.shadow_size=span;
 out.shadow=allocate&&span?reserve_near(live,span):live+0x3000000;
 if(span&&!out.shadow){out.diagnostic="no nearby private adapter range";return false;}
 size_t next=0;for(uint32_t at:pages)out.adapter_pages.emplace_back(at,out.shadow+(next++)*page);
 uint32_t ready=0;for(size_t i=0;i<program.group_count;i++)ready|=program.groups[i];
 for(size_t i=0;i<program.constraint_count;i++){
  const auto&c=program.constraints[i];uint32_t left=out.addresses[size_t(c.left)];bool valid=false;
  if(c.right<0){bool decoded=false;int64_t value=instruction_immediate(image.word(left),&decoded);valid=left&&decoded&&value==c.delta;}
  else {uint32_t right=out.addresses[size_t(c.right)];valid=left&&right&&int64_t(left)-int64_t(right)==c.delta;}
  if(!valid){ready&=~c.group;out.diagnostic+=c.right<0?"Dart object/frame layout constraint failed; ":"Dart allocation return-PC constraint failed; ";}
 }
 auto target_address=[&](const Target&t)->uint64_t{
  if(t.kind==1)return adapter_address(out,t.source);
  if(t.anchor<0||size_t(t.anchor)>=out.addresses.size()||!out.addresses[size_t(t.anchor)])return 0;
  uint64_t address=out.addresses[size_t(t.anchor)];
  if(t.kind==2)return live+branch_target(image.word(address),address);
  if(t.kind==3)return live+address+t.source;
  return live+address;
 };
 std::vector<std::vector<uint8_t>>payloads;payloads.reserve(program.unit_count);std::vector<uintptr_t>locations;
 for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];uintptr_t address=unit.shadow?adapter_address(out,unit.source):(unit.anchor>=0&&out.addresses[size_t(unit.anchor)]?live+out.addresses[size_t(unit.anchor)]:0);
  locations.push_back(address);payloads.emplace_back(unit.bytes,unit.bytes+unit.size);
  bool ok=address&&(unit.shadow||image.executable(address-live,unit.size));
  for(unsigned j=0;j<unit.relocation_count&&ok;j++){
   const auto&r=unit.relocations[j];uint64_t dest=target_address(r.target);uint32_t word=0;
   if(r.offset+4>unit.size||!dest){ok=false;break;}
   if(r.kind==8)word=image.word(dest-live);
   else ok=encode_relative(r.original,address+r.offset,dest,r.kind,word);
   if(!ok){char detail[192];snprintf(detail,sizeof(detail),"relocation source=%x+%x kind=%u pc=%llx destination=%llx; ",unit.source,r.offset,r.kind,(unsigned long long)(address+r.offset),(unsigned long long)dest);out.diagnostic+=detail;}
   if(ok)memcpy(payloads.back().data()+r.offset,&word,4);
  }
  if(!ok){ready&=~uint32_t(unit.group);out.diagnostic+=std::string(unit.label)+": unresolved address/relocation; ";}
 }
 std::array<uint32_t,32>config{};std::array<bool,32>known{};
 for(size_t i=0;i<program.config_count;i++){
  const auto&rule=program.config[i];uint32_t pc=rule.anchor>=0?out.addresses[size_t(rule.anchor)]:0;uint64_t value=0;bool valid=pc!=0;
  if(valid&&rule.kind==1){
   uint64_t callee=branch_target(image.word(pc),pc);uint32_t first=image.word(callee),second=image.word(callee+4),third=image.word(callee+8);
   valid=(first&0xff80001f)==0xd2800002&&(second&0xff80001f)==0xf2800002&&(third&0xfc000000)==0x14000000;
   uint64_t tag=uint64_t(instruction_immediate(first))|(uint64_t((second>>5)&65535)<<(((second>>21)&3)*16));
   valid=valid&&((tag&255)==(rule.source_tag&255))&&(((tag>>8)&15)>=((rule.source_tag>>8)&15));value=tag>>12;
  }else if(valid){bool decoded=false;value=uint64_t(instruction_immediate(image.word(pc),&decoded));valid=decoded;}
  if(valid&&rule.subtract_slot>=0)value-=config[size_t(rule.subtract_slot)];
  if(valid&&rule.slot<config.size()){config[rule.slot]=uint32_t(value);known[rule.slot]=true;}
 }
 if(program.config_count){
  bool snapshot=known[9];for(unsigned i=0;i<=6;i++)snapshot=snapshot&&known[i];for(unsigned i=10;i<=14;i++)snapshot=snapshot&&known[i]&&config[i]<2048;
  snapshot=snapshot&&config[0]<config[3]&&config[3]+3==config[2]&&config[3]+4==config[4]&&config[4]+2==config[1]&&known[15]&&config[15]>config[1]&&config[15]-config[1]<16;
  bool widget=known[7]&&known[8]&&config[7]&&config[8]<64;
  for(unsigned i=16;i<=30;i++)widget=widget&&known[i]&&config[i]>0&&config[i]<0x10000;
  if(!snapshot){ready&=~2u;out.diagnostic+="snapshot allocation/layout graph unresolved; ";}
  if(!widget){ready&=~4u;out.diagnostic+="Android container layout unresolved; ";}
  for(size_t i=0;i<program.unit_count;i++)if(program.units[i].source==program.config_source&&payloads[i].size()>=program.config_count*4){
   memcpy(payloads[i].data(),config.data(),program.config_count*4);
  }
 }
 if((ready&2)&&!strcmp(program.image,"libapp.so")){
  for(const char*name:{"SfAnimIconCapture._buildLayersFromIconInfo","SfWindowAnimImplementor.prepareIconLayers"})if(!prefer_live_boundary(image,live,name,out.patches,out.diagnostic)){
   ready&=~2u;out.diagnostic+="live icon boundary cache guard unresolved; ";break;
  }
 }
 for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];if(!(ready&unit.group))continue;
  if(unit.shadow){
   if(allocate){uintptr_t begin=locations[i]&~uintptr_t(page-1),end=(locations[i]+unit.size+page-1)&~uintptr_t(page-1);if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_WRITE)){ready&=~uint32_t(unit.group);out.diagnostic+="private adapter mapping failed; ";continue;}memcpy(reinterpret_cast<void*>(locations[i]),payloads[i].data(),unit.size);}
  }else for(size_t offset=0;offset<unit.size;offset+=256){Patch patch{};patch.address=locations[i]+offset;patch.size=uint32_t(std::min<size_t>(256,unit.size-offset));patch.group=unit.group;memcpy(patch.bytes,payloads[i].data()+offset,patch.size);memcpy(patch.original,image.at(locations[i]-live+offset,patch.size),patch.size);out.patches.push_back(patch);}
 }
 if(allocate)for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];if(!unit.shadow||!(ready&unit.group))continue;
  __builtin___clear_cache(reinterpret_cast<char*>(locations[i]),reinterpret_cast<char*>(locations[i]+unit.size));
  uintptr_t begin=locations[i]&~uintptr_t(page-1),end=(locations[i]+unit.size+page-1)&~uintptr_t(page-1);
  if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_EXEC)){ready&=~uint32_t(unit.group);out.diagnostic+="private adapter execute protection failed; ";}
 }
 bool shadow_used=false;for(size_t i=0;i<program.unit_count;i++)if(program.units[i].shadow&&(ready&program.units[i].group))shadow_used=true;
 if(allocate&&out.shadow&&!shadow_used){munmap(reinterpret_cast<void*>(out.shadow),out.shadow_size);out.shadow=0;out.shadow_size=0;out.adapter_pages.clear();}
 out.patches.erase(std::remove_if(out.patches.begin(),out.patches.end(),[&](const Patch&p){return !(ready&p.group);}),out.patches.end());out.ready=ready;
 if(!error.empty())out.diagnostic+=error;
 return ready!=0;
}
}
