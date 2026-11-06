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
static void raster(uintptr_t closure){
 uintptr_t block=*(uintptr_t*)(closure+8);assert((*(uintptr_t*)(block+0x48)&((1u<<29)|(1u<<28)))==0);
 if(!cached_raster)hg::material_acquire((uintptr_t)canvas,nullptr,nullptr,1,1,1,nullptr,false);
}
static void export_frame(unsigned kind){
 alignas(8) uint8_t block[120]{};uintptr_t view=7|(uintptr_t(kind)<<28);memcpy(block+0x48,&view,8);
 if(kind)hg::queue_material_target(kind,69,802);uintptr_t closure[2]{0,(uintptr_t)block};hg::material_raster((uintptr_t)closure);
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
static bool texture_bind(uintptr_t pass,uint32_t stage,uint32_t type,const void*slot,const void*metadata,hg::NativeRef*texture,uintptr_t sampler){
 assert(pass==11&&stage==2&&type==3&&slot==(void*)12&&metadata==(void*)13&&sampler==14);seen_texture=texture->object?resource(*texture).color:0;hg::released(*texture);*texture={};return true;
}
static bool draw_uniforms=false,host_copy_failure=false;static uint64_t uniform_binding=0;
static uint8_t uniform_data[512]{};static uintptr_t uniform_view[]={0,0,123,13,192};static uint8_t seen_uniform=0;
static uint8_t*buffer_contents(uintptr_t buffer){assert(buffer==123);return uniform_data;}
static bool copy_host(uintptr_t buffer,const uint8_t*data,size_t start,size_t length,size_t offset){
 assert(buffer==123&&start==0&&offset+length<=sizeof(uniform_data));if(host_copy_failure)return false;memcpy(uniform_data+offset,data,length);return true;
}
static bool uniform(uintptr_t pass,uint64_t binding,uint32_t type,void*view){
 assert(pass==11&&binding==uniform_binding&&type==6&&view==uniform_view);seen_uniform=uniform_data[77];return true;
}
static bool draw(uintptr_t c,const void*,const void*,const void*clip,uint64_t view,uint64_t has_view,uint64_t backdrop,uint64_t has_backdrop,float alpha){
 assert(c==(uintptr_t)canvas&&clip==(void*)9&&view==55&&has_view==1&&backdrop==7&&has_backdrop==1&&alpha==1);
 if(draw_textures){auto texture=hg::retained(texture_input);if(!hg::material_bind(11,2,3,(void*)12,(void*)13,&texture,14))return false;assert(!texture.object);}
 return !draw_uniforms||hg::material_uniform(11,uniform_binding,6,uniform_view);
}
int main(){
 control_table[2]=(uintptr_t)destroy_resource;control_table[4]=(uintptr_t)free_control;
 allocator_ref=make_resource("allocator");context_table[0xb8/8]=(uintptr_t)allocator_maker;context_table[0xe0/8]=(uintptr_t)command_maker;
 blit_table[0x18/8]=(uintptr_t)shader_read;blit_table[0x28/8]=(uintptr_t)encode;
 hg::create_texture=texture_maker;hg::create_blit=blit_maker;hg::add_copy=add_copy;hg::enqueue=enqueue;
 hg::original_acquire=acquire;hg::original_raster=raster;
 fake_context[0]=(uintptr_t)context_table;fake_context[1]=(uintptr_t)backend;backend[2]=(uintptr_t)state;content[0]=(uintptr_t)fake_context;canvas[0]=(uintptr_t)content;
 auto source=make_resource("source");resource(source).color=0xff335577;*(hg::NativeRef*)state=source;*(uintptr_t*)(state+0x18)=12;*(uintptr_t*)(state+0x60)=123;
 auto keeper=make_resource("keeper");*(hg::NativeRef*)(state+0x70)=keeper;
 export_frame(0);assert(observed_color==0xff335577&&copies==1);
 consume_pending=true;export_frame(1);assert(observed_color==0xff123456&&copies==1);
 export_frame(3);assert(copies==1&&hg::materials.begin()->second.locked);
 resource(source).color=0xff000000;*(uintptr_t*)(state+0x18)=33;
 export_frame(2);assert(observed_color==0xff335577&&observed_generation==12&&observed_factory==0);
 assert(*(uintptr_t*)(state+0x18)==33&&*(uintptr_t*)(state+0x60)==123&&(*(hg::NativeRef*)state).object==source.object&&(*(hg::NativeRef*)(state+0x70)).object==keeper.object);
 export_frame(0);assert(observed_color==0xff000000&&observed_generation==33&&observed_factory==123);
 resource(source).color=0xffabcdef;(*(uintptr_t*)(state+0x18))++;export_frame(0);cached_raster=true;export_frame(1);cached_raster=false;assert(copies==3&&observed_color==0xffabcdef);
 resource(source).color=0xff445566;allocation_failure=true;export_frame(1);assert(copies==3);
 export_frame(2);assert(observed_color==0xffabcdef);allocation_failure=false;
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
 alignas(8) uint8_t filter[64]{};float rect[]={0,0,60,60};hg::original_draw=draw;
 auto paint=[&](float a){return hg::material_draw((uintptr_t)canvas,filter,rect,(void*)9,55,1,7,1,a);};
 // Preserve matching material sampling parameters. Different bindings,
 // unmatched geometry, normal draws and failed host copies remain live.
 hg::original_uniform=uniform;hg::buffer_contents=buffer_contents;hg::copy_host=copy_host;draw_uniforms=true;
 memset(uniform_data,0xaa,sizeof(uniform_data));memset(uniform_data+77,0x55,64);
 hg::export_context={};hg::target_x=hg::target_y=0;hg::has_target=true;hg::begin_glass_export(1);hg::export_kind=1;assert(paint(1)&&seen_uniform==0x55);
 assert(hg::export_context.object==(uintptr_t)fake_context);
 memset(uniform_data+77,0x99,64);hg::begin_glass_export(3);hg::export_kind=3;assert(paint(1)&&seen_uniform==0x99&&hg::opening_uniforms.size()==1);
 hg::export_kind=2;assert(paint(1)&&seen_uniform==0x55&&uniform_data[76]==0xaa&&uniform_data[141]==0xaa);
 memset(uniform_data+77,0x99,64);rect[0]++;assert(paint(1)&&seen_uniform==0x99);rect[0]--;
 host_copy_failure=true;assert(paint(1)&&seen_uniform==0x99);host_copy_failure=false;
 hg::export_kind=0;assert(paint(1)&&seen_uniform==0x99);
 uniform_binding=64;hg::export_kind=2;assert(paint(1)&&seen_uniform==0x99);hg::export_kind=0;
 hg::original_bind=texture_bind;draw_uniforms=false;draw_textures=true;texture_input=make_resource("material texture");resource(texture_input).color=17;
 hg::begin_glass_export(1);hg::export_kind=1;assert(paint(1)&&seen_texture==17);
 hg::freeze_material_inputs();resource(texture_input).color=23;
 unsigned saved_copies=copies;hg::begin_glass_export(3);hg::export_kind=3;assert(paint(1)&&seen_texture==23&&hg::opening_textures.size()==1&&copies==saved_copies);
 auto producer=texture_input;texture_input=make_resource("replacement material texture");resource(texture_input).color=91;hg::released(producer);hg::export_kind=2;assert(paint(1)&&seen_texture==17);
 rect[0]++;assert(paint(1)&&seen_texture==91);rect[0]--;hg::export_kind=0;assert(paint(1)&&seen_texture==91);
 producer=texture_input;texture_input={};hg::begin_glass_export(1);hg::export_kind=1;assert(paint(1)&&seen_texture==0&&hg::opening_textures.empty());
 hg::export_kind=2;assert(paint(1)&&seen_texture==0);hg::export_kind=0;hg::released(producer);
 hg::opening_uniforms.clear();hg::opening_textures.clear();
 hg::released(hg::export_context);hg::export_context={};
 for(const auto&im:images)assert(im->disposed&&im->count==0);
 hg::materials.clear();hg::released(source);hg::released(keeper);hg::released(allocator_ref);
 for(const auto&r:resources)assert(r->destroyed&&r->freed&&r->control.owners==-1&&r->control.weak==-1);
 void*orig_a=nullptr,*orig_b=nullptr,*orig_c=nullptr;
 const hg::HookSpec group[]={{0x1000,(void*)0x2000,&orig_a},{0x1020,(void*)0x2020,&orig_b},{0x1040,(void*)0x2040,&orig_c}};
 fail_hook_at=3;assert(!hg::install_hook_group(group,3)&&hook_calls==3&&unhook_calls==2&&!orig_a&&!orig_b&&!orig_c);
 hook_calls=unhook_calls=0;fail_hook_at=99;assert(hg::install_hook_group(group,3)&&hook_calls==3&&unhook_calls==0&&orig_a&&orig_b&&orig_c);
 puts("PASS native GPU-copy ownership, producer overwrite isolation, GPU crop geometry, image/context lifetime, bounded pending exports, failure fallback, interleaved widget refresh isolation, stage restoration, independent stream startup filtering and transactional hook rollback");
}
