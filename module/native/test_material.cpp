#include "material.cpp"
#include <cassert>
#include <memory>
#include <vector>
#include <string>
struct Resource {
 alignas(8) uint8_t object[80]{};
 struct Control {uintptr_t*table;int64_t owners=0,weak=0;Resource*resource;};
 Control control;bool destroyed=false,freed=false;uint32_t color=0;std::string kind;
};
static uintptr_t control_table[5]{};
static std::vector<std::unique_ptr<Resource>>resources;
static void destroy_resource(uintptr_t p){auto*c=(Resource::Control*)p;assert(!c->resource->destroyed);c->resource->destroyed=true;}
static void free_control(uintptr_t p){auto*c=(Resource::Control*)p;assert(c->resource->destroyed&&!c->resource->freed);c->resource->freed=true;}
static hg::NativeRef make_resource(const char*kind,uintptr_t*table=nullptr){
 auto r=std::make_unique<Resource>();r->kind=kind;r->control={control_table,0,0,r.get()};memcpy(r->object,&table,8);
 hg::NativeRef ref{(uintptr_t)r->object,(uintptr_t)&r->control};resources.push_back(std::move(r));return ref;
}
static Resource&resource(hg::NativeRef r){return *((Resource::Control*)r.owner)->resource;}
static hg::NativeRef allocator_ref,pending_source,pending_copy;
static uintptr_t context_table[32]{},blit_table[11]{};
static bool allocation_failure=false;static unsigned copies=0,phase=0;
static int64_t copied_rect[4]{};
static hg::ReturnRef allocator_maker(uintptr_t){return {hg::retained(allocator_ref)};}
static hg::ReturnRef command_maker(uintptr_t){return {make_resource("command")};}
static void texture_maker(hg::NativeRef*out,uintptr_t,const void*descriptor,bool extra){
 assert(!extra);if(allocation_failure){*out={};return;}*out=make_resource("copy");memcpy((void*)(out->object+16),descriptor,48);
}
static void blit_maker(hg::NativeRef*out,uintptr_t){*out=make_resource("blit",blit_table);phase=0;}
static bool add_copy(uintptr_t,hg::NativeRef*src,hg::NativeRef*dst,const void*rect,int64_t x,int64_t y,const char*label,size_t length,uint32_t slice){
 assert(!x&&!y&&!slice&&std::string(label,length)=="HyperGlass backdrop");
 if(((const uint8_t*)rect)[32]){
  memcpy(copied_rect,rect,32);assert(*(int64_t*)(dst->object+0x20)==copied_rect[2]-copied_rect[0]&&*(int64_t*)(dst->object+0x28)==copied_rect[3]-copied_rect[1]);
 }else assert(!memcmp((void*)(src->object+16),(void*)(dst->object+16),48));
 pending_source=*src;pending_copy=*dst;*src={};*dst={};phase=1;return true;
}
static bool shader_read(uintptr_t,const hg::NativeRef*dst){assert(phase==1&&dst->object==pending_copy.object);phase=2;return true;}
static bool encode(uintptr_t){assert(phase==2);phase=3;return true;}
static bool enqueue(uintptr_t,hg::NativeRef*command){
 assert(phase==3);resource(pending_copy).color=resource(pending_source).color;copies++;
 hg::released(pending_source);hg::released(pending_copy);pending_source={};pending_copy={};hg::released(*command);*command={};return true;
}
alignas(8) static uint8_t state[128]{};
static uint32_t observed_color=0;static uintptr_t observed_generation=0,observed_factory=0;
static bool consume_pending=false;
static bool acquire(uintptr_t,const void*,const void*,uint64_t,uint64_t,float,void*,bool){
 if(consume_pending){resource(*(hg::NativeRef*)state).color=0xff123456;(*(uintptr_t*)(state+0x18))++;consume_pending=false;}
 auto texture=*(hg::NativeRef*)state;observed_color=texture.object?resource(texture).color:0;
 observed_generation=*(uintptr_t*)(state+0x18);observed_factory=*(uintptr_t*)(state+0x60);return texture.object!=0;
}
static uintptr_t fake_context[2]{},backend[4]{},content[2]{},canvas[1]{};
struct FakeImage {uintptr_t*table;uint32_t count=1,padding=0;hg::NativeRef texture;uint32_t owning;bool disposed=false;};
static uintptr_t image_table[3]{};static std::vector<std::unique_ptr<FakeImage>>images;
static void dispose_image(uintptr_t p){auto&im=*(FakeImage*)p;assert(!im.disposed&&im.count==0);im.disposed=true;hg::released(im.texture);im.texture={};}
static void make_image(uintptr_t*out,hg::NativeRef*texture,unsigned owning){auto im=std::make_unique<FakeImage>();im->table=image_table;im->texture=*texture;im->owning=owning;*texture={};*out=(uintptr_t)im.get();images.push_back(std::move(im));}
static void image_ready(uintptr_t,const uintptr_t*){}
static bool cached_raster=false;
static void (*raster_draw)()=nullptr;
static void raster(uintptr_t closure){
 uintptr_t block=*(uintptr_t*)(closure+8);assert((*(uintptr_t*)(block+0x48)&((1u<<29)|(1u<<28)))==0);
 if(!cached_raster)hg::material_acquire((uintptr_t)canvas,nullptr,nullptr,1,1,1,nullptr,false);
 if(raster_draw)raster_draw();
}
static void export_frame(unsigned kind,unsigned x=69,unsigned y=802,float scale=1,float shift_x=0,float shift_y=0){
 unsigned ticket=kind?hg::queue_material_target(kind,x,y,{scale,shift_x,shift_y,scale==1?1:.9999f,2.85f}):0;
 alignas(8) uint8_t block[120]{};
 uintptr_t view=7|(uintptr_t(kind)<<28)|(uintptr_t(ticket)<<HG_CAPTURE_VIEW_BITS);
 memcpy(block+0x48,&view,8);
 uintptr_t closure[2]{0,(uintptr_t)block};hg::material_raster((uintptr_t)closure);
 assert(*(uintptr_t*)(block+0x48)==7&&hg::export_kind==0);
}
static unsigned buffer_releases=0,forwarded_callbacks=0;static bool empty_buffer=false;
static unsigned hook_calls=0,unhook_calls=0,fail_hook_at=0;
extern "C" void AHardwareBuffer_release(AHardwareBuffer*buffer){assert(buffer==(AHardwareBuffer*)0x1000);buffer_releases++;}
extern "C" int hg_install_native_hook(void*target,void*,void**original){
 if(!fail_hook_at)return -1;
 if(++hook_calls==fail_hook_at)return -1;
 *original=target;return 0;
}
extern "C" int hg_remove_native_hook(void*){unhook_calls++;return 0;}
static AHardwareBuffer*get_buffer(uintptr_t,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,uintptr_t i,uintptr_t j,uintptr_t k){
 assert(b==2&&c==3&&d==4&&e==5&&f==6&&g==7&&h==8&&i==9&&j==10&&k==11);return empty_buffer?nullptr:(AHardwareBuffer*)0x1000;
}
static void callback(void*p,uintptr_t key){assert((uintptr_t)p==key+0x100||!p);forwarded_callbacks++;}
static void init(uintptr_t key,void*surface,hg::StreamCallback cb,int display,float rate,int a,int b,int orientation,const char*const*names,int count){
 assert(surface==(void*)9&&display==1&&rate==.25f&&a==1120&&b==2436&&orientation==-1&&count==1&&!strcmp(names[0],"keep-original-window-rule"));cb((void*)(key+0x100),key);
}
static bool draw_textures=false;static hg::NativeRef texture_input;static uint32_t seen_texture=0;
static unsigned cache_lookups=0,tinted_lookups=0;
static std::map<std::pair<uintptr_t,uint8_t>,uintptr_t> cache_entries;
static std::vector<std::pair<uintptr_t,hg::BlurCacheKey>> removed_cache_keys;
static bool cache_lookup(uintptr_t cache,const void*key,const void*,void*entry,bool*found){
 cache_lookups++;
 auto item=cache_entries.find({cache,static_cast<const uint8_t*>(key)[0x18]});
 bool hit=item!=cache_entries.end();
 if(found)*found=hit;
 if(hit&&entry)*static_cast<uintptr_t*>(entry)=item->second;
 return hit;
}
static bool tinted_lookup(uintptr_t cache,const void*key,const void*,uint32_t,void*){
 tinted_lookups++;
 return cache_entries.count({cache,static_cast<const uint8_t*>(key)[0x18]})!=0;
}
static void remove_cache_key(uintptr_t cache,const void*key){
 assert(hg::active_pin&&hg::active_pin->active);
 assert(*(uintptr_t*)(state+0x60)==0);
 hg::BlurCacheKey normalized=hg::normalize_blur_key(key);
 assert(!memcmp(key,normalized.bytes,sizeof(normalized.bytes)));
 assert(cache_entries.erase({cache,normalized.bytes[0x18]})==1);
 removed_cache_keys.emplace_back(cache,normalized);
}
static bool expect_pin=false;
static void check_cache_draw(bool target_rect){
 const auto context=(uintptr_t)fake_context;
 auto&home=hg::materials.at(context);
 const bool selected=target_rect&&(hg::export_kind==1||hg::export_kind==2);
 assert(hg::active_draw&&hg::active_draw->selected==selected);
 assert((hg::active_pin&&hg::active_pin->active)==expect_pin);
 auto texture=*(hg::NativeRef*)state;
 if(expect_pin){
  assert(hg::active_pin->context==context&&texture.object==home.texture.object);
  assert(!(*(hg::NativeRef*)(state+0x70)).object);
  assert(*(uintptr_t*)(state+0x18)==home.generation&&*(uintptr_t*)(state+0x60)==0);
 }else{
  assert(texture.object!=home.texture.object);
  assert((*(hg::NativeRef*)(state+0x70)).object&&*(uintptr_t*)(state+0x60)==123);
 }
 assert(hg::material_acquire((uintptr_t)canvas,nullptr,nullptr,1,1,1,nullptr,false));
 assert((*(hg::NativeRef*)state).object==texture.object);
 if(expect_pin)assert(observed_generation==home.generation&&observed_factory==0);
 hg::BlurCacheKey key;
 key.bytes[0]=2;key.bytes[8]=45;key.bytes[0x10]=1;key.bytes[0x14]=17;
 key.bytes[0x18]=target_rect?21:23;
 const auto lookups=cache_lookups,tinted=tinted_lookups;
 bool found=true;
 uintptr_t entry=0;
 if(!expect_pin||!selected){
  assert(hg::material_try_get(101,key.bytes,nullptr,&entry,&found)&&found);
  assert(entry!=home.texture.object&&hg::material_try_get_tinted(101,key.bytes,nullptr,0,nullptr));
  assert(cache_lookups==lookups+1&&tinted_lookups==tinted+1);
  if(!expect_pin)return;
  key.bytes[0x18]=22;
  assert(!hg::material_try_get(101,key.bytes,nullptr,nullptr,&found)&&!found);
  assert(cache_lookups==lookups+2);
  cache_entries.emplace(std::make_pair(101,22),texture.object);
  return;
 }
 assert(cache_entries.at({101,21})!=texture.object);
 assert(!hg::material_try_get(101,key.bytes,nullptr,&entry,&found)&&!found&&!entry);
 assert(!hg::material_try_get_tinted(101,key.bytes,nullptr,0,nullptr));
 auto other_padding=key;
 for(unsigned byte:{4,5,6,7,0x11,0x12,0x13,0x1c,0x1d,0x1e,0x1f})other_padding.bytes[byte]=0xff;
 assert(!hg::material_try_get(101,other_padding.bytes,nullptr,nullptr,nullptr));
 assert(!hg::material_try_get(102,other_padding.bytes,nullptr,nullptr,nullptr));
 assert(cache_lookups==lookups&&tinted_lookups==tinted);
 cache_entries.at({101,21})=texture.object;
 cache_entries.at({102,21})=texture.object;
}
static bool texture_bind(uintptr_t pass,uint32_t stage,uint32_t type,const void*slot,const void*metadata,hg::NativeRef*texture,uintptr_t sampler){
 assert(pass==11&&stage==2&&type==3&&slot==(void*)12&&metadata==(void*)13&&sampler==14);seen_texture=texture->object?resource(*texture).color:0;hg::released(*texture);*texture={};return true;
}
static bool draw_uniforms=false,host_copy_failure=false;static uint64_t uniform_binding=0;
static unsigned uniform_writes=0;
static uint64_t draw_backdrop=7;
static uint8_t uniform_data[512]{};static uintptr_t uniform_view[]={0,0,123,13,192};static float seen_matrix[16];
static uint8_t*buffer_contents(uintptr_t buffer){assert(buffer==123);return uniform_data;}
static bool copy_host(uintptr_t buffer,const uint8_t*data,size_t start,size_t length,size_t offset){
 assert(buffer==123&&start==0&&offset+length<=sizeof(uniform_data));if(host_copy_failure)return false;uniform_writes++;memcpy(uniform_data+offset,data,length);return true;
}
static bool uniform(uintptr_t pass,uint64_t binding,uint32_t type,void*view){
 assert(pass==11&&binding==uniform_binding&&type==6&&view==uniform_view);memcpy(seen_matrix,uniform_data+77,sizeof(seen_matrix));return true;
}
static bool draw(uintptr_t c,const void*,const void*,const void*clip,uint64_t view,uint64_t has_view,uint64_t backdrop,uint64_t has_backdrop,float alpha){
 assert(c==(uintptr_t)canvas&&clip==(void*)9&&view==55&&has_view==1&&backdrop==draw_backdrop&&has_backdrop==1&&alpha==1);
 if(draw_textures){auto texture=hg::retained(texture_input);if(!hg::material_bind(11,2,3,(void*)12,(void*)13,&texture,14))return false;assert(!texture.object);}
 return !draw_uniforms||hg::material_uniform(11,uniform_binding,6,uniform_view);
}
int main(){
 control_table[2]=(uintptr_t)destroy_resource;control_table[4]=(uintptr_t)free_control;
 allocator_ref=make_resource("allocator");hg::allocator_slot=0xc0;hg::command_slot=0xe8;context_table[0xc0/8]=(uintptr_t)allocator_maker;context_table[0xe8/8]=(uintptr_t)command_maker;
 blit_table[0x18/8]=(uintptr_t)shader_read;blit_table[0x28/8]=(uintptr_t)encode;
 hg::create_texture=texture_maker;hg::create_blit=blit_maker;hg::add_copy=add_copy;hg::enqueue=enqueue;
 hg::original_acquire=acquire;hg::original_raster=raster;
 fake_context[0]=(uintptr_t)context_table;fake_context[1]=(uintptr_t)backend;backend[2]=(uintptr_t)state;content[0]=(uintptr_t)fake_context;canvas[0]=(uintptr_t)content;
 auto source=make_resource("source");resource(source).color=0xff335577;*(hg::NativeRef*)state=source;*(uintptr_t*)(state+0x18)=12;*(uintptr_t*)(state+0x60)=123;
 auto keeper=make_resource("keeper");*(hg::NativeRef*)(state+0x70)=keeper;
 const auto context=(uintptr_t)fake_context;
 auto time=hg::Clock::now();
 auto observe=[&](bool resting,int ms){hg::observe_home(context,resting,time+std::chrono::milliseconds(ms));};
 export_frame(0);assert(observed_color==0xff335577&&copies==0);
 observe(true,0);observe(true,50);assert(copies==1);
 consume_pending=true;export_frame(1);assert(observed_color==0xff123456&&copies==1);
 export_frame(3);assert(copies==1&&hg::materials.at(context).generation==12);
 resource(source).color=0xff000000;*(uintptr_t*)(state+0x18)=33;
 export_frame(2);assert(observed_color==0xff335577&&observed_generation==12&&observed_factory==0);
 assert(*(uintptr_t*)(state+0x18)==33&&*(uintptr_t*)(state+0x60)==123&&(*(hg::NativeRef*)state).object==source.object&&(*(hg::NativeRef*)(state+0x70)).object==keeper.object);
 export_frame(0);assert(observed_color==0xff000000&&observed_generation==33&&observed_factory==123);
 // Detached scenes are placed in physical coordinates before the raster.
 // Even with a unit correction, they pin the resting source on demand.
 cached_raster=true;
 raster_draw=[](){
  const float rect[]={869,1656,1040,1827};
  assert(hg::export_target&&hg::export_target->transform.scale==1);
  hg::ensure_export_home_pinned((uintptr_t)fake_context);
  assert(hg::active_pin&&hg::active_pin->active&&hg::export_target->x==unsigned(rect[0]));
  acquire((uintptr_t)canvas,nullptr,nullptr,1,1,1,nullptr,false);
 };
 export_frame(2,869,1656);
 assert(observed_color==0xff335577&&observed_generation==12&&observed_factory==0);
 assert(*(uintptr_t*)(state+0x18)==33&&*(uintptr_t*)(state+0x60)==123);
 raster_draw=nullptr;cached_raster=false;
 observe(false,100);observe(true,150);observe(false,165);
 assert(copies==1&&hg::materials.at(context).generation==12);
 observe(true,200);observe(true,250);assert(copies==2&&hg::materials.at(context).generation==33);
 // Home resumes at full scale while the new stream still samples the app.
 hg::set_home_material_ready(false);
 resource(source).color=0xff5a5a5a;*(uintptr_t*)(state+0x18)=34;
 observe(true,255);observe(true,500);observe(true,750);
 assert(copies==2&&hg::materials.at(context).generation==33);
 export_frame(2);
 assert(observed_color==0xff000000&&observed_generation==33);
 export_frame(0);
 assert(observed_color==0xff5a5a5a&&observed_generation==34);
 hg::set_home_material_ready(true);
 observe(true,800);assert(copies==2);
 resource(source).color=0xffabcdef;
 observe(true,850);assert(copies==3&&hg::materials.at(context).generation==34);
 cached_raster=true;export_frame(1);cached_raster=false;
 resource(source).color=0xff445566;(*(uintptr_t*)(state+0x18))++;
 allocation_failure=true;observe(true,900);assert(copies==3);allocation_failure=false;
 export_frame(2);assert(observed_color==0xffabcdef);
 unsigned orphan=hg::queue_material_target(2,100,200,{1,0,0,1,2.85f});
 unsigned other_stage=hg::queue_material_target(3,101,201,{1,0,0,1,4});
 auto newest=hg::queue_material_target(2,102,202,{1,0,0,1,2.85f});
 assert(hg::take_target(newest,2)->x==102&&hg::take_target(other_stage,3)->x==101);
 assert(hg::display_density.load()==2.85f);
 assert(!hg::take_target(orphan,1)&&!hg::take_target(orphan,2));
 hg::original_dequeue=get_buffer;hg::original_init_excluded=init;hg::original_init_orientation=init;const char*names[]={"keep-original-window-rule"};
 hg::init_orientation(1,(void*)9,callback,1,.25f,1120,2436,-1,names,1);hg::init_excluded(2,(void*)9,callback,1,.25f,1120,2436,-1,names,1);
 auto take=[](uintptr_t stream){return hg::dequeue(stream,2,3,4,5,6,7,8,9,10,11);};
 empty_buffer=true;assert(take(0x101)==nullptr&&buffer_releases==0);empty_buffer=false;
 assert(take(0x102)==nullptr&&buffer_releases==1);assert(take(0x101)==nullptr&&buffer_releases==2);
 assert(take(0x101)==(AHardwareBuffer*)0x1000&&take(0x102)==(AHardwareBuffer*)0x1000&&take(0x999)==(AHardwareBuffer*)0x1000&&buffer_releases==2);
 hg::stream_callback(nullptr,1);hg::stream_callback((void*)0x101,1);assert(take(0x101)==nullptr&&buffer_releases==3&&forwarded_callbacks==4);
 // Native CanvasImage receives a small GPU image before its byte export. The
 // full image and all context/texture owners remain valid until copy submission.
 image_table[2]=(uintptr_t)dispose_image;hg::original_make_image=make_image;hg::original_image_ready=image_ready;
 alignas(8) uint8_t dart[256]{};uintptr_t arena=(uintptr_t)dart,heap=arena&~uintptr_t(0xffffffff),wrapper=arena+65,fields=arena+129,storage=0x12345678;
 auto pointer=[&](uintptr_t at,uintptr_t value){uint32_t low=uint32_t(value);memcpy((void*)at,&low,4);};
 pointer(arena+1+7,wrapper);pointer(wrapper+7,fields);pointer(wrapper+11,arena+193);uintptr_t storage_address=(uintptr_t)&storage;memcpy((void*)(fields+7),&storage_address,8);
 assert(hg::image_peer(arena+1,heap)==storage);
 *(int64_t*)(source.object+0x20)=1120;*(int64_t*)(source.object+0x28)=2436;
 for(unsigned scenario=0;scenario<4;scenario++){
  hg::export_kind=scenario==3?0:1;hg::export_context={(uintptr_t)fake_context,0};hg::export_width=1120;hg::export_height=2436;
  auto input=hg::retained(source);uintptr_t image=0;make_image(&image,&input,2);hg::material_image_ready(0,&image);assert(!input.object);
  hg::export_kind=0;hg::export_context={};uintptr_t peer[8]{};peer[6]=image;
  allocation_failure=scenario==2;bool ok=hg::crop_export_image((uintptr_t)peer,scenario==1?1100:69,802,171,171);allocation_failure=false;
  assert(ok==(scenario==0));assert(hg::captured_images.empty());
  if(ok){assert(peer[6]!=image&&((FakeImage*)image)->disposed);auto&out=*(FakeImage*)peer[6];assert(out.owning==2&&resource(out.texture).color==resource(source).color);assert(copied_rect[0]==69&&copied_rect[1]==802&&copied_rect[2]==240&&copied_rect[3]==973);}
  else assert(peer[6]==image&&!((FakeImage*)image)->disposed);
  hg::release_image(peer[6]);
 }
 hg::export_kind=1;hg::export_context={(uintptr_t)fake_context,0};
 for(unsigned i=0;i<12;i++){auto input=hg::retained(source);uintptr_t image=0;make_image(&image,&input,0);hg::material_image_ready(0,&image);hg::release_image(image);assert(hg::captured_images.size()<=8);}
 hg::captured_images.clear();hg::export_kind=0;hg::export_context={};
 hg::original_draw=draw;hg::original_bind=texture_bind;
 hg::original_uniform=uniform;hg::buffer_contents=buffer_contents;hg::copy_host=copy_host;
 static float rect[]={869,1656.05f,1040,1827.05f};
 hg::original_try_get=cache_lookup;hg::original_try_get_tinted=tinted_lookup;hg::cache_remove=remove_cache_key;
 static float unrelated_rect[]={606,1371,777,1542};
 raster_draw=+[]{
  assert(hg::material_draw((uintptr_t)canvas,nullptr,rect,(void*)9,55,1,draw_backdrop,1,1));
  assert(hg::material_draw((uintptr_t)canvas,nullptr,unrelated_rect,(void*)9,55,1,draw_backdrop,1,1));
 };
 hg::original_draw=+[](uintptr_t,const void*,const void*draw_rect,const void*,uint64_t,uint64_t,uint64_t,uint64_t,float){
  check_cache_draw(draw_rect==rect);
  return true;
 };
 cached_raster=true;  // Pin begins in draw, before the first acquire.
 uint8_t live_state[sizeof(state)];memcpy(live_state,state,sizeof(state));
 const auto saved_home=hg::materials.at(context).texture;
 cache_entries={{{101,21},source.object},{{102,21},source.object},{{101,23},source.object}};
 for(unsigned kind:{0,1,3}){
  export_frame(kind,869,1656);
  assert(removed_cache_keys.empty()&&!hg::active_pin);
 }
 hg::materials.at(context).texture={};
 export_frame(2,869,1656);
 assert(removed_cache_keys.empty()&&!hg::active_pin);
 hg::materials.at(context).texture=saved_home;
 expect_pin=true;
 export_frame(2,869,1656);
 expect_pin=false;
 assert(!hg::active_pin&&!memcmp(live_state,state,sizeof(state)));
 assert(resource(saved_home).control.owners==0&&resource(source).control.owners==0&&resource(keeper).control.owners==0);
 assert(cache_entries.size()==1&&cache_entries.at({101,23})==source.object);
 assert(removed_cache_keys.size()==3);
 assert(removed_cache_keys[0].first==101&&removed_cache_keys[0].second.bytes[0x18]==21);
 assert(removed_cache_keys[1].first==102&&removed_cache_keys[1].second.bytes[0x18]==21);
 assert(removed_cache_keys[2].first==101&&removed_cache_keys[2].second.bytes[0x18]==22);
 cached_raster=false;
 raster_draw=+[]{assert(hg::material_draw((uintptr_t)canvas,nullptr,rect,(void*)9,55,1,draw_backdrop,1,1));};
 hg::original_draw=draw;
 draw_uniforms=true;
 const float scale=.861560663f,shift_x=27.1824672f*2.85f,shift_y=59.1218662f*2.85f;
 const float unscaled[16]={2.85f,0,0,0,0,2.85f,0,0,0,0,1,0,rect[0],rect[1],0,1};
 float zoomed[16];memcpy(zoomed,unscaled,sizeof(zoomed));
 zoomed[0]*=scale;zoomed[5]*=scale;
 zoomed[12]=rect[0]*scale+shift_x;zoomed[13]=rect[1]*scale+shift_y;
 zoomed[15]=.9999f;
 auto matrix=[&](const float*values){memset(uniform_data,0xaa,sizeof(uniform_data));memcpy(uniform_data+77,values,sizeof(zoomed));};
 auto compare=[&](const float*expected){for(unsigned i=0;i<16;i++)assert(std::abs(seen_matrix[i]-expected[i])<.001f);};
 matrix(zoomed);export_frame(2,869,1656,scale,shift_x,shift_y);compare(unscaled);
 assert(uniform_data[76]==0xaa&&uniform_data[141]==0xaa);
 assert(uniform_writes==1);
 matrix(unscaled);export_frame(2,869,1656,scale,shift_x,shift_y);
 assert(!memcmp(seen_matrix,unscaled,sizeof(unscaled))&&uniform_writes==1);
 matrix(zoomed);export_frame(2,606,1371,scale,shift_x,shift_y);compare(zoomed);
 matrix(zoomed);export_frame(3,869,1656,scale,shift_x,shift_y);compare(zoomed);
 host_copy_failure=true;export_frame(2,869,1656,scale,shift_x,shift_y);compare(zoomed);host_copy_failure=false;
 uniform_binding=64;export_frame(2,869,1656,scale,shift_x,shift_y);compare(zoomed);uniform_binding=0;
 assert(hg::resting_transform(unscaled,rect,2.85f)&&!hg::resting_transform(zoomed,rect,2.85f));

 // An unrelated press or widget raster cannot evict the target's texture
 // inputs. A new backdrop generation makes earlier inputs ineligible.
 *(uintptr_t*)(state+0x18)=34;
 draw_uniforms=false;draw_textures=true;texture_input=make_resource("material texture");
 resource(texture_input).color=17;
 export_frame(1,869,1656);assert(seen_texture==17&&hg::opening_inputs.size()==1);
 resource(texture_input).color=23;draw_backdrop=8;
 export_frame(1,869,1656);assert(hg::opening_inputs.size()==2);
 draw_backdrop=7;resource(texture_input).color=91;
 export_frame(3,869,1656);assert(seen_texture==91&&hg::opening_inputs.size()==2);
 export_frame(2,869,1656);assert(seen_texture==17&&hg::opening_inputs.size()==1);
 export_frame(2,869,1656);assert(seen_texture==91);
 export_frame(1,869,1656);assert(hg::opening_inputs.size()==2);
 *(uintptr_t*)(state+0x18)=36;observe(true,450);observe(true,500);
 resource(texture_input).color=92;
 export_frame(2,869,1656);assert(seen_texture==92);
 // Sparse draws cannot qualify a new Home scene, but a scene already
 // confirmed at rest must accept a changed wallpaper on its first draw.
 auto&home=hg::materials.at(context);
 assert(home.generation==36);
 *(uintptr_t*)(state+0x18)=37;observe(true,1000);observe(true,1200);
 assert(home.generation==36);
 observe(true,1250);assert(home.generation==37);
 *(uintptr_t*)(state+0x18)=38;observe(true,5000);assert(home.generation==38);
 hg::set_home_material_ready(false);
 *(uintptr_t*)(state+0x18)=39;observe(true,6000);assert(home.generation==38);
 hg::set_home_material_ready(true);observe(true,6100);assert(home.generation==38);
 observe(true,6150);assert(home.generation==39);
 puts("PASS confirmed resting scene refreshes an idle wallpaper without qualifying sparse transition draws");
 hg::set_workspace_scrolling(true);
 *(uintptr_t*)(state+0x18)=40;observe(true,7000);observe(true,7100);
 assert(home.generation==39);
 hg::set_home_material_ready(false);hg::set_workspace_scrolling(false);
 observe(true,7200);assert(home.generation==39);
 hg::set_home_material_ready(true);observe(true,7300);assert(home.generation==39);
 observe(true,7350);assert(home.generation==40);
 puts("PASS workspace motion preserves the resting texture and scroll end cannot reopen an app scene");
 hg::released(texture_input);
 hg::opening_inputs.clear();
 hg::released(hg::export_context);hg::export_context={};
 for(const auto&im:images)assert(im->disposed&&im->count==0);
 hg::materials.clear();hg::released(source);hg::released(keeper);hg::released(allocator_ref);
 for(const auto&r:resources)assert(r->destroyed&&r->freed&&r->control.owners==-1&&r->control.weak==-1);
 void*orig_a=nullptr,*orig_b=nullptr,*orig_c=nullptr;
 const hg::HookSpec group[]={{0x1000,(void*)0x2000,&orig_a},{0x1020,(void*)0x2020,&orig_b},{0x1040,(void*)0x2040,&orig_c}};
 fail_hook_at=3;assert(!hg::install_hook_group(group,3)&&hook_calls==3&&unhook_calls==2&&!orig_a&&!orig_b&&!orig_c);
 hook_calls=unhook_calls=0;fail_hook_at=99;assert(hg::install_hook_group(group,3)&&hook_calls==3&&unhook_calls==0&&orig_a&&orig_b&&orig_c);
 puts("PASS native GPU-copy ownership, producer overwrite isolation, GPU crop geometry, image/context lifetime, bounded pending exports, failure fallback, interleaved widget refresh isolation, full-export backdrop pin and cache eviction, resting uniform preservation, independent stream startup filtering and transactional hook rollback");
}
