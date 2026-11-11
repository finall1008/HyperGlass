#include "runtime.h"
#include "resolver.hpp"
#include "adaptive_profiles.hpp"
#include "page_guard.h"
#include "material.h"
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <unistd.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"HyperGlass",__VA_ARGS__)
namespace {
std::atomic<bool> runtime_complete{false};
using Dispatch=void(*)(uintptr_t*);Dispatch original_dispatch=nullptr;
void export_dispatch(uintptr_t*r){
 uintptr_t mode=r[15];original_dispatch(r);
 if(mode==0){uintptr_t fp=r[28],bounds=*(uintptr_t*)(fp-16);const double*rect=(const double*)(bounds+7);
  if(rect[0]>=1048576&&rect[1]>=1048576){uintptr_t id=*(uintptr_t*)(*(uintptr_t*)fp-0x98);unsigned kind=(id>>29)&3;if(kind)hg::queue_material_target(kind,unsigned(rect[0])-1048576,unsigned(rect[1])-1048576);}
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
struct ImageState { uintptr_t base=0;std::string path;hg::Resolved result;bool attempted=false; };
std::array<ImageState,3> images;
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
 std::vector<hg_patch>batch;unsigned ready=0;
 for(size_t i=0;i<images.size();i++){
  const auto&r=images[i].result;unsigned enabled=r.ready;
  if(!snapshots)enabled&=~2u;
  if(i==0&&(enabled&16)&&!widget_monochrome())enabled&=~16u;
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
 if(all_attempted&&!snapshots_pending&&(rc>=0||rc==-4))runtime_complete.store(true,std::memory_order_release);
 return rc>=0?ready:0;
}
extern "C" bool hg_runtime_complete(){
 return runtime_complete.load(std::memory_order_acquire);
}
