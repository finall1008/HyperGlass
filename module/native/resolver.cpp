#include "resolver.hpp"
#include "../../adapters/transitions/capture_layout.h"
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
void Locator::append_helpers(const char*name,std::vector<Symbol>&functions) const{
 std::set<uint64_t>seen;for(const auto&s:functions)seen.insert(s.value);
 std::string owner=name;size_t dot=owner.find_last_of('.');std::string prefix=dot==std::string::npos?owner+".":owner.substr(0,dot+1);
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
}
bool Locator::matches_sequence(const Anchor&a,uint64_t start) const{
 if(!image_.at(start,size_t(a.count)*4)||!image_.executable(start,size_t(a.count)*4))return false;
 for(unsigned i=0;i<a.count;i++)if((image_.word(start+i*4)&a.masks[i])!=(a.values[i]&a.masks[i]))return false;
 for(unsigned i=0;i<a.relation_count;i++){
  const auto&r=a.relations[i];
  if(((image_.word(start+r.left*4)>>r.left_shift)&31)!=((image_.word(start+r.right*4)>>r.right_shift)&31))return false;
 }
 return true;
}
uint32_t Locator::resolve(size_t index){
 if(index>=program_.anchor_count)return 0;
 if(resolved_[index]>=0)return uint32_t(resolved_[index]);
 resolved_[index]=0;const auto&a=program_.anchors[index];std::vector<uint32_t>exact,elastic;
 if(a.mode==Anchor::NamedCall||a.mode==Anchor::OperandSequence){
  std::set<uint64_t>destinations;
  for(const auto&symbol:image_.named(a.callee))destinations.insert(symbol.value);
  if(a.mode==Anchor::NamedCall&&destinations.empty())return 0;
  auto functions=image_.named(a.owner);size_t owner_count=functions.size();
  if(a.follow_helpers)append_helpers(a.owner,functions);
  for(size_t i=0;i<functions.size();i++){
   if(i==owner_count&&!exact.empty())break;
   const auto&owner=functions[i];
   uint64_t end=image_.symbol_end(owner);
   for(uint64_t pc=owner.value;pc+4<=end;pc+=4){
    if(a.mode==Anchor::OperandSequence){
     if(pc+size_t(a.count)*4<=end&&matches_sequence(a,pc))exact.push_back(uint32_t(pc+a.before*4));
     continue;
    }
    uint32_t word=image_.word(pc);
    if((word&0xfc000000)!=0x94000000)continue;
    uint64_t callee=branch_target(word,pc);
    if(!destinations.count(callee))continue;
    uint64_t result=pc+a.delta;
    if(result>=owner.value&&result+4<=end&&image_.executable(result,4))exact.push_back(uint32_t(result));
   }
  }
 }else if(a.mode==Anchor::SceneFloor||a.mode==Anchor::SceneCeil){
  uint64_t mark=scene_marks_[a.mode-Anchor::SceneFloor];
  if(mark)exact.push_back(uint32_t(mark+a.delta));
 }else if(a.mode==Anchor::BranchDestination||a.mode==Anchor::RelativeSequence){
  uint32_t source=resolve(size_t(a.dependency));if(!source)return 0;
  uint64_t pc=uint64_t(source)+a.delta,dest=a.mode==Anchor::BranchDestination?branch_target(image_.word(pc),pc):pc;
  if(dest>=uint64_t(a.before)*4){
   uint64_t start=dest-uint64_t(a.before)*4;
   for(const auto&owner:image_.named(a.owner)){
    uint64_t end=image_.symbol_end(owner);
    if(pc>=owner.value&&pc<end&&start>=owner.value&&start+size_t(a.count)*4<=end&&matches_sequence(a,start))exact.push_back(uint32_t(dest));
   }
  }
 }else if(a.mode==5){
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
  auto functions=image_.named(a.owner);append_helpers(a.owner,functions);
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
static bool add_widget_branch(const ElfImage&image,uintptr_t live,uint64_t pc,uintptr_t target,unsigned group,bool link,std::vector<Patch>&patches){
 uint32_t word=0;
 if(!target||!image.executable(pc,4)||!image.at(pc,4)||!encode_relative(link?0x94000000:0x14000000,live+pc,target,1,word))return false;
 Patch patch{};patch.address=live+pc;patch.size=4;patch.group=uint8_t(group);
 memcpy(patch.original,image.at(pc,4),4);memcpy(patch.bytes,&word,4);patches.push_back(patch);return true;
}
static std::vector<uint64_t> direct_calls(const ElfImage&image,const Symbol&owner,uint64_t callee){
 std::vector<uint64_t> calls;
 for(uint64_t pc=owner.value;pc+4<=owner.value+owner.size;pc+=4){
  uint32_t word=image.word(pc);
  if((word&0xfc000000)==0x94000000&&branch_target(word,pc)==callee)calls.push_back(pc);
 }
 return calls;
}
static bool named_widget_call(const ElfImage&image,const Symbol&owner,const char*name,uint32_t preceding=0){
 auto callees=image.named(name);
 if(callees.size()!=1)return false;
 auto calls=direct_calls(image,owner,callees.front().value);
 return calls.size()==1&&(!preceding||(calls.front()>=owner.value+8&&image.word(calls.front()-8)==preceding));
}
static bool dart_frame(const ElfImage&image,const Symbol&owner){
 return owner.size>=16&&image.executable(owner.value,owner.size)&&image.word(owner.value)==0xa9bf79fd&&image.word(owner.value+4)==0xaa0f03fd;
}

struct SceneFrame {
 std::array<uint64_t,2> marks{};
 int32_t layer=0,bounds=0,round=0,view=0;
};
static constexpr uint32_t fp_load=0xf84003a0,fp_store=0xf80003a0,fp_load_double=0xfc4003a0;
static int32_t frame_slot(uint32_t word,uint32_t opcode,int64_t size){
 if((word&0xffe00fe0)!=opcode)return 0;
 int64_t slot=instruction_immediate(word);
 return slot<0&&slot>=-size&&slot%8==0?int32_t(slot):0;
}

// Trace the return value of _resolveViewId through the boundary's tail call.
// Between the producer and the argument move, the compiler reloads other
// arguments from FP. A write to x0 or another call invalidates that proof.
static unsigned boundary_view_argument(const ElfImage&image,const Symbol&boundary,uint64_t producer,uint64_t consumer){
 if(consumer<=producer+4||!dart_frame(image,boundary))return 0;
 uint32_t move=image.word(consumer-4);
 unsigned reg=move&31;
 if((move&0xffffffe0)!=0xaa0003e0||reg<4||reg>7)return 0;
 for(uint64_t pc=producer+4;pc<consumer-4;pc+=4){
  uint32_t word=image.word(pc);
  if((word&0xffe00fe0)==fp_load_double)continue;
  if((word&0xffe00fe0)!=fp_load||(word&31)==0)return 0;
 }
 return reg;
}

static int32_t scene_view_slot(const ElfImage&image,uint64_t start,uint64_t end,int64_t frame,unsigned view_arg,const SceneFrame&scene){
 std::array<unsigned,32> origins{};
 for(unsigned i=0;i<origins.size();i++)origins[i]=i;
 int32_t view=0;
 bool layer_saved=false,bounds_saved=false,round_saved=false;
 for(uint64_t pc=start;pc<end;pc+=4){
  uint32_t word=image.word(pc);
  unsigned dest=word&31;
  if((word&0xfc000000)==0x94000000)break;
  if((word&0xffe0ffe0)==0xaa0003e0){origins[dest]=origins[(word>>16)&31];continue;}
  if(int32_t slot=frame_slot(word,fp_store,frame)){
   unsigned origin=origins[dest];
   if(slot==scene.layer)layer_saved=origin==1;
   if(slot==scene.bounds)bounds_saved=origin==2;
   if(slot==scene.round)round_saved=origin>=3&&origin<=7&&origin!=view_arg;
   if(origin==view_arg){
    if(view&&view!=slot)return 0;
    view=slot;
   }else if(view==slot)view=0;
   continue;
  }
  if(frame_slot(word,0xfc0003a0,frame)||(word==0xd503201f))continue; // stur dReg or nop
  if((word&0xff8003ff)==0x91000360||(word&0xffc003ff)==0xf9400000){origins[0]=31;continue;}
  return 0;
 }
 return layer_saved&&bounds_saved&&round_saved&&view!=scene.layer&&view!=scene.bounds&&view!=scene.round?view:0;
}

// Confirm that each native call receives the view ID from the same Dart
// slot. The caller may marshal additional register arguments before the BL.
static unsigned native_view_argument(const ElfImage&image,uint64_t start,uint64_t call,int64_t frame,int32_t view){
 unsigned argument=0,written=0;
 for(uint64_t pc=call;pc>start;){
  pc-=4;
  uint32_t word=image.word(pc);
  unsigned reg=word&31;
  int32_t slot=frame_slot(word,fp_load,frame);
  if(!slot&&(word&0xffffffe0)!=0x9100c2c0&&(word&0xffe0ffff)!=0xaa0003e1&&word!=0xd503201f)break;
  if(slot==view&&!(written&(1u<<reg))){
   if(argument||reg<4||reg>7)return 0;
   argument=reg;
  }
  if(word!=0xd503201f)written|=1u<<reg;
 }
 return argument;
}

static const char* locate_scene_frame(const ElfImage&image,SceneFrame&scene){
 auto owners=image.named("OffsetLayer.toImage"),builders=image.named("OffsetLayer._createSceneForImage");
 auto boundaries=image.named("RenderRepaintBoundary.toImage"),views=image.named("RenderRepaintBoundary._resolveViewId");
 auto rasters=image.named("_NativeScene.toImage");
 if(owners.size()!=1||builders.size()!=1||boundaries.size()!=1||views.size()!=1||rasters.size()!=1)
  return "scene call graph is missing or ambiguous";
 const auto&owner=owners.front();
 uint32_t reserve=image.word(owner.value+8);
 if(!dart_frame(image,owner)||(reserve&0xff8003ff)!=0xd10001ef)return "scene frame prologue changed";
 int64_t frame=instruction_immediate(reserve);
 auto create=direct_calls(image,owner,builders.front().value),raster=direct_calls(image,owner,rasters.front().value);
 auto produce_view=direct_calls(image,boundaries.front(),views.front().value);
 auto to_image=direct_calls(image,boundaries.front(),owner.value);
 if(create.size()!=1||raster.size()!=2||produce_view.size()!=1||to_image.size()!=1)
  return "scene call edges are missing or ambiguous";
 uint64_t call=create.front(),end=owner.value+owner.size;
 if(call<owner.value+24||call+20>=end)return "scene creation arguments moved out of the frame";
 uint32_t layer=image.word(call-12),bounds=image.word(call-8),ratio=image.word(call-4);
 scene.layer=frame_slot(layer,fp_load,frame);scene.bounds=frame_slot(bounds,fp_load,frame);
 uint32_t save_scene=image.word(call+8),round=image.word(call+12),branch=image.word(call+16);
 scene.round=frame_slot(round,fp_load,frame);
 if(!scene.layer||!scene.bounds||!scene.round||scene.layer==scene.bounds||
    scene.round==scene.layer||scene.round==scene.bounds||(layer&31)!=1||(bounds&31)!=2||
    !frame_slot(ratio,fp_load_double,frame)||(ratio&31)!=0||
    (image.word(call+4)&0xffffffe0)!=0xaa0003e0||
    (image.word(call+4)&31)!=(save_scene&31)||frame_slot(save_scene,fp_store,frame)!=scene.layer||
    (branch&0xfff80000)!=0x37200000||(branch&31)!=(round&31))
  return "scene layer, bounds or rounding slot is unproven";
 uint64_t ceil=branch_target(branch,call+16);
 if(raster[0]<=call+20||raster[0]+20>=ceil||ceil>=raster[1]||raster[1]+24>end)
  return "floor and ceil call paths are unproven";
 unsigned view_arg=boundary_view_argument(image,boundaries.front(),produce_view.front(),to_image.front());
 if(!view_arg)return "boundary view ID argument is unproven";
 scene.view=scene_view_slot(image,owner.value+12,call,frame,view_arg,scene);
 if(!scene.view)return "view ID spill is missing or ambiguous";

 uint64_t await=0;
 unsigned native_arg=0;
 for(unsigned i=0;i<2;i++){
  uint64_t pc=raster[i],begin=i?ceil:call+20;
  unsigned rounds=0;
  for(uint64_t at=begin;at<pc+16;at+=4){
   uint32_t word=image.word(at);
   rounds+=(word&0xfffffc00)==(i?0x9e680000u:0x9e700000u); // FCVTPS / FCVTMS
   int32_t slot=frame_slot(word,fp_store,frame);
   if(slot==scene.bounds||slot==scene.view)return "scene bounds or view ID overwritten before writeback";
  }
  unsigned arg=native_view_argument(image,begin,pc,frame,scene.view);
  if(rounds!=2||!arg||(i&&arg!=native_arg))return "native scene view ID or rounding path changed";
  native_arg=arg;
  uint32_t wait=image.word(pc+12),store=image.word(pc+16),resume=image.word(pc+20);
  uint64_t destination=branch_target(wait,pc+12);
  int32_t slot=frame_slot(store,fp_store,frame);
  if(image.word(pc+4)!=0xaa0003e1||(image.word(pc+8)&31)!=1||!frame_slot(image.word(pc+8),fp_store,frame)||
     (wait&0xfc000000)!=0x94000000||!image.executable(destination,4)||(i&&destination!=await)||
     !slot||(store&31)!=0||slot==scene.layer||slot==scene.view||
     frame_slot(resume,fp_load,frame)!=scene.layer||(resume&31)!=2)
   return "scene image writeback is unproven";
  await=destination;
  scene.marks[i]=pc+16;
 }
 return nullptr;
}
// Replace the renderer's own ColorFilter construction. Wrapping its final
// widget would apply the matrix twice when an ImageView or TextView already
// owns the filter, and would also change the cached raster path.
static bool plan_vector_filters(const ElfImage&image,uintptr_t live,uintptr_t filter,std::vector<Patch>&out){
 auto matrices=image.named("FlutterDisableImageView.disabledColorFilter");
 if(matrices.size()!=1||!filter)return false;
 uint64_t allocate=0;
 const auto&factory=matrices.front();
 for(uint64_t pc=factory.value;pc+12<=factory.value+factory.size;pc+=4){
  uint32_t word=image.word(pc);
  if((word&0xfc000000)!=0x94000000||(image.word(pc+4)&0xffc003ff)!=0x91400361||
     (image.word(pc+8)&0xffc003ff)!=0xf9400021)continue;
  if(allocate)return false;
  allocate=branch_target(word,pc);
 }
 if(!allocate)return false;
 std::vector<Patch>patches;
 for(const char*name:{"_VectorDrawableBoxState._buildLiveCached","_VectorDrawableBoxState._renderScene"}){
  auto owners=image.named(name);
  if(owners.size()!=1||!dart_frame(image,owners.front())||!named_widget_call(image,owners.front(),"_VectorDrawableBoxState._resolveTintColor"))return false;
  const auto&owner=owners.front();uint64_t end=owner.value+owner.size;
  uint64_t select=0,call=0,join=0;
  for(uint64_t pc=owner.value+4;pc+64<=end;pc+=4){
   uint32_t nonnull=image.word(pc),skip=image.word(pc+8);
   if(image.word(pc-4)!=0x6b16001f||nonnull!=0x54000061||image.word(pc+4)!=0xaa1603e3||
      (skip&0xfc000000)!=0x14000000)continue;
   uint64_t block=branch_target(nonnull,pc),result=branch_target(skip,pc+8);
   if(result<=block+32||result>block+80||result>=end||image.word(result-4)!=0xaa0103e3)continue;
   uint64_t at=result-36;uint32_t word=image.word(at);
   if(at<block||(word&0xfc000000)!=0x94000000||branch_target(word,at)!=allocate||
      image.word(at+4)!=0xaa0003e1||image.word(at+12)!=0xb8007020||
      (image.word(at+20)&0xffffffe0)!=0xb800b020||
      (image.word(at+24)&0xffffffe0)!=0xd2800020||
      (image.word(at+28)&0xffffffe0)!=0xf8013020||
      (image.word(at+24)&31)!=(image.word(at+28)&31))continue;
   if(call)return false;
   select=pc;call=at;join=result;
  }
  if(!call||!add_widget_branch(image,live,select,live+branch_target(image.word(select),select),HG_WIDGET_VECTOR,false,patches)||
     !add_widget_branch(image,live,call,filter,HG_WIDGET_VECTOR,true,patches))return false;
  uint32_t words[]={0xaa0003e3,0}; // mov x3,x0, then the original output join
  if(!encode_relative(0x14000000,live+call+8,live+join,1,words[1]))return false;
  Patch patch{};patch.address=live+call+4;patch.size=sizeof(words);patch.group=HG_WIDGET_VECTOR;
  memcpy(patch.original,image.at(call+4,patch.size),patch.size);
  memcpy(patch.bytes,words,patch.size);patches.push_back(patch);
 }
 out.insert(out.end(),patches.begin(),patches.end());return true;
}
static bool plan_compound_wrapping(const ElfImage&image,uintptr_t live,const WidgetWrap&wrap,Resolved&out){
 std::vector<Patch>patches;
 const char*name="_FlutterTextViewState._applyDrawableTint";
 auto owners=image.named(name),callers=image.named("_FlutterTextViewState._buildCompoundDrawableWidget");
 // Wrap the caller's original Widget in x2 before the provider tint runs.
 if(!(owners.size()==1&&callers.size()==1&&dart_frame(image,owners.front())&&
     named_widget_call(image,callers.front(),name,0xaa0003e2)&&
     named_widget_call(image,owners.front(),"_FlutterTextViewState._tintedCompoundCopy")&&
     add_widget_branch(image,live,owners.front().value,adapter_address(out,wrap.compound),HG_WIDGET_COMPOUND,false,patches)))return false;
 uint64_t call=out.addresses[size_t(wrap.call_anchor)];
 if(!call||!add_widget_branch(image,live,call,adapter_address(out,wrap.dispatch),HG_WIDGET_COMPOUND,true,patches)||
    !add_widget_branch(image,live,call+4,adapter_address(out,wrap.resume),HG_WIDGET_COMPOUND,false,patches))return false;
 out.patches.insert(out.patches.end(),patches.begin(),patches.end());
 return true;
}
static bool plan_background_filter(const ElfImage&image,uintptr_t live,uint64_t call,uintptr_t adapter,std::vector<Patch>&out){
 auto contents=image.named("_ImageViewState._buildContent");
 auto backgrounds=image.named("_ImageViewState._buildBackgroundOnly");
 auto trees=image.named("_ImageViewState._buildImageContentTree");
 if(contents.size()!=1||backgrounds.size()!=1||trees.size()!=1||
    image.named("RenderViewDecorationMixin._paintDrawableAt").empty()||
    !image.named("_ImageViewState._buildBackgroundOnlyFuture").empty())return false;
 const auto&content=contents.front();const auto&tree=trees.front();
 if(!dart_frame(image,content)||!dart_frame(image,tree)||
    call<tree.value+0x24||call+8>tree.value+tree.size||
    image.word(tree.value+0x10)!=0xf81f03a1||image.word(call-4)!=0xf87e7abe||image.word(call)!=0xd63f03c0||
    image.word(content.value+12)!=0x9100c2c0||image.word(content.value+0x28)!=0xb803b060)return false;
 unsigned routes=0;
 for(uint64_t pc=content.value+20;pc+16<=content.value+content.size;pc+=4){
  uint32_t word=image.word(pc);
  if((word&0xfc000000)!=0x94000000||branch_target(word,pc)!=backgrounds.front().value)continue;
  bool first=image.word(pc-20)==0x910082c8&&image.word(pc-16)==0xb803b088&&image.word(pc-12)==0xaa0403e1;
  bool second=image.word(pc-20)==0x910082c2&&image.word(pc-16)==0xb803b002&&image.word(pc-8)==0xaa0003e1;
  if((!first&&!second)||image.word(pc+4)!=0xaa1d03ef||image.word(pc+12)!=0xd65f03c0)return false;
  routes++;
 }
 if(routes!=2||!adapter)return false;
 uint32_t words[]={0xf87e7ab1,0}; // ldr x17,[x21,x30,lsl #3]; bl adapter
 if(!encode_relative(0x94000000,live+call,adapter,1,words[1]))return false;
 Patch patch{};patch.address=live+call-4;patch.size=sizeof(words);patch.group=HG_WIDGET_ARTWORK;
 memcpy(patch.original,image.at(call-4,patch.size),patch.size);
 memcpy(patch.bytes,words,patch.size);out.push_back(patch);
 return true;
}
static bool plan_widget_layout_density(const ElfImage&image,uintptr_t live,std::vector<Patch>&out){
 const char*physical_name="DensitySemantics.physicalDensity";
 auto layout=image.named("DensitySemantics.layoutDensity"),physical=image.named(physical_name);
 auto dimension=image.named("DensitySemantics.dimensionDensity");
 if(layout.size()!=1||dimension.size()!=1||physical.size()!=1||!named_widget_call(image,layout.front(),physical_name)||
    !named_widget_call(image,dimension.front(),physical_name))return false;
 // The earlier Android layout path divided physical dimensions by the
 // provider density divided by the host scale. The newer layout helper
 // accepts provider density directly; its physical fallback keeps the ratio.
 const uint64_t destination=physical.front().value;
 if(image.word(destination)!=0x4ea11c3f||image.word(destination+4)!=0x4ea01c01||
    image.word(destination+8)!=0x4ebf1fe0)return false;
 bool divides=false;
 for(uint64_t pc=destination+12;pc+8<=destination+physical.front().size;pc+=4)
  divides|=image.word(pc)==0x1e611802&&image.word(pc+4)==0x4ea21c40;
 if(!divides)return false;
 std::vector<Patch>patches;
 const std::pair<const char*,uint64_t> callers[]={
  {"FrbTypedArrayAdapter.getLayoutDimension",layout.front().value},
  {"FrbTypedArrayAdapter.getLayoutDimensionPixelSize",layout.front().value},
  {"FrbTypedArrayAdapter._layoutDensity",layout.front().value},
  {"FrbTypedArrayAdapter.getDimensionPixelSize",dimension.front().value},
  {"FrbTypedArrayAdapter.getDimensionPixelOffset",dimension.front().value},
 };
 for(const auto&caller:callers){
  auto owners=image.named(caller.first);
  if(owners.size()!=1)return false;
  const auto&owner=owners.front();size_t before=patches.size();
  for(uint64_t pc=owner.value;pc+4<=owner.value+owner.size;pc+=4){
   uint32_t word=image.word(pc);
   if((word&0xfc000000)==0x94000000&&branch_target(word,pc)==caller.second&&
      !add_widget_branch(image,live,pc,live+destination,4,true,patches))return false;
  }
  if(patches.size()==before)return false;
 }
 out.insert(out.end(),patches.begin(),patches.end());return true;
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

// A return needs the current boundary, even when the existing SF layers have
// the same source identity. Route that case through the stock release path.
static bool recapture_return_boundary(const ElfImage&image,uintptr_t live,std::vector<Patch>&patches){
 auto owners=image.named("SfWindowAnimImplementor.prepareIconLayers");
 auto reparent=image.named("RustLibApiImpl.crateApiRecentsSfAnimAgentSfAnimAgentReparentIconToHome");
 auto release=image.named("SfWindowAnimImplementor.releaseIconLayers");
 auto precreate=image.named("SfWindowAnimImplementor._tryReusePreCreated");
 auto log=image.named("_LoggerImpl.i");
 if(owners.size()!=1||reparent.size()!=1||release.size()!=1||precreate.size()!=1||log.size()!=1)return false;
 const auto&owner=owners.front();uint64_t end=image.symbol_end(owner);
 auto reparent_calls=direct_calls(image,owner,reparent.front().value);
 auto precreate_calls=direct_calls(image,owner,precreate.front().value);
 if(reparent_calls.size()!=1||precreate_calls.size()!=1)return false;
 uint64_t reuse=reparent_calls.front(),pre=precreate_calls.front();
 if(reuse+24>=pre||pre+36>=end)return false;

 auto receiver_load=[&](uint64_t pc){
  return (image.word(pc)&0xff80001f)==0x92800011&&image.word(pc+4)==0xf8716ba1;
 };
 auto is_true_branch=[&](uint64_t pc){return (image.word(pc)&0xfff8001f)==0x37200000;};
 // Reparent is followed by the iterator loop, then the early-return block.
 if((image.word(reuse+12)&0xfc000000)!=0x14000000||
    branch_target(image.word(reuse+12),reuse+12)>=reuse)return false;
 uint64_t release_start=0,guard=0;
 for(uint64_t call:direct_calls(image,owner,release.front().value)){
  if(call<=reuse||call+8>=pre||!receiver_load(call-8))continue;
  for(uint64_t pc=call+4;pc<pre;pc+=4){
   if(!is_true_branch(pc)||!receiver_load(pc+4))continue;
   uint64_t next=branch_target(image.word(pc),pc);
   if(next<=pre||next>=end||pc+20!=pre)continue;
   if(release_start)return false;
   release_start=call-8;guard=pc;
  }
 }
 if(!release_start||(image.word(guard-8)&0xff80001f)!=0x92800011||image.word(guard-4)!=0xf8716ba0)return false;
 uint64_t entry=0;
 for(uint64_t pc=owner.value;pc+12<reuse;pc+=4){
  if(!is_true_branch(pc)||!receiver_load(pc+4)||image.word(pc+4)!=image.word(release_start)||
     (image.word(pc+12)&0xff80001f)!=0x92800011||image.word(pc+16)!=0xf8716ba0)continue;
  uint64_t exit=branch_target(image.word(pc),pc);
  if(exit<=reuse+16||exit+16>=release_start)continue;
  if(entry)return false;
  entry=pc+4;
 }
 if(!entry)return false;
 uint64_t resume=0;
 for(uint64_t pc:direct_calls(image,owner,log.front().value)){
  if(pc<entry+16||pc+8>=reuse||image.word(pc+4)!=image.word(release_start)||
     image.word(pc+8)!=0xf8716ba2)continue;
  if(resume)return false;
  resume=pc+4;
 }
 if(!resume)return false;
 // Use the same opening flag as the press-cache guard. Only a closing
 // call leaves the reuse block. Opening resumes after its reuse log,
 // where the stock code reloads the receiver and runs the iterator.
 uint32_t code[]={image.word(guard-8),image.word(guard-4),0,0};
 if(!encode_relative(image.word(guard),entry+8,release_start,3,code[2])||
    !encode_relative(0x14000000,entry+12,resume,1,code[3]))return false;
 Patch patch{};patch.address=live+entry;patch.size=sizeof(code);patch.group=2;
 memcpy(patch.original,image.at(entry,sizeof(code)),sizeof(code));
 memcpy(patch.bytes,code,sizeof(code));patches.push_back(patch);return true;
}

static bool material_frame_info(const ElfImage&image){
 auto render=image.named("_ZNK8impeller15MiGlassContents6RenderERKNS_14ContentContextERKNS_6EntityERNS_10RenderPassE");
 auto bind=image.named("_ZN8impeller25MiGlassV13F16VertexShader13BindFrameInfoERNS_14ResourceBinderENS_10BufferViewE");
 auto upload=image.named("_ZN8impeller10HostBuffer7EmplaceEPKvmm");
 if(render.size()!=1||bind.size()!=1||upload.size()!=1)return false;
 auto calls=direct_calls(image,render.front(),bind.front().value);
 return calls.size()==2&&std::all_of(calls.begin(),calls.end(),[&](uint64_t pc){
  return pc>=render.front().value+24&&image.word(pc-24)==0x52801803&&
         (image.word(pc-12)&0xfc000000)==0x94000000&&
         branch_target(image.word(pc-12),pc-12)==upload.front().value;
 });
}

static bool material_glass_cache(const ElfImage&image){
 auto get=image.named("_ZN8impeller12MiGlassCache6TryGetERKNS_19MiGlassBlurCacheKeyERKNS_5TRectIfEEPNS0_14BlurCacheEntryEPb");
 auto tinted=image.named("_ZN8impeller12MiGlassCache12TryGetTintedERKNS_19MiGlassBlurCacheKeyERKNS_17MiGlassTintConfigEjPNSt3_fl10shared_ptrINS_7TextureEEE");
 auto remove=image.named("_ZN8impeller12MiGlassCache6RemoveERKNS_19MiGlassBlurCacheKeyE");
 auto query=image.named("_ZN8impeller19MiGlassFilterHelper15QueryGlassCacheEPKN7flutter18DlGlassImageFilterERKNS_14ContentContextERKNS_5TRectIfEENSt3_fl8optionalImEERKNS_19MiGlassBlurCacheKeyERKNS_6MatrixEb");
 auto draw=image.named("_ZN8impeller6Canvas17DrawHyperMaterialEPKN7flutter13DlImageFilterERKNS_5TRectIfEERKNSt3_fl8optionalIS6_EENSA_IlEENSA_ImEEf");
 auto acquire=image.named("_ZN8impeller6Canvas24AcquireGlassBlurTexturesEPKN7flutter18DlGlassImageFilterERKNS_5TRectIfEENSt3_fl8optionalImEEfRNS_18MiGlassCacheResultEb");
 auto find=image.named("_ZNSt3_fl12__hash_tableINS_17__hash_value_typeIN8impeller19MiGlassBlurCacheKeyENS2_12MiGlassCache14BlurCacheEntryEEENS_22__unordered_map_hasherIS3_S6_NS4_16BlurCacheKeyHashENS_8equal_toIS3_EELb1EEENS_21__unordered_map_equalIS3_S6_SA_S8_Lb1EEENS_9allocatorIS6_EEE4findIS3_EENS_15__hash_iteratorIPNS_11__hash_nodeIS6_PvEEEERKT_");
 if(get.size()!=1||tinted.size()!=1||remove.size()!=1||query.size()!=1||draw.size()!=1||acquire.size()!=1||find.size()!=1)return false;
 if(direct_calls(image,draw.front(),get.front().value).empty()||
    direct_calls(image,draw.front(),tinted.front().value).size()!=1||
    direct_calls(image,acquire.front(),query.front().value).empty()||
    direct_calls(image,query.front(),get.front().value).size()!=1||
    direct_calls(image,query.front(),remove.front().value).empty()||
    direct_calls(image,remove.front(),find.front().value).empty())return false;
 auto lookups=direct_calls(image,get.front(),find.front().value);
 if(lookups.size()!=1)return false;

 // The hash table reads the five key fields through x2. Register
 // destinations and load ordering can vary without changing the layout.
 bool mode=false,backdrop=false,pass_blur=false,radii=false;
 const auto&lookup=find.front();
 for(uint64_t pc=lookup.value,end=pc+std::min<uint64_t>(lookup.size,24*4);pc+4<=end;pc+=4){
  uint32_t word=image.word(pc),opcode=word&0xffc003e0;
  int64_t offset=instruction_immediate(word);
  mode|=opcode==0xb9800040&&offset==0;
  backdrop|=opcode==0xf9400040&&offset==8;
  pass_blur|=opcode==0x39400040&&offset==0x10;
  radii|=opcode==0x69400040&&offset==0x14;
 }
 if(!mode||!backdrop||!pass_blur||!radii)return false;

 // TryGet clears its optional bool before looking up the key. Accept an
 // x4 alias and intervening instructions between the null check and store.
 uint32_t out_registers=1u<<4;
 uint64_t null_exit=0;
 for(uint64_t pc=get.front().value,end=std::min(lookups.front(),pc+24*4);pc<end;pc+=4){
  uint32_t word=image.word(pc);
  if((word&0xffe0ffe0)==0xaa0003e0&&(out_registers&(1u<<((word>>16)&31))))
   out_registers|=1u<<(word&31);
  if((word&0xff000000)==0xb4000000&&(out_registers&(1u<<(word&31))))
   null_exit=branch_target(word,pc);
  if((word&0xfffffc1f)==0x3900001f&&(out_registers&(1u<<((word>>5)&31)))&&pc<null_exit&&null_exit<=end)
   return true;
 }
 return false;
}

static bool capture_layer_hash_abi(const ElfImage&image){
 auto hashes=image.named("_getHash");
 if(hashes.size()!=1)return false;
 // The native bridge invokes this Dart leaf with x15 and x26 prepared.
 // Verify the entire leaf, including the in-object atomic hash update, so
 // there can be no call or safepoint on the temporary native stack.
 const uint32_t leaf[]={
  0xf94001e0,0xb8403000,0xb4000060,0xd37ff800,0xd65f03c0,
  0xf9445b41,0x92407c22,0xd360fc23,0xd29b4c21,0xf2bfffe1,
  0x9b027c21,0x8b030021,0xf9045b41,0x92407421,0xb4fffee1,
  0xf94001e0,0xd1000400,0xd3607c23,0xc85f7c02,0xd360fc44,
  0xb50000c4,0xaa030042,0xc8047c02,0xb5ffff64,0xd37ff820,
  0xd65f03c0,0xd5033f5f,0xd37ff880,0xd65f03c0,
 };
 const auto*code=image.at(hashes.front().value,sizeof(leaf));
 return code&&image.symbol_end(hashes.front())>=hashes.front().value+sizeof(leaf)&&
        !memcmp(code,leaf,sizeof(leaf));
}

static bool plan_material_scroll(const ElfImage&image,uintptr_t live,uintptr_t adapter,std::vector<Patch>&patches){
 auto workspace=image.named("WorkspaceGetxController._folderClingController");
 auto scrolling=image.named("MamlVisibilitySignals.setScrolling");
 auto set_value=image.named("RxObjectMixin.value=");
 auto position=image.named("_PagePosition.page");
 if(workspace.size()!=1||scrolling.size()!=1||set_value.empty()||position.size()!=1)return false;
 // The compiler emits the anonymous page listener in the gap after this
 // named getter. Qualify the listener using its page-position call.
 auto listener=workspace.front();
 const uint64_t end=image.symbol_end(listener);
 listener.value+=listener.size;
 if(listener.value>=end||image.word(listener.value)!=0xa9bf79fd)return false;
 listener.size=end-listener.value;
 auto calls=direct_calls(image,listener,scrolling.front().value);
 auto page_calls=direct_calls(image,listener,position.front().value);
 if(calls.size()!=1||page_calls.empty()||page_calls.front()>=calls.front())return false;
 const uint64_t call=calls.front(),entry=scrolling.front().value;
 // The workspace selects a canonical bool from the page-position
 // comparison, spills it, and reloads the same value in argument x2.
 if(call<listener.value+56||image.word(call-56)!=0x910082d0||
    image.word(call-52)!=0x9100c2d1||image.word(call-48)!=0x9a91c202||
    image.word(call-44)!=0xf81f03a2||image.word(call-4)!=0xf85f03a2)return false;
 const uint32_t setter=image.word(entry+84);
 const bool sets_value=(setter&0xfc000000)==0x94000000&&
  std::any_of(set_value.begin(),set_value.end(),[&](const Symbol&symbol){
   return symbol.value==branch_target(setter,entry+84);
  });
 return sets_value&&image.word(entry+20)==0xf81e83a2&&
        image.word(entry+48)==0xf85e83a0&&image.word(entry+80)==0xaa0003e2&&
        image.word(entry+88)==0xf85e83a0&&
        (image.word(entry+92)&0xfff8001f)==0x36200000&&
        add_widget_branch(image,live,call,adapter,2,true,patches);
}

static bool plan_material_scene(const ElfImage&image,uintptr_t live,uintptr_t adapter,std::vector<Patch>&patches){
 auto detector=image.named("SceneTransitionDetector.detectSceneTransition");
 auto sender=image.named("SceneAnimationSignalManager.sendSignal");
 auto processor=image.named("SceneAnimationSignalManager._processSignal");
 auto names=image.named("SceneAnimationSignalType._enumToString");
 if(detector.size()!=1||sender.size()!=1||processor.size()!=1||names.size()!=1)return false;
 auto calls=direct_calls(image,detector.front(),sender.front().value);
 if(calls.size()!=1)return false;
 const auto start=detector.front().value,call=calls.front();
 // The detector moves the animation signal from x3 to the sender's x2.
 if(call!=start+44||image.word(start+8)!=0xaa0303f0||
    image.word(start+12)!=0xaa0203e3||image.word(start+16)!=0xaa1003e2||
    image.word(call-4)!=0xaa0003e1)return false;
 auto process_calls=direct_calls(image,sender.front(),processor.front().value);
 const auto send=sender.front().value,process=processor.front().value;
 // The sender forwards x2. The processor loads signal.type before
 // interpolating the enum's name.
 if(process_calls.size()!=1||image.word(send+16)!=0xf81f03a2||
    image.word(process+12)!=0xf81a83a2||image.word(process+0x50)!=0xf85a83a5||
    image.word(process+0x54)!=0xb84070a6||image.word(process+0x58)!=0x8b1c80c6||
    image.word(process_calls.front()-16)!=0xb840f002||
    image.word(process_calls.front()-12)!=0x8b1c8042)return false;
 // Check the compressed enum-name load used by the original interpolation.
 unsigned name_loads=0;
 const auto&name=names.front();
 for(uint64_t pc=name.value;pc+12<=image.symbol_end(name);pc+=4)
  if(image.word(pc)==0xb840f022&&image.word(pc+4)==0x8b1c8042&&image.word(pc+8)==0xb8013002)
   ++name_loads;
 return name_loads==1&&add_widget_branch(image,live,call,adapter,2,true,patches);
}

static bool plan_maml_repaint(const ElfImage&image,uintptr_t live,uintptr_t adapter,Resolved&out){
 auto keys=image.named("MamlIconCache.getGlobalKeyForTag");
 auto elements=image.named("GlobalKey._currentElement");
 auto hashes=image.named("_getHash");
 auto lengths=image.named("_HashVMBase._usedData");
 auto iterators=image.named("_CompactIterator.moveNext");
 auto strings=image.named("allocateOneByteString");
 auto controllers=image.named("ShortcutIconGetxController.get");
 auto render_objects=image.named("Element.findRenderObject");
 if(keys.size()!=1||elements.size()!=1||hashes.size()!=1||lengths.size()!=1||
    iterators.size()!=1||strings.size()!=1||controllers.size()!=1||render_objects.size()!=1)return false;

 const uint64_t key=keys.front().value,element=elements.front().value;
 const uint64_t hash=hashes.front().value,length=lengths.front().value,iterator=iterators.front().value;
 const uint64_t string=strings.front().value;
 // Both registries use the current isolate field table. The element
 // lookup binds BuildOwner's map, then checks the compact map's data sentinel.
 uint32_t table=image.word(key+0x14),tags=image.word(key+0x18),binding=image.word(element+0x14);
 uint32_t owner=image.word(element+0x20),registry=image.word(element+0x34);
 if((table&0xffc003ff)!=0xf9400340||table!=image.word(element+0x10)||
    (tags&0xffc003ff)!=0xf9400000||(binding&0xffc003ff)!=0xf9400000||
    image.word(key+0x1c)!=0xf9402370||image.word(key+0x20)!=0x6b10001f||
    (owner&0xffe0001f)!=0xd2800011||image.word(element+0x24)!=0xb8716801||
    image.word(element+0x28)!=0x8b1c8021||(registry&0xffe00fff)!=0xb8400020||
    image.word(element+0x38)!=0x8b1c8000||image.word(element+0x3c)!=0xaa0003e1||
    image.word(element+0x4c)!=0xb840f022||image.word(element+0x54)!=0x6b00005f)return false;
 if(!named_widget_call(image,keys.front(),"_LinkedHashMapMixin.putIfAbsent")){
  // The VM emits several generic instantiations, so qualify the actual BL.
  uint32_t call=image.word(key+0x54);
  auto implementations=image.named("_LinkedHashMapMixin.putIfAbsent");
  if((call&0xfc000000)!=0x94000000||std::none_of(implementations.begin(),implementations.end(),[&](const Symbol&s){
   return s.value==branch_target(call,key+0x54);
  }))return false;
 }
 uint32_t lookup=image.word(element+0x44);
 auto lookups=image.named("_LinkedHashMapMixin._getValueOrData");
 if((lookup&0xfc000000)!=0x94000000||std::none_of(lookups.begin(),lookups.end(),[&](const Symbol&s){
  return s.value==branch_target(lookup,element+0x44);
 }))return false;

 // Corroborate the model identity hash, compact-map cursor and string
 // layout through the running ELF's own VM stubs.
 if(image.word(hash)!=0xf94001e0||image.word(hash+4)!=0xb8403000||image.word(hash+12)!=0xd37ff800||
    image.word(length)!=0xf94001e1||image.word(length+4)!=0xb8413020||
    image.word(iterator+0x64)!=0xb840b066||image.word(iterator+0x68)!=0x93417cc7||
    image.word(iterator+0x90)!=0x8b080870||image.word(iterator+0x94)!=0xb840f200||
    image.word(iterator+0x98)!=0x8b1c8000||image.word(iterator+0x9c)!=0x6b03001f)return false;
 uint32_t first=image.word(string+0x58),second=image.word(string+0x5c);
 if((first&0xffe0001f)!=0xd2800010||(second&0xffe0001f)!=0xf2a00010||
    image.word(string+0x60)!=0xaa100042||image.word(string+0x64)!=0xf81ff002||
    image.word(string+0x6c)!=0xb8007006)return false;
 uint32_t tag=uint32_t(instruction_immediate(first))|uint32_t(instruction_immediate(second));
 if((tag&0xfff)!=0x5c||tag>>12==0)return false;

 std::vector<Patch> patches;
 for(const char*name:{"SfAnimIconCapture._buildLayersFromIconInfo","SfWindowAnimImplementor.prepareIconLayers"}){
  auto functions=image.named(name);
  if(functions.size()!=1)return false;
  const auto&function=functions.front();
  auto controller_calls=direct_calls(image,function,controllers.front().value);
  auto key_calls=direct_calls(image,function,element);
  size_t count=0;
  for(uint64_t call:key_calls){
   uint32_t load_model=image.word(call-24),load_key=image.word(call-8);
   unsigned model_reg=(load_model>>5)&31;
   if((model_reg!=0&&model_reg!=2)||(load_model&~0x3e0u)!=0xf8417001||
      image.word(call-20)!=0xf100203f||(image.word(call-16)&0xff00001f)!=0x54000001||
      (image.word(call-12)&0xffe0001f)!=0xd2800011||
      load_key!=(0xb8716801|((model_reg^2u)<<5))||image.word(call-4)!=0x8b1c8021||
      image.word(call+4)!=0x6b16001f||image.word(call+8)!=0x54000061||
      image.word(call+20)!=0xaa0003e1||(image.word(call+24)&0xfc000000)!=0x94000000||
      branch_target(image.word(call+24),call+24)!=render_objects.front().value||
      std::none_of(controller_calls.begin(),controller_calls.end(),[&](uint64_t pc){return pc<call-24&&call-pc<=64;}))
    continue;
   if(!add_widget_branch(image,live,call-4,adapter+(model_reg?8:0),2,true,patches))return false;
   count++;
  }
  if(count!=1)return false;
 }
 out.maml_repaint={uint32_t(instruction_immediate(table)),uint32_t(instruction_immediate(tags)),
                   uint32_t(instruction_immediate(binding)),uint32_t(instruction_immediate(owner)),
                   uint32_t(instruction_immediate(registry)),tag>>12};
 out.patches.insert(out.patches.end(),patches.begin(),patches.end());
 return true;
}

// A Hero flight inflates the icon's keyed capture boundary again in its
// overlay shuttle and in MultiFlyUnderlay. GlobalKey reparenting would then
// detach the boundary from the attached icon or from the visible copy. Guard
// the parent spill in _retakeInactiveElement, after the original lookup and
// Widget.canUpdate check, and derive every Dart field the guard reads.
static bool plan_icon_retake(const ElfImage&image,uintptr_t live,uintptr_t adapter,uintptr_t registration_adapter,Resolved&out){
 auto retakes=image.named("Element._retakeInactiveElement");
 auto elements=image.named("GlobalKey._currentElement");
 auto updates=image.named("Widget.canUpdate");
 auto widgets=image.named("Element.widget");
 auto stateless=image.named("StatelessElement.build");
 auto desktop=image.named("ShortcutIconWidget.buildDesktopIcon");
 auto icon_build=image.named("ShortcutIconWidget.build");
 auto capture=image.named("SfAnimIconCapture._buildLayersFromIconInfo");
 auto controllers=image.named("ShortcutIconGetxController.get");
 if(retakes.size()!=1||elements.size()!=1||updates.size()!=1||widgets.size()!=1||stateless.size()!=1||
    desktop.size()!=1||icon_build.size()!=1||capture.size()!=1||controllers.size()!=1)return false;
 const auto&retake=retakes.front();
 if(!dart_frame(image,retake)||(image.word(retake.value+8)&0xffc003ff)!=0xd10001ef)return false;
 // (this, key, newWidget) arrive in x1..x3. The prologue spills this at
 // FP-8 and newWidget at FP-0x10 before looking up the key's element.
 const uint32_t shuffle[]={0xaa0203e0,0xaa0303f0,0xaa0203e3,0xaa1003e2,0xaa0103f0,0xaa0303e1,0xaa1003e3,0xf81f83a3,0xf81f03a2};
 for(size_t i=0;i<sizeof(shuffle)/sizeof(shuffle[0]);i++)if(image.word(retake.value+12+4*i)!=shuffle[i])return false;
 auto lookups=direct_calls(image,retake,elements.front().value);
 auto compares=direct_calls(image,retake,updates.front().value);
 const uint64_t lookup=retake.value+12+4*9;
 // A missing element returns null through the function's own epilogue.
 if(lookups.size()!=1||lookups.front()!=lookup||compares.size()!=1||
    image.word(lookup+4)!=0xaa0003e2||image.word(lookup+8)!=0xf81e83a2||image.word(lookup+12)!=0x6b16005f||
    (image.word(lookup+16)&0xff00001f)!=0x54000001||image.word(lookup+20)!=0xaa1603e0||
    image.word(lookup+24)!=0xaa1d03ef||image.word(lookup+28)!=0xa8c179fd||image.word(lookup+32)!=0xd65f03c0)return false;
 uint64_t site=0;uint32_t parent=0;
 for(uint64_t pc=compares.front()+4;pc+12<=retake.value+retake.size;pc+=4){
  uint32_t load=image.word(pc);
  if(image.word(pc-4)!=0xf85e83a3||(load&0xffe00fff)!=0xb8400064||image.word(pc+4)!=0x8b1c8084||
     image.word(pc+8)!=0xf81f03a4||image.word(pc+12)!=0x6b16009f||(image.word(pc+16)&0xff00001f)!=0x54000000)continue;
  if(site)return false;
  site=pc+8;parent=uint32_t(instruction_immediate(load));
 }
 if(!site)return false;

 // Element.widget is a leaf load of Element._widget with a null check.
 const uint64_t getter=widgets.front().value;
 uint32_t widget=image.word(getter);
 if((widget&0xffe00fff)!=0xb8400020||image.word(getter+4)!=0x8b1c8000||image.word(getter+8)!=0x6b16001f)return false;
 // Widget.canUpdate compares the receiver and argument keys at one offset.
 uint32_t key=0;
 for(uint64_t pc=updates.front().value;pc+20<=updates.front().value+updates.front().size;pc+=4){
  uint32_t first=image.word(pc+8),second=image.word(pc+16);
  if(image.word(pc)!=0xf85f83a1||image.word(pc+4)!=0xf85f03a0||(first&0xffe00fff)!=0xb8400022||
     image.word(pc+12)!=0x8b1c8042||(second&0xffe00fff)!=0xb8400001||
     instruction_immediate(first)!=instruction_immediate(second))continue;
  key=uint32_t(instruction_immediate(first));break;
 }
 // buildDesktopIcon loads the late controller and compares it with the
 // uninitialized-late sentinel from the object pool.
 uint32_t controller=0,sentinel=0;
 for(uint64_t pc=desktop.front().value+16;pc+16<=desktop.front().value+std::min<uint64_t>(desktop.front().size,64);pc+=4){
  uint32_t load=image.word(pc),pool=image.word(pc+8);
  // x2 holds the receiver, copied from x1 just before this load.
  if((image.word(pc-4)!=0xaa0103e2&&image.word(pc-8)!=0xaa0103e2)||(load&0xffe00fff)!=0xb8400040||
     image.word(pc+4)!=0x8b1c8000||(pool&0xffc003ff)!=0xf9400370||image.word(pc+12)!=0x6b10001f)continue;
  controller=uint32_t(instruction_immediate(load));sentinel=uint32_t(instruction_immediate(pool));break;
 }
 // The capture reads controller.captureKey before GlobalKey._currentElement.
 uint32_t capture_key=0;unsigned captures=0;
 auto controller_calls=direct_calls(image,capture.front(),controllers.front().value);
 for(uint64_t call:direct_calls(image,capture.front(),elements.front().value)){
  uint32_t value=image.word(call-12),read=image.word(call-8);
  if((value&0xffe0001f)!=0xd2800011||(read&0xfffffc1f)!=0xb8716801||image.word(call-4)!=0x8b1c8021||
     std::none_of(controller_calls.begin(),controller_calls.end(),[&](uint64_t pc){return pc<call&&call-pc<=64;}))continue;
  capture_key=uint32_t(instruction_immediate(value));captures++;
 }
 // StatelessElement.build calls widget.build through the dispatch table.
 uint32_t selector=0;unsigned dispatches=0;
 for(uint64_t pc=stateless.front().value+8;pc+12<=stateless.front().value+stateless.front().size;pc+=4){
  uint32_t low=image.word(pc-8),high=image.word(pc-4);
  if((low&0xffe0001f)!=0xd2800011||(high&0xffe0001f)!=0xf2a00011||image.word(pc)!=0x8b11001e||
     image.word(pc+4)!=0xf87e7abe||image.word(pc+8)!=0xd63f03c0)continue;
  selector=uint32_t(instruction_immediate(low)|instruction_immediate(high));dispatches++;
 }
 if(!parent||!widget||!key||!controller||!sentinel||captures!=1||!capture_key||dispatches!=1||!selector)return false;

 // Element.mount inlines the GlobalKey class-range test before registering
 // owner.registry[key] = this. A flight copy must not replace the attached
 // icon's registration, whose later unmount would then drop the key.
 auto mounts=image.named("Element.mount");
 if(mounts.size()!=1||!dart_frame(image,mounts.front())||image.word(mounts.front().value+20)!=0xf81f83a1)return false;
 uint64_t range=0,register_call=0;uint32_t owner_field=0;
 for(uint64_t pc=mounts.front().value+24;pc+44<=mounts.front().value+mounts.front().size;pc+=4){
  uint32_t load=image.word(pc),owner=image.word(pc+40);
  if((load&0xffe00fff)!=0xb8400002||uint32_t(instruction_immediate(load))!=key||image.word(pc+4)!=0x8b1c8042||
     image.word(pc+8)!=0xf85ff040||image.word(pc+12)!=0xd34c7c00||(image.word(pc+16)&0xffe0001f)!=0x92800011||
     image.word(pc+20)!=0x8b110010||(image.word(pc+24)&0xffc003ff)!=0xf100021f||image.word(pc+28)!=(0x54000008|((0x20/4)<<5))||
     image.word(pc+32)!=0xf85f83a0||(image.word(pc+36)&0xffe00fff)!=0xb8400001||owner!=0x8b1c8021||
     image.word(pc+44)!=0x6b16003f||(image.word(pc+48)&0xff00001f)!=0x54000000||image.word(pc+52)!=0xaa0003e3||
     (image.word(pc+56)&0xfc000000)!=0x94000000)continue;
  if(range)return false;
  range=pc+28;register_call=pc+56;owner_field=uint32_t(instruction_immediate(image.word(pc+36)));
 }
 if(!range||!owner_field)return false;
 // The registration body stores into BuildOwner's GlobalKey registry, the
 // same map that GlobalKey._currentElement reads.
 const uint64_t registration=branch_target(image.word(register_call),register_call);
 uint32_t registry=image.word(registration+8);
 if(!image.executable(registration,40)||image.word(registration)!=0xa9bf79fd||image.word(registration+4)!=0xaa0f03fd||
    (registry&0xffe00fff)!=0xb8400020||uint32_t(instruction_immediate(registry))!=out.maml_repaint.element_map||
    image.word(registration+12)!=0x8b1c8000||image.word(registration+16)!=0xaa0003e1||
    (image.word(registration+20)&0xfc000000)!=0x94000000||image.word(registration+24)!=0xaa1603e0||
    image.word(registration+28)!=0xaa1d03ef||image.word(registration+32)!=0xa8c179fd||image.word(registration+36)!=0xd65f03c0)return false;

 std::vector<Patch> patches;
 if(!add_widget_branch(image,live,site,adapter,2,true,patches)||
    !add_widget_branch(image,live,range,registration_adapter,2,true,patches))return false;
 out.icon_retake={parent,uint32_t(instruction_immediate(widget)),key,controller,sentinel,capture_key,selector,
                  live+uintptr_t(icon_build.front().value),owner_field};
 out.patches.insert(out.patches.end(),patches.begin(),patches.end());
 return true;
}

bool resolve_program(const char*path,uintptr_t live,const Program&program,Resolved&out,bool allocate){
 ElfImage image;std::string error;if(!image.load(path,error)){out.diagnostic=error;return false;}
 {auto entry=image.named("Dart_CreateIsolateGroup");if(entry.size()==1&&image.executable(entry.front().value,16))out.isolate_group_entry=live+uintptr_t(entry.front().value);}
 SceneFrame scene;
 const char*scene_failure=program.config_count?locate_scene_frame(image,scene):nullptr;
 if(scene_failure)scene=SceneFrame{};
 Locator locator(image,program,scene.marks);out.addresses.resize(program.anchor_count);
 for(size_t i=0;i<program.anchor_count;i++)out.addresses[i]=locator.resolve(i);
 size_t page=size_t(sysconf(_SC_PAGESIZE));std::set<uint32_t>pages;
 for(size_t i=0;i<program.unit_count;i++)if(program.units[i].shadow){const auto&u=program.units[i];for(uint32_t at=u.source&~uint32_t(page-1),end=(u.source+u.size+page-1)&~uint32_t(page-1);at<end;at+=uint32_t(page))pages.insert(at);}
 size_t span=pages.size()*page;out.shadow_size=span;
 out.shadow=allocate&&span?reserve_near(live,span):live+0x3000000;
 if(span&&!out.shadow){out.diagnostic="no nearby private adapter range";return false;}
 size_t next=0;for(uint32_t at:pages)out.adapter_pages.emplace_back(at,out.shadow+(next++)*page);
 uint32_t ready=0;for(size_t i=0;i<program.group_count;i++)ready|=program.groups[i];
 if(scene_failure){
  ready&=~2u;
  out.diagnostic+=std::string("Dart scene frame ABI: ")+scene_failure+"; ";
 }
 for(size_t i=0;i<program.constraint_count;i++){
  const auto&c=program.constraints[i];uint32_t left=out.addresses[size_t(c.left)];bool valid=false;
  if(c.kind==ConstraintKind::VirtualSlot){
   // The anchor recipe fixes LDR Xt,[Xn,#imm]; only the slot may move.
   bool decoded=false;int64_t value=left?instruction_immediate(image.word(left),&decoded):0;
   valid=decoded&&(image.word(left)&0xffc00000)==0xf9400000&&value>0&&size_t(c.delta)<out.virtual_slots.size();
   if(valid)out.virtual_slots[size_t(c.delta)]=uint32_t(value);
   else {ready&=~c.group;out.diagnostic+="engine virtual-call slot unresolved; ";}
   continue;
  }
  if(c.right<0){bool decoded=false;int64_t value=instruction_immediate(image.word(left),&decoded);valid=left&&decoded&&(c.kind==ConstraintKind::Minimum?value>=c.delta:value==c.delta);}
  else {uint32_t right=out.addresses[size_t(c.right)];valid=left&&right&&int64_t(left)-int64_t(right)==c.delta;}
  if(!valid){ready&=~c.group;out.diagnostic+=c.kind==ConstraintKind::Minimum?"Dart frame is smaller than the adapter ABI requires; ":c.right<0?"Dart object/frame layout constraint failed; ":"Dart allocation return-PC constraint failed; ";}
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
 std::vector<bool>selected(program.unit_count,true);
 constexpr unsigned wrap_groups=HG_WIDGET_VECTOR|HG_WIDGET_COMPOUND;
 std::vector<std::pair<unsigned,const char*>>unresolved_wrapping;
 for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];uintptr_t address=unit.shadow?adapter_address(out,unit.source):(unit.anchor>=0&&out.addresses[size_t(unit.anchor)]?live+out.addresses[size_t(unit.anchor)]:0);
  locations.push_back(address);payloads.emplace_back(unit.bytes,unit.bytes+unit.size);
  bool wrap_unit=program.widget_wrap&&i==program.widget_wrap->unit;
  if(wrap_unit&&(ready&HG_WIDGET_COMPOUND)){selected[i]=false;continue;}
  if(unit.guard>=0){
   if(locator.candidates(size_t(unit.guard))>1){ready&=~uint32_t(unit.group);out.diagnostic+=std::string(unit.label)+": ambiguous layout; ";}
   selected[i]=(out.addresses[size_t(unit.guard)]!=0)==unit.when_guard_matches;
   if(!selected[i])continue;
  }
  bool ok=address&&(unit.shadow||image.executable(address-live,unit.size));
  for(unsigned j=0;j<unit.relocation_count&&ok;j++){
   const auto&r=unit.relocations[j];uint64_t dest=target_address(r.target);uint32_t word=0;
   if(r.offset+4>unit.size||!dest){ok=false;break;}
   if(r.kind==8)word=image.word(dest-live);
   else if(r.kind==9||r.kind==10){
    uint32_t original=image.word(dest-live),reg=original&31;
    if(r.kind==9){
     ok=(original&0xffe00fe0)==0xf80003a0&&reg<29; // stur xReg,[Dart FP,#slot]
     word=(r.original&0xfffffc00)|(reg<<5)|reg; // orr wReg,wReg,#0xffffff
    }else{
     ok=(original&0xffffffe0)==0x36000040&&reg<29; // tbz wReg,#0,store
     word=0x92800020|reg; // Dart Smi(-1), the default white ARGB
    }
   }
   else ok=encode_relative(r.original,address+r.offset,dest,r.kind,word);
   if(!ok){char detail[192];snprintf(detail,sizeof(detail),"relocation source=%x+%x kind=%u pc=%llx destination=%llx; ",unit.source,r.offset,r.kind,(unsigned long long)(address+r.offset),(unsigned long long)dest);out.diagnostic+=detail;}
   if(ok)memcpy(payloads.back().data()+r.offset,&word,4);
  }
  if(!ok){
   if(wrap_unit)selected[i]=false;
   else{
    ready&=~uint32_t(unit.group);
    if(program.widget_wrap&&(unit.group&wrap_groups)){
     selected[i]=false;unresolved_wrapping.emplace_back(unit.group,unit.label);
    }else out.diagnostic+=std::string(unit.label)+": unresolved address/relocation; ";
   }
  }
 }
 if(program.widget_wrap){
  const auto&wrap=*program.widget_wrap;
  if((ready&HG_WIDGET_ARTWORK)&&plan_background_filter(image,live,out.addresses[size_t(wrap.image_filter_anchor)],
    adapter_address(out,wrap.image_filter),out.patches)){
   for(size_t i=0;i<program.unit_count;i++)if(program.units[i].anchor==wrap.image_filter_anchor)selected[i]=false;
   out.adapted_widget_paths|=HG_WIDGET_ARTWORK;
  }
  if((ready&4)&&!(ready&HG_WIDGET_VECTOR)){
   uint64_t source=out.addresses[size_t(wrap.filter_anchor)];
   if(source&&plan_vector_filters(image,live,live+source,out.patches))out.adapted_widget_paths|=HG_WIDGET_VECTOR;
  }
  if(selected[wrap.unit]){
   if((ready&4)&&plan_compound_wrapping(image,live,wrap,out))out.adapted_widget_paths|=HG_WIDGET_COMPOUND;
   selected[wrap.unit]=(out.adapted_widget_paths&HG_WIDGET_COMPOUND)!=0;
  }
  ready|=out.adapted_widget_paths;
 }
 for(const auto&failure:unresolved_wrapping)if(!(out.adapted_widget_paths&failure.first))out.diagnostic+=std::string(failure.second)+": unresolved address/relocation; ";
 std::array<uint32_t,HG_CAPTURE_CONFIG_WORDS>config{};std::array<bool,HG_CAPTURE_CONFIG_WORDS>known{};
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
  for(unsigned slot:{HG_CAPTURE_CLIP_SUPERELLIPSE,HG_CAPTURE_OPACITY,HG_CAPTURE_CLIP_RRECT,HG_CAPTURE_COLOR_FILTER}){
   if(!known[slot]||config[slot]<=config[0]||config[slot]>=config[15]){
    ready&=~2u;out.diagnostic+="local icon layer allocation ABI unresolved; ";break;
   }
  }
  if(!widget){ready&=~4u;out.diagnostic+="Android container layout unresolved; ";}
  config[HG_CAPTURE_LAYER_SLOT]=uint32_t(scene.layer);
  config[HG_CAPTURE_BOUNDS_SLOT]=uint32_t(scene.bounds);
  config[HG_CAPTURE_ROUND_SLOT]=uint32_t(scene.round);
  config[HG_CAPTURE_VIEW_SLOT]=uint32_t(scene.view);
  for(size_t i=0;i<program.unit_count;i++)if(program.units[i].source==program.config_source){
   if(payloads[i].size()==sizeof(config))memcpy(payloads[i].data(),config.data(),sizeof(config));
   else {ready&=~(2u|4u|uint32_t(HG_WIDGET_MONOCHROME));out.diagnostic+="capture configuration payload size mismatch; ";}
  }
 }
 // Each monochrome route still needs the common widget adapters.
 if(!(ready&4))ready&=~uint32_t(HG_WIDGET_MONOCHROME);
 if((ready&4)&&program.widget_wrap&&!image.named("DensitySemantics.layoutDensity").empty()&&
    !plan_widget_layout_density(image,live,out.patches))out.diagnostic+="Android widget layout-density conversion unresolved; ";
 if((ready&2)&&!strcmp(program.image,"libhyper_os_flutter.so")){
  if(!material_frame_info(image)){
   ready&=~2u;out.diagnostic+="material screen-transform FrameInfo ABI unresolved; ";
  }
  if(!material_glass_cache(image)){
   ready&=~2u;out.diagnostic+="material glass cache ABI unresolved; ";
  }
 }
 if((ready&2)&&!strcmp(program.image,"libapp.so")){
  if(!capture_layer_hash_abi(image)){
   ready&=~2u;out.diagnostic+="detached capture layer identity ABI unresolved; ";
  }
  if(!recapture_return_boundary(image,live,out.patches)){
   ready&=~2u;out.diagnostic+="SF return reuse and release flow unresolved; ";
  }
  for(const char*name:{"SfAnimIconCapture._buildLayersFromIconInfo","SfWindowAnimImplementor.prepareIconLayers"})if(!prefer_live_boundary(image,live,name,out.patches,out.diagnostic)){
   ready&=~2u;out.diagnostic+="live icon boundary cache guard unresolved; ";break;
  }
  if(ready&2){
   uintptr_t repaint=0,material_scene=0,material_scroll=0,icon_retake=0,icon_register=0;
   for(size_t i=0;i<program.unit_count;i++){
    const auto&unit=program.units[i];
    if(!strcmp(unit.label,".maml_repaint"))repaint=adapter_address(out,unit.source);
    if(!strcmp(unit.label,".material_scene"))material_scene=adapter_address(out,unit.source);
    if(!strcmp(unit.label,".material_scroll"))material_scroll=adapter_address(out,unit.source);
    if(!strcmp(unit.label,".icon_retake"))icon_retake=adapter_address(out,unit.source);
    if(!strcmp(unit.label,".icon_register"))icon_register=adapter_address(out,unit.source);
   }
   if(!repaint||!plan_maml_repaint(image,live,repaint,out)){
    ready&=~2u;out.diagnostic+="MAML repaint registry or SF capture ABI unresolved; ";
   }else if(!material_scene||!plan_material_scene(image,live,material_scene,out.patches)){
    ready&=~2u;out.diagnostic+="material scene signal ABI unresolved; ";
   }else if(!material_scroll||!plan_material_scroll(image,live,material_scroll,out.patches)){
    ready&=~2u;out.diagnostic+="workspace scroll signal ABI unresolved; ";
   }else if(!icon_retake||!icon_register||!plan_icon_retake(image,live,icon_retake,icon_register,out)){
    // Without the guards, Hero flight copies keep stock GlobalKey reparenting.
    out.icon_retake={};out.diagnostic+="icon boundary GlobalKey guard unresolved; ";
   }
  }
 }
 for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];if(!selected[i]||!(ready&unit.group))continue;
  if(unit.shadow){
   if(allocate){uintptr_t begin=locations[i]&~uintptr_t(page-1),end=(locations[i]+unit.size+page-1)&~uintptr_t(page-1);if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_WRITE)){ready&=~uint32_t(unit.group);out.diagnostic+="private adapter mapping failed; ";continue;}memcpy(reinterpret_cast<void*>(locations[i]),payloads[i].data(),unit.size);}
  }else for(size_t offset=0;offset<unit.size;offset+=256){Patch patch{};patch.address=locations[i]+offset;patch.size=uint32_t(std::min<size_t>(256,unit.size-offset));patch.group=unit.group;memcpy(patch.bytes,payloads[i].data()+offset,patch.size);memcpy(patch.original,image.at(locations[i]-live+offset,patch.size),patch.size);out.patches.push_back(patch);}
 }
 if(allocate)for(size_t i=0;i<program.unit_count;i++){
  const auto&unit=program.units[i];if(!selected[i]||!unit.shadow||!(ready&unit.group))continue;
  __builtin___clear_cache(reinterpret_cast<char*>(locations[i]),reinterpret_cast<char*>(locations[i]+unit.size));
  uintptr_t begin=locations[i]&~uintptr_t(page-1),end=(locations[i]+unit.size+page-1)&~uintptr_t(page-1);
  if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_EXEC)){ready&=~uint32_t(unit.group);out.diagnostic+="private adapter execute protection failed; ";}
 }
 bool shadow_used=false;for(size_t i=0;i<program.unit_count;i++)if(selected[i]&&program.units[i].shadow&&(ready&program.units[i].group))shadow_used=true;
 if(allocate&&out.shadow&&!shadow_used){munmap(reinterpret_cast<void*>(out.shadow),out.shadow_size);out.shadow=0;out.shadow_size=0;out.adapter_pages.clear();}
 out.patches.erase(std::remove_if(out.patches.begin(),out.patches.end(),[&](const Patch&p){return !(ready&p.group);}),out.patches.end());out.ready=ready;
 out.adapted_widget_paths&=ready;
 if(!error.empty())out.diagnostic+=error;
 return ready!=0;
}
}
