#include "runtime.h"
#include "resolver.hpp"
#include "adaptive_profiles.hpp"
#include "page_guard.h"
#include "material.h"
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <unistd.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"HyperGlass",__VA_ARGS__)
namespace {
std::atomic<bool> runtime_complete{false};
using Dispatch=void(*)(uintptr_t*);Dispatch original_dispatch=nullptr;
void select_maml_repaint_key(uintptr_t*registers);
void export_dispatch(uintptr_t*r){
 uintptr_t mode=r[15];
 if(mode==9){select_maml_repaint_key(r);return;}
 original_dispatch(r);
 // The Dart adapter reports the stage through the saved scratch register
 // after marking the view ID in its current frame slot.
 if(mode==0&&r[15]){uintptr_t fp=r[28],bounds=*(uintptr_t*)(fp-16);const double*rect=(const double*)(bounds+7);
  hg::queue_material_target(unsigned(r[15]),unsigned(rect[0])-1048576,unsigned(rect[1])-1048576);
 }

 if(mode!=1)return;
 uintptr_t image=r[0],heap=r[27]<<32;uint32_t mark=*(uint32_t*)(image+11)>>1;
 if(mark>>24!=0x15)return;
 uintptr_t peer=hg::image_peer(image,heap);
 hg::crop_export_image(peer,(mark>>12)&4095,mark&4095,unsigned(*(uintptr_t*)(image+15)),unsigned(*(uintptr_t*)(image+23)));
}
// Written by the module settings page into its shared media directory, which
// the launcher reads with its storage access. Absent means the default (on).
bool widget_monochrome(){
 static int value=-1;
 if(value<0){
  value=1;FILE*f=fopen("/storage/emulated/0/Android/media/dev.hyperglass.lsposed/widget-monochrome","re");
  if(f){value=fgetc(f)!='0';fclose(f);}
  if(!value)LOG("Widget artwork keeps provider colors (monochrome off)");
 }
 return value;
}
// -1 keeps initialization pending until the setting can be read reliably.
int badge_monochrome(){
 static int value=-1,last_error=0;
 if(value<0){
  int error=0;
  FILE*f=fopen("/storage/emulated/0/Android/media/dev.hyperglass.lsposed/badge-monochrome","re");
  if(f){
   int setting=fgetc(f);error=ferror(f)?EIO:EAGAIN;fclose(f);
   if(setting=='0'||setting=='1')value=setting-'0';
  }else{
   error=errno;
   // A missing setting means the default only after shared storage is visible.
   if(error==ENOENT&&access("/storage/emulated/0/Android",F_OK)==0)value=0;
  }
  if(value>=0)LOG("Notification badge monochrome %s",value?"on":"off");
  else if(error!=last_error){
   LOG("Notification badge setting pending errno=%d; will retry",error);
   last_error=error;
  }
 }
 return value;
}
struct ImageState { uintptr_t base=0;std::string path;hg::Resolved result;bool attempted=false; };
std::array<ImageState,3> images;

template<class T> T dart_value(uintptr_t address){
 T value;
 memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));
 return value;
}
struct DartMapEntries {
 uintptr_t data=0;
 uint32_t size=0;
};
// Dart's compact map stores alternating compressed keys and values. The
// corresponding iterator skips deleted entries whose key equals the array.
DartMapEntries dart_map_entries(uintptr_t map,uintptr_t heap,uintptr_t nil){
 if(!(map&1)||map==nil)return {};
 uintptr_t data=heap+dart_value<uint32_t>(map+0xf);
 if(!(data&1)||data==nil)return {};
 uint32_t used=dart_value<uint32_t>(map+0x13),length=dart_value<uint32_t>(data+0xb);
 return used<=length&&!(used&3)&&!(length&1)?DartMapEntries{data,used/2}:DartMapEntries{};
}
bool mounted_key(const DartMapEntries&elements,uint32_t key){
 if(!elements.data||key==uint32_t(elements.data))return false;
 for(uint32_t i=0;i<elements.size;i+=2)
  if(dart_value<uint32_t>(elements.data+0xf+4*i)==key)return true;
 return false;
}
void select_maml_repaint_key(uintptr_t*r){
 const auto&abi=images[0].result.maml_repaint;
 const uintptr_t nil=r[21],heap=r[27]<<32,model=r[16];
 if(!abi.string_class||!(model&1)||model==nil)return;
 uint32_t identity=dart_value<uint32_t>(model+3);
 if(!identity)return;
 uintptr_t table=dart_value<uintptr_t>(r[25]+abi.field_table);
 uintptr_t tags=dart_value<uintptr_t>(table+abi.tag_map);
 if(tags==dart_value<uintptr_t>(r[26]+0x40))return; // Lazy static sentinel.
 uintptr_t binding=dart_value<uintptr_t>(table+abi.binding);
 if(!(binding&1)||binding==nil)return;
 uintptr_t owner=heap+dart_value<uint32_t>(binding+abi.build_owner);
 if(!(owner&1)||owner==nil)return;
 auto elements=dart_map_entries(heap+dart_value<uint32_t>(owner+abi.element_map),heap,nil);
 if(!elements.size||mounted_key(elements,uint32_t(r[1])))return;

 // A mounted MAML boundary uses the same model identity across render
 // surfaces. Match its repaint tag while allowing an instance namespace.
 char suffix[32]="mamlIcon_";
 char*end=std::to_chars(suffix+sizeof("mamlIcon_")-1,suffix+sizeof(suffix)-sizeof("_repaint"),identity).ptr;
 memcpy(end,"_repaint",sizeof("_repaint")-1);
 size_t suffix_size=end-suffix+sizeof("_repaint")-1;
 auto entries=dart_map_entries(tags,heap,nil);
 uintptr_t selected=0;
 for(uint32_t i=0;i<entries.size;i+=2){
  uintptr_t slot=entries.data+0xf+4*i;
  uintptr_t tag=heap+dart_value<uint32_t>(slot);
  if(!(tag&1)||tag==nil||tag==entries.data||dart_value<uint32_t>(tag-1)>>12!=abi.string_class)continue;
  uint32_t length=dart_value<uint32_t>(tag+7)>>1;
  if(length<suffix_size)continue;
  size_t prefix=length-suffix_size;
  const char*text=reinterpret_cast<const char*>(tag+15);
  if((prefix&&text[prefix-1]!='_')||memcmp(text+prefix,suffix,suffix_size))continue;
  uint32_t key=dart_value<uint32_t>(slot+4);
  if(!mounted_key(elements,key))continue;
  if(selected&&selected!=heap+key)return; // An ambiguous source keeps the original fallback.
  selected=heap+key;
 }
 if(selected)r[1]=selected;
}

std::mutex mutex;
struct Mapping {uintptr_t begin,end;unsigned long offset;std::string path,permission;};
struct Layout {std::vector<Elf64_Phdr>segments;uint64_t offset=0;bool valid=false;};
std::map<std::string,Layout>layouts;
bool executable_mapping(uintptr_t base,const std::string&path,const Layout&layout,const std::vector<Mapping>&maps){
 bool executable=false;
 for(const auto&ph:layout.segments)if(ph.p_type==PT_LOAD&&(ph.p_flags&PF_X)){
  uintptr_t cursor=base+ph.p_vaddr,end=cursor+ph.p_filesz;
  // Inline hooks can split one ELF segment into adjacent executable VMAs.
  for(const auto&m:maps)if(m.path==path&&m.permission[2]=='x'&&m.begin<=cursor&&cursor<m.end){
   cursor=m.end<end?m.end:end;if(cursor==end)break;
  }
  if(cursor!=end)return false;executable=true;
 }
 return executable;
}
void find_images(){
 if(std::all_of(images.begin(),images.end(),[](const ImageState&s){return s.base!=0;}))return;
 FILE*f=fopen("/proc/self/maps","re");if(!f)return;
 char line[1536],path[1024],permission[8];unsigned long begin,end,offset;
 std::vector<Mapping>maps;
 while(fgets(line,sizeof(line),f))if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %1023s",&begin,&end,permission,&offset,path)==5)maps.push_back({begin,end,offset,path,permission});
 fclose(f);
 for(size_t i=0;i<images.size();i++)if(!images[i].base){
  for(const auto&m:maps){
   const char*leaf=strrchr(m.path.c_str(),'/');
   if(m.permission!="r--p"||!leaf)continue;
   std::string path=m.path;
   if(path.size()>4&&path.substr(path.size()-4)==".apk")path+="!/lib/arm64-v8a/"+std::string(hg_programs[i]->image);
   else if(strcmp(leaf+1,hg_programs[i]->image))continue;
   auto item=layouts.find(path);
   if(item==layouts.end()){Layout layout;std::string error;layout.valid=hg::image_layout(path.c_str(),layout.segments,layout.offset,error);item=layouts.emplace(path,std::move(layout)).first;}
   const auto&layout=item->second;
   if(!layout.valid||m.offset!=layout.offset||!executable_mapping(m.begin,m.path,layout,maps))continue;
   images[i].base=m.begin;images[i].path=path;break;
  }
 }
}
}
extern "C" unsigned hg_runtime_tick(const char*phase){
 // A loader callback can hold the linker lock while the worker resolves
 // symbols. Let the worker retry instead of waiting in the callback.
 std::unique_lock<std::mutex>lock(mutex,std::try_to_lock);
 if(!lock.owns_lock())return 0;
 find_images();
 if(!hg_page_guard_ready())return 0;
 for(size_t i=0;i<images.size();i++){
  auto&s=images[i];if(!s.base||s.attempted)continue;s.attempted=true;
  hg::resolve_program(s.path.c_str(),s.base,*hg_programs[i],s.result);
  LOG("RESOLVED image=%s features=%u sites=%zu diagnostic=%s",hg_programs[i]->image,s.result.ready,s.result.patches.size(),s.result.diagnostic.c_str());

 }
 // Marked Dart exports and their native scheduler share one commit batch.
 bool snapshots=(images[0].result.ready&2)&&(images[1].result.ready&2);
 if(snapshots)snapshots=hg::install_material_hooks(images[1].base,*hg_programs[1],images[1].result)&&hg::install_backdrop_stream_hooks();
 if(snapshots&&!original_dispatch){
  uintptr_t target=hg::adapter_address(images[0].result,0xce2770);
  snapshots=target&&hg_install_native_hook((void*)target,(void*)export_dispatch,(void**)&original_dispatch)==0&&original_dispatch;
 }
 std::vector<hg_patch>batch;unsigned ready=0;bool settings_pending=false;
 for(size_t i=0;i<images.size();i++){
  const auto&r=images[i].result;unsigned enabled=r.ready;
  if(!snapshots)enabled&=~2u;
  if(i==0&&(enabled&HG_WIDGET_MONOCHROME)&&!widget_monochrome())enabled&=~unsigned(HG_WIDGET_MONOCHROME);
  if(i==0&&(enabled&32)){
   int monochrome=badge_monochrome();
   if(monochrome!=1)enabled&=~32u;
   settings_pending=monochrome<0;
  }
  ready|=enabled;
  for(const auto&p:r.patches)if(p.group&enabled)batch.push_back(p);
 }
 int rc=batch.empty()?0:hg_apply_batch(batch.data(),batch.size());
 static int last=0;
 if(rc>0)LOG("APPLIED pid=%d features=%u sites=%zu phase=%s",getpid(),ready,batch.size(),phase);
 else if(rc<0&&rc!=last)LOG("Patch deferred rc=%d features=%u sites=%zu",rc,ready,batch.size());
 last=rc;
 if(rc>=0 && (ready&8))hg_install_resources();
 bool all_attempted=std::all_of(images.begin(),images.end(),[](const ImageState&s){return s.attempted;});
 bool snapshots_pending=(images[0].result.ready&2)&&(images[1].result.ready&2)&&!snapshots;
 if(all_attempted&&!snapshots_pending&&!settings_pending&&(rc>=0||rc==-4))runtime_complete.store(true,std::memory_order_release);
 return rc>=0?ready:0;
}
extern "C" bool hg_runtime_complete(){
 return runtime_complete.load(std::memory_order_acquire);
}
