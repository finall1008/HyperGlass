#include "material.h"
#include "runtime.h"
#include <chrono>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>
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
 uintptr_t context;uint64_t view;std::array<int64_t,7>geometry;
 bool operator<(const GlassKey&o)const{return std::tie(context,view,geometry)<std::tie(o.context,o.view,o.geometry);}
};
struct SavedTexture {NativeRef texture;const void*slot;uint32_t stage,type;~SavedTexture(){released(texture);}};
using Clock=std::chrono::steady_clock;
struct HomeMaterial {
 NativeRef texture;
 uintptr_t generation=0;
 uint64_t stamp=0;
 uint32_t scene_epoch=0;
 Clock::time_point resting_since{},last_resting{};
 ~HomeMaterial(){released(texture);}
};
std::mutex material_mutex;
std::map<uintptr_t,HomeMaterial>materials;
uint64_t material_stamp=0;
std::atomic<float> display_density{0};
constexpr uint32_t scene_ready=1,workspace_scrolling=2,scene_flags=3;
// The upper bits version the scene and workspace state together.
std::atomic<uint32_t> home_scene_epoch{scene_ready};

struct MaterialTarget {
 unsigned kind=0,x=0,y=0;
 HgCaptureTransform transform;
};
constexpr uintptr_t view_mask=(uintptr_t{1}<<HG_CAPTURE_VIEW_BITS)-1;
constexpr unsigned ticket_mask=(1u<<HG_CAPTURE_TICKET_BITS)-1;
std::mutex targets_mutex;
std::map<unsigned,MaterialTarget>targets;
unsigned target_sequence=0;
std::optional<MaterialTarget> take_target(unsigned ticket,unsigned kind){
 std::lock_guard<std::mutex>lock(targets_mutex);
 auto item=targets.extract(ticket);
 if(item.empty()||item.mapped().kind!=kind)return {};
 return item.mapped();
}

struct CaptureInputs {
 std::map<unsigned,std::unique_ptr<SavedTexture>> textures;
 std::optional<GlassKey> target;
 uintptr_t generation=0;
 uintptr_t context=0;
};
std::mutex glass_mutex;
std::map<GlassKey,CaptureInputs> opening_inputs;
thread_local std::optional<MaterialTarget> export_target;
thread_local CaptureInputs* export_inputs=nullptr;
struct BackdropSelection {uintptr_t context,generation;};
thread_local std::optional<BackdropSelection> export_home;
thread_local unsigned export_kind=0;
thread_local NativeRef export_context;
thread_local uint32_t export_width=0,export_height=0;
struct MaterialDraw {
 const float*rect;
 bool selected,texture_capture;
 unsigned texture_index=0;
 std::optional<bool> resting;
};
thread_local MaterialDraw* active_draw=nullptr;
thread_local bool export_target_drawn=false;
// Field diagnostics for snapshot exports that cannot render the target as
// glass. Each message is limited to one line per second.
void report_export_fallback(unsigned kind,bool drawn,bool pinned){
 static std::atomic<int64_t> last_missing{0},last_unpinned{0};
 auto report=[](std::atomic<int64_t>&last){
  int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  int64_t previous=last.load(std::memory_order_relaxed);
  return now-previous>=1000&&last.compare_exchange_strong(previous,now,std::memory_order_relaxed);
 };
 if(!drawn){if(report(last_missing))LOG("Snapshot export stage=%u drew no glass plate at its target",kind);}
 else if(kind==2&&!pinned&&report(last_unpinned))LOG("Return snapshot used the live backdrop: no resting desktop material");
}
struct BlurCacheKey {
 alignas(8) uint8_t bytes[32]{};
};
static_assert(sizeof(BlurCacheKey)==32&&alignof(BlurCacheKey)==8);
BlurCacheKey normalize_blur_key(const void*raw){
 BlurCacheKey key;
 const auto*src=static_cast<const uint8_t*>(raw);
 memcpy(key.bytes,src,4);            // layer mode
 memcpy(key.bytes+8,src+8,8);        // backdrop ID
 memcpy(key.bytes+0x10,src+0x10,1);  // use PassBlur
 memcpy(key.bytes+0x14,src+0x14,8);  // horizontal and vertical radii
 return key;
}
struct ExportPin {
 bool active=false;
 uintptr_t context=0,state=0;
 NativeRef saved_texture{},saved_keeper{};
 uintptr_t saved_generation=0,saved_factory=0;
 std::vector<std::pair<uintptr_t,BlurCacheKey>> touched_cache_keys;
};
thread_local ExportPin* active_pin=nullptr;
using CacheTryGet=bool(*)(uintptr_t,const void*,const void*,void*,bool*);
using CacheTryGetTinted=bool(*)(uintptr_t,const void*,const void*,uint32_t,void*);
using CacheRemove=void(*)(uintptr_t,const void*);
CacheTryGet original_try_get=nullptr;
CacheTryGetTinted original_try_get_tinted=nullptr;
CacheRemove cache_remove=nullptr;

bool material_try_get(uintptr_t cache,const void*key,const void*rect,void*entry,bool*found){
 if(export_kind==2&&active_pin&&active_pin->active&&key){
  const bool target=active_draw&&active_draw->selected;
  if(target){
   if(found)*found=false;
  }else if(original_try_get(cache,key,rect,entry,found))return true;
  auto normalized=normalize_blur_key(key);
  auto&keys=active_pin->touched_cache_keys;
  auto same_key=[&](const auto&item){
   return item.first==cache&&!memcmp(item.second.bytes,normalized.bytes,sizeof(normalized.bytes));
  };
  if(std::none_of(keys.begin(),keys.end(),same_key))keys.emplace_back(cache,normalized);
  return false;
 }
 return original_try_get(cache,key,rect,entry,found);
}
bool material_try_get_tinted(uintptr_t cache,const void*key,const void*tint,uint32_t hash,void*out){
 if(export_kind==2&&active_pin&&active_pin->active&&active_draw&&active_draw->selected)return false;
 return original_try_get_tinted(cache,key,tint,hash,out);
}
void finish_export_home_pin(ExportPin&pin){
 if(!pin.active)return;
 // Flush the export's blur entries and their tinted textures before
 // returning the context to its live PassBlur state.
 if(cache_remove)for(const auto&[cache,key]:pin.touched_cache_keys)cache_remove(cache,key.bytes);
 std::swap(*(NativeRef*)pin.state,pin.saved_texture);
 std::swap(*(NativeRef*)(pin.state+0x70),pin.saved_keeper);
 *(uintptr_t*)(pin.state+0x18)=pin.saved_generation;
 *(uintptr_t*)(pin.state+0x60)=pin.saved_factory;
 pin.active=false;
 released(pin.saved_texture);
 released(pin.saved_keeper);
}

void freeze_material_inputs(CaptureInputs&inputs);
using Raster=void(*)(uintptr_t);Raster original_raster=nullptr;
void material_raster(uintptr_t closure){
 uintptr_t block=*(uintptr_t*)(closure+8),&view=*(uintptr_t*)(block+0x48);
 unsigned previous_kind=export_kind;
 auto previous_target=export_target;
 auto*previous_inputs=export_inputs;
 auto previous_home=export_home;
 auto*previous_pin=active_pin;
 NativeRef previous_context=export_context;uint32_t previous_width=export_width,previous_height=export_height;
 export_context={};export_home.reset();export_target.reset();export_inputs=nullptr;
 export_width=*(uint32_t*)(block+0x30);export_height=*(uint32_t*)(block+0x34);
 export_kind=(view>>28)&3;
 ExportPin pin;
 active_pin=export_kind==2?&pin:nullptr;
 CaptureInputs inputs;
 if(export_kind){
  export_target=take_target((view>>HG_CAPTURE_VIEW_BITS)&ticket_mask,export_kind);
  view&=view_mask;
 }
 if(export_target&&(export_kind==1||export_kind==2))export_inputs=&inputs;
 const bool previous_drawn=export_target_drawn;export_target_drawn=false;
 original_raster(closure);
 if(export_inputs)report_export_fallback(export_kind,export_target_drawn,pin.active);
 export_target_drawn=previous_drawn;
 finish_export_home_pin(pin);
 active_pin=previous_pin;
 if(export_kind==1&&export_inputs&&!inputs.textures.empty()){
  freeze_material_inputs(inputs);
  if(!inputs.textures.empty()){
   std::lock_guard<std::mutex>lock(glass_mutex);
   if(opening_inputs.size()>=8)opening_inputs.erase(opening_inputs.begin());
   auto key=*inputs.target;
   opening_inputs.insert_or_assign(key,std::move(inputs));
  }
 }
 released(export_context);
 export_context=previous_context;export_width=previous_width;export_height=previous_height;
 export_kind=previous_kind;export_target=previous_target;export_inputs=previous_inputs;export_home=previous_home;
}
using CreateTexture=void(*)(NativeRef*,uintptr_t,const void*,bool);CreateTexture create_texture=nullptr;
// impeller::Context allocator and command-buffer slots from the current engine.
uint32_t allocator_slot=0,command_slot=0;
using CreateBlit=void(*)(NativeRef*,uintptr_t);CreateBlit create_blit=nullptr;
using AddCopy=bool(*)(uintptr_t,NativeRef*,NativeRef*,const void*,int64_t,int64_t,const char*,size_t,uint32_t);AddCopy add_copy=nullptr;
using Enqueue=bool(*)(uintptr_t,NativeRef*);Enqueue enqueue=nullptr;
NativeRef clone_texture(uintptr_t context,NativeRef source,const int64_t*crop=nullptr){
 NativeRef copy{};if(!source.object||!create_texture||!create_blit||!add_copy||!enqueue)return copy;
 auto table=*(uintptr_t**)context;using Maker=ReturnRef(*)(uintptr_t);
 ReturnRef allocator=((Maker)table[allocator_slot/8])(context),command=((Maker)table[command_slot/8])(context);
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
void freeze_material_inputs(CaptureInputs&inputs){
 std::map<std::pair<uintptr_t,uintptr_t>,NativeRef>copies;
 // Render/cache producers have submitted their commands at this point.
 for(auto&entry:inputs.textures){
  auto&source=entry.second->texture;
  auto key=std::make_pair(source.object,source.owner);auto found=copies.find(key);
  NativeRef copy=found==copies.end()?clone_texture(inputs.target->context,source):retained(found->second);
  if(!copy.object){inputs.textures.clear();return;}
  copies[key]=copy;released(source);source=copy;
 }
}
uintptr_t blur_state(uintptr_t context){
 uintptr_t backend=*(uintptr_t*)(context+8);
 return backend?*(uintptr_t*)(backend+0x10):0;
}
void ensure_export_home_pinned(uintptr_t context){
 if(export_kind!=2||!active_pin||active_pin->active)return;
 auto&pin=*active_pin;
 uintptr_t generation=0;
 {
  std::lock_guard<std::mutex>lock(material_mutex);
  uintptr_t state=blur_state(context);
  auto item=materials.find(context);
  if(!state||item==materials.end()||!item->second.texture.object)return;
  auto&home=item->second;
  home.resting_since={};
  home.last_resting={};
  generation=home.generation;
  pin.saved_texture=retained(home.texture);
  pin.state=state;
 }
 export_home=BackdropSelection{context,generation};
 auto&live_generation=*(uintptr_t*)(pin.state+0x18);
 auto&factory=*(uintptr_t*)(pin.state+0x60);
 std::swap(*(NativeRef*)pin.state,pin.saved_texture);
 std::swap(*(NativeRef*)(pin.state+0x70),pin.saved_keeper);
 pin.saved_generation=std::exchange(live_generation,generation);
 pin.saved_factory=std::exchange(factory,0);
 pin.context=context;
 pin.active=true;
}
using Acquire=bool(*)(uintptr_t,const void*,const void*,uint64_t,uint64_t,float,void*,bool);Acquire original_acquire=nullptr;
bool material_acquire(uintptr_t canvas,const void*filter,const void*rect,uint64_t view,uint64_t has_view,float scale,void*result,bool flag){
 uintptr_t context=**(uintptr_t**)canvas;
 if(export_kind&&!export_context.object)export_context=retained(*(NativeRef*)*(uintptr_t*)canvas);
 ensure_export_home_pinned(context);
 bool ok=original_acquire(canvas,filter,rect,view,has_view,scale,result,flag);
 if(export_kind==1&&export_inputs){
  std::lock_guard<std::mutex>lock(material_mutex);
  uintptr_t state=blur_state(context);
  auto item=materials.find(context);
  uintptr_t generation=state?*(uintptr_t*)(state+0x18):0;
  if(state&&item!=materials.end()&&item->second.texture.object&&
     export_target->transform.scale==1&&generation==item->second.generation){
   export_inputs->context=context;
   export_inputs->generation=generation;
  }else{
   export_inputs->context=0;
   export_inputs->textures.clear();
  }
 }
 return ok;
}
void observe_home(uintptr_t context,bool resting,Clock::time_point now){
 const auto epoch=home_scene_epoch.load(std::memory_order_relaxed);
 if((epoch&scene_flags)!=scene_ready)return;
 uintptr_t state=blur_state(context);
 if(!state)return;
 std::lock_guard<std::mutex>lock(material_mutex);
 auto item=materials.find(context);
 if(item==materials.end()){
  if(!resting)return;
  if(materials.size()>=8){
   auto oldest=materials.begin();
   for(auto it=materials.begin();it!=materials.end();++it)if(it->second.stamp<oldest->second.stamp)oldest=it;
   materials.erase(oldest);
  }
  item=materials.try_emplace(context).first;
 }
 auto&home=item->second;home.stamp=++material_stamp;
 if(!resting){home.resting_since={};home.last_resting={};return;}
 constexpr auto settling=std::chrono::milliseconds(48);
 const bool settled=home.last_resting-home.resting_since>=settling;
 // A confirmed Home scene can be idle until a wallpaper buffer arrives.
 // Gaps still reset qualification for a scene that has yet to settle.
 if(home.scene_epoch!=epoch||home.resting_since==Clock::time_point{}||
    (!settled&&now-home.last_resting>std::chrono::milliseconds(100)))
  home.resting_since=now;
 home.scene_epoch=epoch;
 home.last_resting=now;
 // Isolated full-scale frames around the start of a return do not qualify.
 if(now-home.resting_since<settling)return;
 const auto&texture=*(const NativeRef*)state;
 uintptr_t generation=*(const uintptr_t*)(state+0x18);
 if(!texture.object||(home.texture.object&&home.generation==generation))return;
 NativeRef cloned=clone_texture(context,texture);
 if(cloned.object){
  if(home_scene_epoch.load(std::memory_order_relaxed)!=epoch){released(cloned);return;}
  released(home.texture);home.texture=cloned;home.generation=generation;
  std::lock_guard<std::mutex> lock(glass_mutex);
  for(auto it=opening_inputs.begin();it!=opening_inputs.end();){
   if(it->first.context==context&&it->second.generation!=generation)it=opening_inputs.erase(it);
   else ++it;
  }
 }
}
using DrawMaterial=bool(*)(uintptr_t,const void*,const void*,const void*,uint64_t,uint64_t,uint64_t,uint64_t,float);
DrawMaterial original_draw=nullptr;
using BindTexture=bool(*)(uintptr_t,uint32_t,uint32_t,const void*,const void*,NativeRef*,uintptr_t);BindTexture original_bind=nullptr;
bool material_bind(uintptr_t pass,uint32_t stage,uint32_t type,const void*slot,const void*metadata,NativeRef*texture,uintptr_t sampler){
 if(export_inputs&&active_draw&&active_draw->texture_capture&&texture->object){
  unsigned index=active_draw->texture_index++;
  if(export_kind==1&&export_inputs->context==export_inputs->target->context){
   auto saved=std::make_unique<SavedTexture>();
   saved->texture=retained(*texture);saved->slot=slot;saved->stage=stage;saved->type=type;
   export_inputs->textures[index]=std::move(saved);
  }else if(export_kind==2&&export_home&&export_home->context==export_inputs->context&&
           export_home->generation==export_inputs->generation){
   auto it=export_inputs->textures.find(index);
   if(it!=export_inputs->textures.end()){
    const auto&saved=*it->second;
    if(saved.slot==slot&&saved.stage==stage&&saved.type==type){
     released(*texture);*texture=retained(saved.texture);
    }
   }
  }
 }
 return original_bind(pass,stage,type,slot,metadata,texture,sampler);
}
using BufferContents=uint8_t*(*)(uintptr_t);BufferContents buffer_contents=nullptr;
using CopyHost=bool(*)(uintptr_t,const uint8_t*,size_t,size_t,size_t);CopyHost copy_host=nullptr;
using BindUniform=bool(*)(uintptr_t,uint64_t,uint32_t,void*);BindUniform original_uniform=nullptr;
bool resting_transform(const float*matrix,const float*rect,float density){
 auto equal=[](float a,float b){return std::abs(a-b)<0.001f;};
 return equal(matrix[0],density)&&equal(matrix[5],density)&&
        equal(matrix[1],0)&&equal(matrix[4],0)&&equal(matrix[15],1)&&
        std::abs(matrix[12]-rect[0])<1&&std::abs(matrix[13]-rect[1])<1;
}
void rest_screen_transform(float*matrix,const HgCaptureTransform&transform){
 // Left-multiply the inverse workspace transform. Retain all other axes
 // and the local transform, including fractional material origins.
 for(unsigned i=0;i<16;i+=4){
  matrix[i+3]/=transform.w;
  matrix[i]=(matrix[i]-transform.x*matrix[i+3])/transform.scale;
  matrix[i+1]=(matrix[i+1]-transform.y*matrix[i+3])/transform.scale;
 }
}
bool material_uniform(uintptr_t pass,uint64_t binding,uint32_t type,void*view){
 if(active_draw&&binding==0&&(!export_kind||active_draw->selected)){
  const auto*words=(const uintptr_t*)view;
  uintptr_t buffer=words[2]?words[2]:words[0];
  // FrameInfo is the vertex binding. Its second matrix is the display
  // sampling transform, independent of the geometry transform before it.
  if(buffer&&words[4]==192){
   auto data=buffer_contents(buffer);
   if(data){
    float matrix[16];memcpy(matrix,data+words[3]+64,sizeof(matrix));
    if(!export_kind){
     float density=display_density.load(std::memory_order_relaxed);
     if(density>0&&std::abs(matrix[12]-active_draw->rect[0])<1&&std::abs(matrix[13]-active_draw->rect[1])<1)
      active_draw->resting=resting_transform(matrix,active_draw->rect,density);
    }else if(export_target&&export_target->transform.scale!=1&&
             !resting_transform(matrix,active_draw->rect,export_target->transform.density)){
     rest_screen_transform(matrix,export_target->transform);
     copy_host(buffer,reinterpret_cast<const uint8_t*>(matrix),0,sizeof(matrix),words[3]+64);
    }
   }
  }
 }
 return original_uniform(pass,binding,type,view);
}
bool material_draw(uintptr_t canvas,const void*filter,const void*rect,const void*clip,uint64_t view,uint64_t has_view,uint64_t backdrop,uint64_t has_backdrop,float alpha){
 uintptr_t context=**(uintptr_t**)canvas;
 if(export_kind&&!export_context.object)export_context=retained(*(NativeRef*)*(uintptr_t*)canvas);
 ensure_export_home_pinned(context);
 const auto*r=(const float*)rect;
 const bool target_draw=export_target&&(export_kind==1||export_kind==2)&&
                     std::abs(r[0]-export_target->x)<=1&&std::abs(r[1]-export_target->y)<=1;
 bool texture_capture=false;
 if(target_draw&&export_inputs&&!export_inputs->target){
  auto quantize=[](float v){return int64_t(std::llround(double(v)*16));};
  GlassKey key{context,(has_view&1)?view:0,{quantize(r[0]),quantize(r[1]),quantize(r[2]),quantize(r[3]),(has_backdrop&1)?int64_t(backdrop):0,int64_t(has_backdrop&1),int64_t(has_view&1)}};
  std::lock_guard<std::mutex>lock(glass_mutex);
  auto saved=opening_inputs.extract(key);
  if(export_kind==2&&!saved.empty())*export_inputs=std::move(saved.mapped());
  export_inputs->target=key;texture_capture=true;
 }
 export_target_drawn|=target_draw;
 MaterialDraw draw{r,target_draw,texture_capture,0,{}};
 auto*previous=active_draw;active_draw=&draw;
 bool ok=original_draw(canvas,filter,rect,clip,view,has_view,backdrop,has_backdrop,alpha);
 active_draw=previous;
 if(!export_kind&&ok&&draw.resting)observe_home(context,*draw.resting,Clock::now());
 return ok;
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
void set_material_density(float density){
 display_density.store(density,std::memory_order_relaxed);
}
static void update_scene_flag(uint32_t flag,bool enabled){
 auto epoch=home_scene_epoch.load(std::memory_order_relaxed);
 while(bool(epoch&flag)!=enabled&&!home_scene_epoch.compare_exchange_weak(
       epoch,(epoch+(scene_flags+1))^flag,std::memory_order_relaxed)){}
}
void set_home_material_ready(bool ready){
 update_scene_flag(scene_ready,ready);
}
void set_workspace_scrolling(bool scrolling){
 update_scene_flag(workspace_scrolling,scrolling);
}
unsigned queue_material_target(unsigned kind,unsigned x,unsigned y,const HgCaptureTransform&transform){
 std::lock_guard<std::mutex>lock(targets_mutex);
 // A cancelled image cannot misroute the next capture, even in the same stage.
 if(targets.size()>=64){
  auto oldest=targets.upper_bound(target_sequence);
  targets.erase(oldest==targets.end()?targets.begin():oldest);
 }
 target_sequence=target_sequence%ticket_mask+1;
 targets.insert_or_assign(target_sequence,MaterialTarget{kind,x,y,transform});
 if(kind!=3)set_material_density(transform.density);
 return target_sequence;
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
 if(original_raster&&original_draw&&original_bind&&original_uniform&&original_acquire&&original_image_ready&&
    original_try_get&&original_try_get_tinted&&cache_remove)return true;
 if(!(result.ready&2))return false;
 create_texture=(CreateTexture)entry(program,result,base,".material-create-texture");
 create_blit=(CreateBlit)entry(program,result,base,".material-create-blit");
 add_copy=(AddCopy)entry(program,result,base,".material-add-copy");
 enqueue=(Enqueue)entry(program,result,base,".material-enqueue");
 allocator_slot=result.virtual_slots[0];command_slot=result.virtual_slots[1];
 original_make_image=(MakeImage)entry(program,result,base,".material-make-image");
 buffer_contents=(BufferContents)entry(program,result,base,".material-buffer-contents");
 copy_host=(CopyHost)entry(program,result,base,".material-copy-host");
 cache_remove=(CacheRemove)entry(program,result,base,".material-cache-remove");
 if(!create_texture||!create_blit||!add_copy||!enqueue||!allocator_slot||!command_slot||!original_make_image||!buffer_contents||!copy_host||!cache_remove)return false;
 const HookSpec specs[]={
  {entry(program,result,base,".material-raster"),(void*)material_raster,(void**)&original_raster},
  {entry(program,result,base,".material-draw"),(void*)material_draw,(void**)&original_draw},
  {entry(program,result,base,".material-bind"),(void*)material_bind,(void**)&original_bind},
  {entry(program,result,base,".material-uniform"),(void*)material_uniform,(void**)&original_uniform},
  {entry(program,result,base,".material-acquire"),(void*)material_acquire,(void**)&original_acquire},
  {entry(program,result,base,".material-image-ready"),(void*)material_image_ready,(void**)&original_image_ready},
  {entry(program,result,base,".material-cache-try-get"),(void*)material_try_get,(void**)&original_try_get},
  {entry(program,result,base,".material-cache-try-get-tinted"),(void*)material_try_get_tinted,(void**)&original_try_get_tinted},
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
