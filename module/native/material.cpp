#include "material.h"
#include "runtime.h"
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <deque>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"HyperGlass",__VA_ARGS__)
namespace hg {
namespace {
/* The engine's libc++ control blocks are retained through their own virtual
 * release methods. Semantic ABI constraints cover these fields and slots. */
struct NativeRef {uintptr_t object=0,owner=0;};
NativeRef retained(NativeRef p){if(p.owner)__atomic_fetch_add((int64_t*)(p.owner+8),1,__ATOMIC_RELAXED);return p;}
void released(NativeRef p){if(p.owner&&__atomic_fetch_sub((int64_t*)(p.owner+8),1,__ATOMIC_ACQ_REL)==0){auto table=*(uintptr_t**)p.owner;((void(*)(uintptr_t))table[2])(p.owner);if(__atomic_fetch_sub((int64_t*)(p.owner+16),1,__ATOMIC_ACQ_REL)==0)((void(*)(uintptr_t))table[4])(p.owner);}}
struct ReturnRef {NativeRef value;~ReturnRef(){released(value);}};
NativeRef clone_texture(uintptr_t,NativeRef,const int64_t*);
struct GlassKey {
 uintptr_t context;uint64_t view;std::array<int64_t,8>geometry;
 bool operator<(const GlassKey&o)const{return std::tie(context,view,geometry)<std::tie(o.context,o.view,o.geometry);}
};
struct SavedTexture {NativeRef texture;const void*slot;uint32_t stage,type;~SavedTexture(){released(texture);}};
std::mutex glass_mutex;std::map<GlassKey,std::shared_ptr<SavedTexture>>opening_textures;
struct SavedUniform {uint64_t binding;uint32_t type;std::vector<uint8_t>bytes;};
std::map<GlassKey,SavedUniform>opening_uniforms;
thread_local const GlassKey*active_draw=nullptr;thread_local unsigned uniform_index=0,texture_index=0;
void begin_glass_export(unsigned kind){if(kind==1){std::lock_guard<std::mutex>lock(glass_mutex);opening_uniforms.clear();opening_textures.clear();}}
void freeze_material_inputs();
struct HomeMaterial {
 NativeRef texture,keeper;uintptr_t generation=0;uint64_t stamp=0;bool locked=false;
 ~HomeMaterial(){released(texture);released(keeper);}
};
std::mutex material_mutex;std::map<uintptr_t,HomeMaterial>materials;thread_local unsigned export_kind=0;uint64_t material_stamp=0;
struct Target {unsigned kind,x,y;};std::mutex targets_mutex;std::deque<Target>targets;
thread_local unsigned target_x=0,target_y=0;thread_local bool has_target=false;
thread_local NativeRef export_context;thread_local uint32_t export_width=0,export_height=0;
using Raster=void(*)(uintptr_t);Raster original_raster=nullptr;
void material_raster(uintptr_t closure){
 uintptr_t block=*(uintptr_t*)(closure+8),&view=*(uintptr_t*)(block+0x48);unsigned previous=export_kind;
 NativeRef previous_context=export_context;uint32_t previous_width=export_width,previous_height=export_height;export_context={};
 unsigned previous_x=target_x,previous_y=target_y;bool previous_target=has_target;
 export_width=*(uint32_t*)(block+0x30);export_height=*(uint32_t*)(block+0x34);
 export_kind=(view>>28)&3;
 has_target=false;if(export_kind){std::lock_guard<std::mutex>lock(targets_mutex);if(!targets.empty()){auto target=targets.front();targets.pop_front();if(target.kind==export_kind){target_x=target.x;target_y=target.y;has_target=true;}}}
 view&=~uintptr_t((1u<<29)|(1u<<28));
 begin_glass_export(export_kind);
 if(export_kind==1){std::lock_guard<std::mutex>lock(material_mutex);for(auto&entry:materials)entry.second.locked=true;}
 original_raster(closure);
 if(export_kind==1)freeze_material_inputs();
 if(export_kind==2){std::lock_guard<std::mutex>lock(material_mutex);for(auto&entry:materials)entry.second.locked=false;}
 released(export_context);export_context=previous_context;export_width=previous_width;export_height=previous_height;export_kind=previous;target_x=previous_x;target_y=previous_y;has_target=previous_target;
}
using CreateTexture=void(*)(NativeRef*,uintptr_t,const void*,bool);CreateTexture create_texture=nullptr;
using CreateBlit=void(*)(NativeRef*,uintptr_t);CreateBlit create_blit=nullptr;
using AddCopy=bool(*)(uintptr_t,NativeRef*,NativeRef*,const void*,int64_t,int64_t,const char*,size_t,uint32_t);AddCopy add_copy=nullptr;
using Enqueue=bool(*)(uintptr_t,NativeRef*);Enqueue enqueue=nullptr;
NativeRef clone_texture(uintptr_t context,NativeRef source,const int64_t*crop=nullptr){
 NativeRef copy{};if(!source.object||!create_texture||!create_blit||!add_copy||!enqueue)return copy;
 auto table=*(uintptr_t**)context;using Maker=ReturnRef(*)(uintptr_t);
 ReturnRef allocator=((Maker)table[0xb8/8])(context),command=((Maker)table[0xe0/8])(context);
 if(!allocator.value.object||!command.value.object)return copy;
 alignas(8) uint8_t descriptor[48];memcpy(descriptor,(void*)(source.object+16),sizeof(descriptor));
 if(crop){int64_t size[]={crop[2]-crop[0],crop[3]-crop[1]};memcpy(descriptor+16,size,sizeof(size));}
 create_texture(&copy,allocator.value.object,descriptor,false);if(!copy.object)return copy;
 NativeRef blit{};create_blit(&blit,command.value.object);if(!blit.object){released(copy);return {};}
 alignas(8) uint8_t region[40]{};NativeRef src=retained(source),dst=retained(copy);
 if(crop){memcpy(region,crop,32);region[32]=1;}
 constexpr char label[]="HyperGlass backdrop";
 bool ok=add_copy(blit.object,&src,&dst,region,0,0,label,sizeof(label)-1,0);released(src);released(dst);
 auto pass=*(uintptr_t**)blit.object;
 if(ok)ok=((bool(*)(uintptr_t,const NativeRef*))pass[0x18/8])(blit.object,&copy);
 if(ok)ok=((bool(*)(uintptr_t))pass[0x28/8])(blit.object);
 if(ok)ok=enqueue(context,&command.value);
 released(blit);
 if(!ok){released(copy);return {};}
 return copy;
}
void freeze_material_inputs(){
 std::lock_guard<std::mutex>lock(glass_mutex);std::map<std::pair<uintptr_t,uintptr_t>,NativeRef>copies;bool ok=true;
 // Render/cache producers have submitted their commands at this point. A
 // separate copy inside their active pass can run ahead of the producer.
 for(auto&entry:opening_textures){auto&source=entry.second->texture;auto key=std::make_pair(source.object,source.owner);auto found=copies.find(key);
  NativeRef copy=found==copies.end()?clone_texture(entry.first.context,source):retained(found->second);
  if(!copy.object){ok=false;break;}copies[key]=copy;released(source);source=copy;
 }
 if(!ok){opening_textures.clear();opening_uniforms.clear();}
}
using Acquire=bool(*)(uintptr_t,const void*,const void*,uint64_t,uint64_t,float,void*,bool);Acquire original_acquire=nullptr;
bool material_acquire(uintptr_t canvas,const void*filter,const void*rect,uint64_t view,uint64_t has_view,float scale,void*result,bool flag){
 std::lock_guard<std::mutex>lock(material_mutex);uintptr_t context=**(uintptr_t**)canvas,backend=*(uintptr_t*)(context+8),state=backend?*(uintptr_t*)(backend+0x10):0;
 if(export_kind&&!export_context.object)export_context=retained(*(NativeRef*)*(uintptr_t*)canvas);
 if(!state)return original_acquire(canvas,filter,rect,view,has_view,scale,result,flag);
 if(materials.find(context)==materials.end()&&materials.size()>=8){auto oldest=materials.begin();for(auto it=materials.begin();it!=materials.end();++it)if(it->second.stamp<oldest->second.stamp)oldest=it;materials.erase(oldest);}
 auto&home=materials[context];home.stamp=++material_stamp;auto&texture=*(NativeRef*)state;auto&keeper=*(NativeRef*)(state+0x70);auto&generation=*(uintptr_t*)(state+0x18);auto&factory=*(uintptr_t*)(state+0x60);
 if(export_kind==1)home.locked=true;
 bool pinned=export_kind==2&&home.texture.object;uintptr_t saved_generation=generation,saved_factory=factory;
 if(pinned){std::swap(texture,home.texture);std::swap(keeper,home.keeper);generation=home.generation;factory=0;}
 bool ok=original_acquire(canvas,filter,rect,view,has_view,scale,result,flag);
 if(pinned){std::swap(texture,home.texture);std::swap(keeper,home.keeper);generation=saved_generation;factory=saved_factory;}
 // Keep the last displayed home frame. Opening exports can consume the
 // transition's next AHB even when their visible cache still shows home.
 if(!pinned&&texture.object&&(!home.texture.object||(!export_kind&&!home.locked&&home.generation!=generation))){
  NativeRef cloned=clone_texture(context,texture);if(cloned.object){released(home.texture);released(home.keeper);home.texture=cloned;home.keeper={};home.generation=generation;}
 }
 return ok;
}
using DrawMaterial=bool(*)(uintptr_t,const void*,const void*,const void*,uint64_t,uint64_t,uint64_t,uint64_t,float);
DrawMaterial original_draw=nullptr;
using BindTexture=bool(*)(uintptr_t,uint32_t,uint32_t,const void*,const void*,NativeRef*,uintptr_t);BindTexture original_bind=nullptr;
bool material_bind(uintptr_t pass,uint32_t stage,uint32_t type,const void*slot,const void*metadata,NativeRef*texture,uintptr_t sampler){
 std::shared_ptr<SavedTexture>saved;
 if((export_kind==1||export_kind==2)&&active_draw&&texture->object){GlassKey key=*active_draw;key.geometry[7]=texture_index++;
  // Retain the producer through submission; freeze it after the raster task.
  if(export_kind==1){auto copy=retained(*texture);if(copy.object){saved=std::make_shared<SavedTexture>();saved->texture=copy;saved->slot=slot;saved->stage=stage;saved->type=type;std::lock_guard<std::mutex>lock(glass_mutex);opening_textures[key]=saved;}}
  else{std::lock_guard<std::mutex>lock(glass_mutex);auto at=opening_textures.find(key);if(at!=opening_textures.end())saved=at->second;}
  if(export_kind==2&&saved&&saved->slot==slot&&saved->stage==stage&&saved->type==type){released(*texture);*texture=retained(saved->texture);}
 }
 return original_bind(pass,stage,type,slot,metadata,texture,sampler);
}
using BufferContents=uint8_t*(*)(uintptr_t);BufferContents buffer_contents=nullptr;
using CopyHost=bool(*)(uintptr_t,const uint8_t*,size_t,size_t,size_t);CopyHost copy_host=nullptr;
using BindUniform=bool(*)(uintptr_t,uint64_t,uint32_t,void*);BindUniform original_uniform=nullptr;
bool material_uniform(uintptr_t pass,uint64_t binding,uint32_t type,void*view){
 if((export_kind==1||export_kind==2)&&active_draw&&binding==0){auto words=(uintptr_t*)view;uintptr_t buffer=words[2]?words[2]:words[0];size_t length=words[4];
  if(buffer&&length>=128&&length<=65536){GlassKey key=*active_draw;key.geometry[7]=uniform_index++;auto data=buffer_contents(buffer);
   if(data){data+=words[3]+64;std::lock_guard<std::mutex>lock(glass_mutex);
    if(export_kind==1)opening_uniforms[key]={binding,type,std::vector<uint8_t>(data,data+64)};
    else{auto at=opening_uniforms.find(key);if(at!=opening_uniforms.end()&&at->second.binding==binding&&at->second.type==type&&at->second.bytes.size()==64)copy_host(buffer,at->second.bytes.data(),0,64,words[3]+64);}
   }
  }
 }
 return original_uniform(pass,binding,type,view);
}
bool material_draw(uintptr_t canvas,const void*filter,const void*rect,const void*clip,uint64_t view,uint64_t has_view,uint64_t backdrop,uint64_t has_backdrop,float alpha){
 uintptr_t context=**(uintptr_t**)canvas;
 // In-pass glass on folders/other desktop pages does not call AcquireGlass.
 // Its context must also reach the GPU crop before any byte readback.
 if(export_kind&&!export_context.object)export_context=retained(*(NativeRef*)*(uintptr_t*)canvas);
 GlassKey draw_key{};
 if(export_kind){const float*r=(const float*)rect;auto quantize=[](float v){return int64_t(std::llround(double(v)*16));};
  draw_key={context,(has_view&1)?view:0,{quantize(r[0]),quantize(r[1]),quantize(r[2]),quantize(r[3]),(has_backdrop&1)?int64_t(backdrop):0,int64_t(has_backdrop&1),int64_t(has_view&1),0}};
 }
 const GlassKey*previous=active_draw;unsigned previous_uniform=uniform_index,previous_texture=texture_index;active_draw=export_kind&&has_target&&std::abs(draw_key.geometry[0]-int64_t(target_x)*16)<=16&&std::abs(draw_key.geometry[1]-int64_t(target_y)*16)<=16?&draw_key:nullptr;uniform_index=texture_index=0;
 bool ok=original_draw(canvas,filter,rect,clip,view,has_view,backdrop,has_backdrop,alpha);active_draw=previous;uniform_index=previous_uniform;texture_index=previous_texture;return ok;
}

void retain_image(uintptr_t p){if(p)__atomic_fetch_add((uint32_t*)(p+8),1,__ATOMIC_RELAXED);}
void release_image(uintptr_t p){if(p&&__atomic_fetch_sub((uint32_t*)(p+8),1,__ATOMIC_ACQ_REL)==1)((void(*)(uintptr_t))(*(uintptr_t**)p)[2])(p);}
struct CapturedImage {
 uintptr_t image;NativeRef context;
 ~CapturedImage(){release_image(image);released(context);}
};
std::mutex image_mutex;std::map<uintptr_t,std::unique_ptr<CapturedImage>>captured_images;
using MakeImage=void(*)(uintptr_t*,NativeRef*,unsigned);MakeImage original_make_image=nullptr;
using ImageReady=void(*)(uintptr_t,const uintptr_t*);ImageReady original_image_ready=nullptr;
void material_image_ready(uintptr_t closure,const uintptr_t*result){
 if(export_kind&&export_context.object&&*result){
  uintptr_t source=*(uintptr_t*)(*result+0x10);
  if(source&&*(int64_t*)(source+0x20)==export_width&&*(int64_t*)(source+0x28)==export_height){
   auto item=std::make_unique<CapturedImage>();item->image=*result;retain_image(item->image);item->context=retained(export_context);
   std::lock_guard<std::mutex>lock(image_mutex);if(captured_images.size()>=8)captured_images.erase(captured_images.begin());captured_images[*result]=std::move(item);
  }
 }
 original_image_ready(closure,result);
}

using StreamCallback=void(*)(void*,uintptr_t);
using InitBlur=void(*)(uintptr_t,void*,StreamCallback,int,float,int,int,int,const char*const*,int);
using Dequeue=AHardwareBuffer*(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
InitBlur original_init_excluded=nullptr,original_init_orientation=nullptr;
Dequeue original_dequeue=nullptr;
struct Stream {StreamCallback callback=nullptr;uintptr_t texture=0;};
std::mutex stream_mutex;std::map<uintptr_t,Stream>streams;std::map<uintptr_t,bool>priming;
void stream_callback(void*texture,uintptr_t key){
 StreamCallback callback=nullptr;
 {std::lock_guard<std::mutex>lock(stream_mutex);auto it=streams.find(key);if(it==streams.end())return;
  auto&s=it->second;callback=s.callback;uintptr_t next=(uintptr_t)texture;
  if(s.texture!=next){priming.erase(s.texture);s.texture=next;if(next)priming[next]=true;}
 }
 if(callback)callback(texture,key);
}
void start_stream(uintptr_t key,StreamCallback callback){
 std::lock_guard<std::mutex>lock(stream_mutex);auto&s=streams[key];priming.erase(s.texture);s={callback,0};
}
void init_excluded(uintptr_t key,void*surface,StreamCallback callback,int display,float rate,int a,int b,int orientation,const char*const*names,int count){
 start_stream(key,callback);original_init_excluded(key,surface,stream_callback,display,rate,a,b,orientation,names,count);
}
void init_orientation(uintptr_t key,void*surface,StreamCallback callback,int display,float rate,int a,int b,int orientation,const char*const*names,int count){
 start_stream(key,callback);original_init_orientation(key,surface,stream_callback,display,rate,a,b,orientation,names,count);
}
AHardwareBuffer*dequeue(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j,uintptr_t k){
 AHardwareBuffer*buffer=original_dequeue(a,b,c,d,e,f,g,h,i,j,k);bool discard=false;
 if(buffer){std::lock_guard<std::mutex>lock(stream_mutex);auto it=priming.find(a);if(it!=priming.end()&&it->second){it->second=false;discard=true;}}
 if(discard){AHardwareBuffer_release(buffer);return nullptr;}
 return buffer;
}
struct HookSpec {uintptr_t target;void*replace;void**original;};
bool install_hook_group(const HookSpec*specs,size_t count){
 std::vector<size_t>installed;
 for(size_t i=0;i<count;i++){
  if(*specs[i].original)continue;
  if(!specs[i].target||hg_install_native_hook((void*)specs[i].target,specs[i].replace,specs[i].original)!=0||!*specs[i].original){
   for(size_t j=installed.size();j>0;--j){size_t k=installed[j-1];hg_remove_native_hook((void*)specs[k].target);*specs[k].original=nullptr;}
   return false;
  }
  installed.push_back(i);
 }
 return true;
}
uintptr_t entry(const Program&program,const Resolved&result,uintptr_t base,const char*label){
 for(size_t i=0;i<program.unit_count;i++)if(!strcmp(program.units[i].label,label)){
  int index=program.units[i].anchor;return index>=0&&result.addresses[size_t(index)]?base+result.addresses[size_t(index)]:0;
 }
 return 0;
}
}
void queue_material_target(unsigned kind,unsigned x,unsigned y){
 std::lock_guard<std::mutex>lock(targets_mutex);if(targets.size()>=64)targets.pop_front();targets.push_back({kind,x,y});
}
uintptr_t image_peer(uintptr_t image,uintptr_t heap){
 uintptr_t wrapper=heap+*(uint32_t*)(image+7),fields=heap+*(uint32_t*)(wrapper+7),storage=*(uintptr_t*)(fields+7);
 return storage?*(uintptr_t*)storage:0;
}
bool crop_export_image(uintptr_t peer,unsigned x,unsigned y,unsigned width,unsigned height){
 if(!peer||!width||!height)return false;
 uintptr_t&image=*(uintptr_t*)(peer+0x30);std::unique_ptr<CapturedImage>capture;
 {std::lock_guard<std::mutex>lock(image_mutex);auto it=captured_images.find(image);if(it==captured_images.end())return false;capture=std::move(it->second);captured_images.erase(it);}
 NativeRef texture=*(NativeRef*)(image+0x10);if(!texture.object)return false;
 int64_t fw=*(int64_t*)(texture.object+0x20),fh=*(int64_t*)(texture.object+0x28);
 if(x+uint64_t(width)>uint64_t(fw)||y+uint64_t(height)>uint64_t(fh))return false;
 int64_t rect[]={int64_t(x),int64_t(y),int64_t(x)+width,int64_t(y)+height};NativeRef copy=clone_texture(capture->context.object,texture,rect);
 if(!copy.object)return false;
 uintptr_t replacement=0;original_make_image(&replacement,&copy,*(uint32_t*)(image+0x20));released(copy);if(!replacement)return false;
 uintptr_t previous=image;image=replacement;release_image(previous);
 return true;
}
bool install_material_hooks(uintptr_t base,const Program&program,const Resolved&result){
 if(original_raster&&original_draw&&original_bind&&original_uniform&&original_acquire&&original_image_ready)return true;
 if(!(result.ready&2))return false;
 create_texture=(CreateTexture)entry(program,result,base,".material-create-texture");
 create_blit=(CreateBlit)entry(program,result,base,".material-create-blit");
 add_copy=(AddCopy)entry(program,result,base,".material-add-copy");
 enqueue=(Enqueue)entry(program,result,base,".material-enqueue");
 original_make_image=(MakeImage)entry(program,result,base,".material-make-image");
 buffer_contents=(BufferContents)entry(program,result,base,".material-buffer-contents");
 copy_host=(CopyHost)entry(program,result,base,".material-copy-host");
 if(!create_texture||!create_blit||!add_copy||!enqueue||!original_make_image||!buffer_contents||!copy_host)return false;
 const HookSpec specs[]={
  {entry(program,result,base,".material-raster"),(void*)material_raster,(void**)&original_raster},
  {entry(program,result,base,".material-draw"),(void*)material_draw,(void**)&original_draw},
  {entry(program,result,base,".material-bind"),(void*)material_bind,(void**)&original_bind},
  {entry(program,result,base,".material-uniform"),(void*)material_uniform,(void**)&original_uniform},
  {entry(program,result,base,".material-acquire"),(void*)material_acquire,(void**)&original_acquire},
  {entry(program,result,base,".material-image-ready"),(void*)material_image_ready,(void**)&original_image_ready},
 };
 bool ok=install_hook_group(specs,sizeof(specs)/sizeof(specs[0]));
 static bool reported=false;if(ok&&!reported){reported=true;LOG("Native material capture ready: independent GPU backdrop, live foreground");}
 return ok;
}
bool install_backdrop_stream_hooks(){
 if(original_dequeue&&original_init_excluded&&original_init_orientation)return true;
 FILE*f=fopen("/proc/self/maps","re");if(!f)return false;
 char line[1536],path[1024],permission[8];unsigned long begin,end,offset;uintptr_t base=0;std::string file;std::vector<std::pair<uintptr_t,uintptr_t>>executable;
 while(fgets(line,sizeof(line),f))if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %1023s",&begin,&end,permission,&offset,path)==5){
  const char*leaf=strrchr(path,'/');if(!leaf||strcmp(leaf+1,"libhyper_surface_context.so"))continue;
  if(offset==0){base=begin;file=path;}if(permission[2]=='x')executable.push_back({begin,end});
 }
 fclose(f);if(!base)return false;ElfImage image;std::string error;if(!image.load(file.c_str(),error))return false;
 auto symbol=[&](const char*name){auto found=image.named(name);uintptr_t target=0;for(const auto&s:found){uintptr_t value=base+s.value;bool mapped=false;for(const auto&m:executable)if(m.first<=value&&value+16<=m.second)mapped=true;if(!mapped)continue;if(target&&target!=value)return uintptr_t(0);target=value;}return target;};
 uintptr_t buffer=symbol("HyperSurfaceTexture_dequeueHardwareBuffer"),excluded=symbol("HyperSurfaceContext_initPassBlurWithExcluded"),orientation=symbol("HyperSurfaceContext_initPassBlurWithDisplayOrientation");
 if(!buffer||!excluded||!orientation)return false;
 const HookSpec specs[]={
  {buffer,(void*)dequeue,(void**)&original_dequeue},
  {excluded,(void*)init_excluded,(void**)&original_init_excluded},
  {orientation,(void*)init_orientation,(void**)&original_init_orientation},
 };
 bool ok=install_hook_group(specs,sizeof(specs)/sizeof(specs[0]));
 static bool reported=false;if(ok&&!reported){reported=true;LOG("Backdrop stream hooks ready: per-stream startup buffer filtering");}
 return ok;
}
}
