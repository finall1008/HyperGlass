#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
namespace {
bool settings_test=false,storage_available=false,setting_absent=false;
int setting_error=ENOENT,setting_byte='1',setting_reads=0,setting_batches=0;
FILE* settings_fopen(const char*path,const char*mode){
 if(settings_test&&!strcmp(path,"/storage/emulated/0/Android/media/dev.hyperglass.lsposed/badge-monochrome")){
  ++setting_reads;
  if(!storage_available||setting_absent){errno=setting_error;return nullptr;}
  FILE*f=tmpfile();
  if(f){if(setting_byte!=EOF)fputc(setting_byte,f);rewind(f);}
  return f;
 }
 return fopen(path,mode);
}
int settings_access(const char*path,int mode){
 if(settings_test&&!strcmp(path,"/storage/emulated/0/Android")){
  if(storage_available)return 0;
  errno=ENOENT;return -1;
 }
 return access(path,mode);
}
}
#define fopen settings_fopen
#define access settings_access
#include "runtime.cpp"
#undef access
#undef fopen
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace {
std::mutex pause_mutex;
std::condition_variable pause_cv;
bool worker_inside=false,release_worker=false,callback_returned=false;
std::atomic<unsigned>guard_calls{0};
const uint32_t* test_viewport=nullptr;
float viewport_density=0;
bool test_capture_queue=false;
std::vector<bool> scene_updates,scroll_updates;

void test_icon_global_key(){
 alignas(8) uint8_t objects[0x800]{};
 const uintptr_t arena=(uintptr_t)objects+1,heap=arena&~uintptr_t(0xffffffff);
 const uintptr_t nil=arena,sentinel=arena+0x20,key=arena+0x40,root=arena+0x60,
                 icon=arena+0x80,copy=arena+0xa0,detached=arena+0xc0,holder=arena+0xe0,
                 element=arena+0x100,icon_widget=arena+0x120,boundary_widget=arena+0x140,
                 controller=arena+0x160,owner=arena+0x180,registry=arena+0x1a0,entries=arena+0x1c0,
                 dock=arena+0x1e0,dock_widget=arena+0x200;
 auto field=[](uintptr_t address,uintptr_t value){
  uint32_t compressed=uint32_t(value);memcpy((void*)address,&compressed,sizeof(compressed));
 };
 auto&result=images[0].result;
 auto&abi=result.icon_retake;
 abi={7,0xb,7,0x13,8,0xf,2,0x1234,0x1b};
 auto&element_map=result.maml_repaint.element_map;element_map=0x1f;
 uintptr_t dispatch[8]{},pool[]={0,sentinel},frame[]={boundary_widget,0,0},registers[31]{};
 dispatch[abi.build_selector+3]=abi.icon_build;
 for(auto node:{root,icon,copy,detached,holder,element,dock})field(node+abi.widget,nil);
 field(root+abi.parent,nil);field(detached+abi.parent,nil);
 field(icon+abi.parent,root);field(copy+abi.parent,root);
 field(icon+abi.widget,icon_widget);field(icon_widget-1,3<<12);
 field(icon_widget+abi.controller,controller);field(controller+abi.capture_key,key);
 // A second attached icon, such as the tablet dock, with the same controller.
 field(dock+abi.parent,root);field(dock+abi.widget,dock_widget);field(dock_widget-1,3<<12);
 field(dock_widget+abi.controller,controller);
 field(sentinel+abi.capture_key,key); // A matching word must not make the sentinel a controller.
 field(boundary_widget+abi.key,key);field(element+abi.widget,boundary_widget);
 field(owner+element_map,registry);field(registry+0xf,entries);field(registry+0x13,4);
 field(entries+0xb,4);field(entries+0xf,key);field(entries+0x13,holder);
 field(element+abi.owner,owner);
 registers[1]=element;registers[2]=key;registers[3]=holder; // x1 is not yet the BuildOwner at the guard.
 registers[20]=(uintptr_t)dispatch;registers[21]=nil;registers[26]=(uintptr_t)pool;
 registers[27]=heap>>32;registers[28]=(uintptr_t)(frame+2);
 auto check=[&](uintptr_t holder_parent,uintptr_t new_parent,uintptr_t retake,uintptr_t registration){
  field(holder+abi.parent,holder_parent);field(element+abi.parent,new_parent);
  registers[4]=holder_parent;frame[1]=new_parent;
  registers[15]=13;export_dispatch(registers);assert(registers[15]==retake);
  frame[1]=element;
  registers[15]=14;export_dispatch(registers);assert(registers[15]==registration);
 };
 check(detached,copy,0,0);
 check(icon,copy,1,1);
 check(icon,icon,0,0);
 check(copy,icon,1,0);
 check(icon,dock,1,1);
 check(dock,icon,1,1);
 field(icon_widget+abi.controller,sentinel);check(icon,copy,0,0);
 field(icon_widget+abi.controller,controller);
 // Another GlobalKey on the same subtree keeps stock reparenting.
 field(controller+abi.capture_key,sentinel);check(icon,copy,0,0);
 field(controller+abi.capture_key,key);
 abi.icon_build=0;check(icon,copy,0,0);
 abi={};element_map=0;
 puts("PASS icon GlobalKey retake and registration distinguish attached owners, other attached icons, copies and detached holders");
}
}
extern "C" bool hg_page_guard_ready(){
 if(settings_test)return true;
 if(guard_calls.fetch_add(1)==0){
  std::unique_lock<std::mutex>lock(pause_mutex);worker_inside=true;pause_cv.notify_all();
  pause_cv.wait(lock,[]{return release_worker;});
 }
 return false;
}
extern "C" int hg_apply_batch(const hg_patch*patches,size_t count){
 assert(settings_test&&count==1&&patches[0].group==32);++setting_batches;return 1;
}
extern "C" int hg_install_native_hook(void*,void*,void**){assert(false);return -1;}
extern "C" void hg_install_resources(){assert(false);}
namespace hg {
bool image_layout(const char*,std::vector<Elf64_Phdr>&,uint64_t&,std::string&){assert(false);return false;}
bool resolve_program(const char*,uintptr_t,const Program&,Resolved&,bool){assert(false);return false;}
uintptr_t adapter_address(const Resolved&,uint32_t source){assert(test_viewport&&source==hg_programs[0]->config_source);return (uintptr_t)test_viewport;}
bool install_material_hooks(uintptr_t,const Program&,const Resolved&){assert(false);return false;}
bool install_backdrop_stream_hooks(){assert(false);return false;}
void set_material_density(float density){assert(test_viewport);viewport_density=density;}
void set_home_material_ready(bool ready){scene_updates.push_back(ready);}
void set_workspace_scrolling(bool scrolling){scroll_updates.push_back(scrolling);}
unsigned queue_material_target(unsigned,unsigned,unsigned,const HgCaptureTransform&){assert(test_capture_queue);return 1;}
uintptr_t image_peer(uintptr_t,uintptr_t){assert(false);return 0;}
bool crop_export_image(uintptr_t,unsigned,unsigned,unsigned,unsigned){assert(false);return false;}
}
int main(){
 using namespace std::chrono_literals;
 alignas(8) uint8_t viewport[0x1200]{};
 uintptr_t arena=(uintptr_t)viewport,heap=arena&~uintptr_t(0xffffffff);
 uintptr_t layer=arena+1,view=arena+0x101,config=arena+0x201,physical=arena+0x301,matrix=arena+0x401,storage=arena+0x501;
 uint32_t capture[HG_CAPTURE_CONFIG_WORDS]{};
 capture[2]=0x141e;capture[10]=0x5b;capture[11]=0xb;capture[12]=0x2b;capture[13]=15;capture[14]=31;
 auto field=[&](uintptr_t address,auto value){memcpy((void*)address,&value,sizeof(value));};
 field(layer-1,capture[2]<<12);field(layer+0x2b,uint32_t(view));field(view+0x5b,uint32_t(config));
 field(config+0xb,uint32_t(physical));field(physical+15,1120.0);field(physical+31,2436.0);
 field(layer+0x53,uint32_t(matrix));field(matrix+7,uint32_t(storage));
 std::array<double,16> transform{2.85,0,0,0,0,2.85,0,0,0,0,1,0,0,0,0,1};
 field(storage+0x17,transform);
 alignas(16) unsigned char bridge[HG_CAPTURE_RESULT_OFFSET+HG_CAPTURE_RESULT_SIZE]{};
 auto*registers=reinterpret_cast<uintptr_t*>(bridge);registers[1]=layer;registers[15]=10;registers[21]=arena+0x601;registers[27]=heap>>32;
 test_viewport=capture;export_dispatch(registers);assert(viewport_density==2.85f);
 field(physical+15,0.0);transform[0]=transform[5]=3;field(storage+0x17,transform);
 export_dispatch(registers);assert(viewport_density==2.85f);
 field(physical+15,1120.0);transform[0]=transform[5]=2.85;field(storage+0x17,transform);
 test_viewport=nullptr;
 puts("PASS live viewport initializes material density before any marked export and excludes detached layers");
 test_icon_global_key();
 const auto signal=arena+1,type=arena+0x101,name=arena+0x201;
 images[0].result.maml_repaint.string_class=0x73;
 field(signal+7,uint32_t(type));field(type+0xf,uint32_t(name));
 field(name-1,uint32_t(0x73<<12));
 registers[2]=signal;registers[15]=11;
 auto send_signal=[&](const char*text){
  auto length=strlen(text);field(name+7,uint32_t(length*2));
  memcpy((void*)(name+15),text,length);export_dispatch(registers);
 };
 for(const char*text:{"openingRemoteAnimationOpen","homeNewIntent","closingRemoteAnimationOpen",
                      "closingRemoteAnimationClose","exitOverviewState","enterOverviewState","exitOverviewState"}){
  send_signal(text);
 }
 assert(scene_updates==std::vector<bool>({false,false,true,false,true}));
 // Launching an app from overview invalidates its pending Home recovery.
 send_signal("enterOverviewState");send_signal("openingRemoteAnimationOpen");send_signal("exitOverviewState");
 assert(scene_updates==std::vector<bool>({false,false,true,false,true,false,false}));
 puts("PASS animation signals gate the Home handoff and accept only an entered overview's exit");

 const auto source=arena+0x701,bounds_obj=arena+0x801,child_off=arena+0x901,other_layer=arena+0xa01,
            other_off=arena+0xb01,zoom_layer=arena+0xc01,zoom_mat=arena+0xd01,zoom_store=arena+0xe01,
            zoom_child=arena+0xf01;
 uintptr_t frame[3]={bounds_obj,source,0};
 registers[28]=reinterpret_cast<uintptr_t>(frame+2);
 registers[25]=0x1234;
 field(bounds_obj+7,0.0);field(bounds_obj+15,0.0);field(bounds_obj+23,60.0);field(bounds_obj+31,60.0);
 field(source+3,uint32_t(1234));
 double bx=0,by=0;
 remember_mounted_capture({HgCapturePlacement{10,20,2.85,2.85,60,60,0,1120,2436},
                           registers[25],source,1234,0,0});
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.x==10);
 field(source+3,uint32_t(4321));
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.viewport_width==0);
 frame[1]=arena+0x601;
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.viewport_width==0);
 // A collection can move the layer while an app is in front. Its identity
 // hash moves with it and still finds the mounted placement.
 const uintptr_t moved=arena+0x11c1;frame[1]=moved;field(moved+3,uint32_t(1234));
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.x==10);
 frame[1]=source;field(source+3,uint32_t(1234));
 send_signal("openingRemoteAnimationClose");
 send_signal("closingRemoteAnimationClose");
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.x==10);
 puts("PASS detached capture retains matching mounted entries across scene transitions");

 static HgCapturePlacement dispatch_placement{};
 static uintptr_t dispatch_layer=0,dispatch_view_slot=0;
 static unsigned dispatch_identity=0,dispatch_kind=1;
 static uintptr_t dispatch_bounds=0;
 auto stub_dispatch=[](uintptr_t*r){
  auto*res=capture_result(r);
  if(res->placement.viewport_width)return;
  res->view_slot=&dispatch_view_slot;
  res->placement=dispatch_placement;
  res->layer=dispatch_layer;
  res->identity=dispatch_identity;
  res->transform=HgCaptureTransform{1,0,0,1,2.85f};
  double exported[4]={1048576.0+dispatch_placement.x,1048576.0+dispatch_placement.y,
                      1048576.0+dispatch_placement.x+dispatch_placement.width,
                      1048576.0+dispatch_placement.y+dispatch_placement.height};
  memcpy((void*)(dispatch_bounds+7),exported,sizeof(exported));
  r[15]=dispatch_kind;
 };
 test_capture_queue=true;original_dispatch=stub_dispatch;dispatch_bounds=bounds_obj;
 mounted_captures.clear();resting_layers.clear();
 dispatch_layer=source;dispatch_identity=1234;dispatch_kind=1;
 dispatch_placement=HgCapturePlacement{869.0,1656.05,2.85,2.85,60,60,0,1120,2436};
 registers[15]=0;export_dispatch(registers);
 assert(mounted_captures.size()==1&&mounted_captures.front().placement.x==869.0);
 registers[15]=12;
 registers[2]=registers[21]+0x20;export_dispatch(registers);
 assert(workspace_scrolling&&mounted_captures.size()==1);
 dispatch_placement.x=793.0;
 registers[15]=0;export_dispatch(registers);
 assert(mounted_captures.size()==1&&mounted_captures.front().placement.x==869.0);
 registers[15]=12;
 registers[2]=registers[21];export_dispatch(registers);
 assert(workspace_scrolling);
 registers[2]=registers[21]+0x30;export_dispatch(registers);
 assert(!workspace_scrolling&&scroll_updates==std::vector<bool>({true,false}));

 // Cold page-return: resting snapshot captures unvisited resting layers,
 // skips zoomed subtrees, and recovers scrolled opening captures.
 mounted_captures.clear();resting_layers.clear();resting_snapshot_retries=12;
 capture[0]=0x140f;capture[3]=0x141b;capture[4]=0x141f;capture[15]=0x1422;
 field(layer-1,(capture[2]<<12)|0x700);
 field(layer+0x1f,uint32_t(registers[21]));field(layer+0x47,uint32_t(source));
 field(source-1,(capture[3]<<12)|0x500);field(source+3,uint32_t(0));
 field(source+0x1f,uint32_t(layer));field(source+0x37,uint32_t(other_layer));
 field(source+0x47,uint32_t(registers[21]));field(source+0x4f,uint32_t(child_off));
 field(child_off+7,304.91);field(child_off+15,581.07);
 field(other_layer-1,(capture[3]<<12)|0x500);field(other_layer+3,uint32_t(555));
 field(other_layer+0x1f,uint32_t(layer));field(other_layer+0x37,uint32_t(zoom_layer));
 field(other_layer+0x47,uint32_t(registers[21]));field(other_layer+0x4f,uint32_t(other_off));
 field(other_off+7,-200.0);field(other_off+15,581.07);
 field(zoom_layer-1,(capture[2]<<12)|0x700);
 field(zoom_layer+0x1f,uint32_t(layer));field(zoom_layer+0x37,uint32_t(registers[21]));
 field(zoom_layer+0x47,uint32_t(zoom_child));field(zoom_layer+0x53,uint32_t(zoom_mat));
 field(zoom_mat+7,uint32_t(zoom_store));
 std::array<double,16> zmat{0.9,0,0,0,0,0.9,0,0,0,0,1,0,10,10,0,1};
 field(zoom_store+0x17,zmat);
 field(zoom_child-1,(capture[3]<<12)|0x500);field(zoom_child+3,uint32_t(777));
 field(zoom_child+0x1f,uint32_t(zoom_layer));field(zoom_child+0x37,uint32_t(registers[21]));
 field(zoom_child+0x47,uint32_t(registers[21]));field(zoom_child+0x4f,uint32_t(other_off));
 test_viewport=capture;registers[1]=layer;registers[15]=10;
 export_dispatch(registers);
 assert(resting_layers.size()==1&&std::abs(resting_layers.front().x-304.91*2.85)<0.01);
 assert(resting_snapshot_retries==11);
 field(other_layer+0x37,uint32_t(registers[21]));
 field(source+3,uint32_t(1234));
 registers[15]=12;registers[2]=registers[21]+0x20;export_dispatch(registers);
 field(bounds_obj+7,0.0);field(bounds_obj+15,0.0);
 dispatch_placement=HgCapturePlacement{793.0,304.91*0+581.07*2.85,2.85,2.85,60,60,0,1120,2436};
 registers[15]=0;export_dispatch(registers);
 prepare_capture(registers,bx,by);
 assert(std::abs(capture_result(registers)->placement.x-304.91*2.85)<0.01&&
        std::abs(capture_result(registers)->placement.y-581.07*2.85)<0.01);

 // Moving an OffsetLayer at rest updates both caches; viewport resize clears them.
 registers[15]=12;registers[2]=registers[21]+0x30;export_dispatch(registers);
 field(child_off+7,120.0);field(child_off+15,240.0);
 registers[15]=10;export_dispatch(registers);
 prepare_capture(registers,bx,by);
 assert(std::abs(capture_result(registers)->placement.x-120.0*2.85)<0.01&&
        std::abs(capture_result(registers)->placement.y-240.0*2.85)<0.01);
 field(physical+15,1200.0);
 export_dispatch(registers);
 assert(mounted_captures.empty()&&resting_layers.size()<=1);
 // A second FlutterView, such as the tablet dock, reports its own size on
 // the same thread. It must not clear the Home view's placements.
 const auto dock_layer=arena+0x1001,dock_view=arena+0x1041,dock_config=arena+0x1081,
            dock_physical=arena+0x10c1,dock_matrix=arena+0x1101,dock_storage=arena+0x1141;
 field(dock_layer-1,capture[2]<<12);field(dock_layer+0x2b,uint32_t(dock_view));
 field(dock_view+0x5b,uint32_t(dock_config));field(dock_config+0xb,uint32_t(dock_physical));
 field(dock_physical+15,1880.0);field(dock_physical+31,320.0);
 field(dock_layer+0x53,uint32_t(dock_matrix));field(dock_matrix+7,uint32_t(dock_storage));
 field(dock_storage+0x17,transform);
 remember_mounted_capture({HgCapturePlacement{10,20,2.85,2.85,60,60,0,1200,2436},registers[25],source,1234,0,0});
 for(int i=0;i<3;i++){
  registers[1]=dock_layer;export_dispatch(registers);
  registers[1]=layer;export_dispatch(registers);
 }
 assert(mounted_captures.size()==1&&mounted_captures.front().placement.viewport_width==1200);
 // An app reached by quick switching has no opening capture. Its detached
 // layer rebuilds at the resting walk's position; the frame names the view.
 mounted_captures.clear();resting_layers.clear();
 remember_resting_layer({registers[25],source,1234,500,600,2.85,2.85,1200,2436});
 field(source+3,uint32_t(1234));field(source+0x1f,uint32_t(registers[21]));
 field(bounds_obj+7,0.0);field(bounds_obj+15,0.0);field(bounds_obj+23,60.0);field(bounds_obj+31,60.0);
 prepare_capture(registers,bx,by);
 {const auto&pl=capture_result(registers)->placement;
  assert(pl.view==HG_CAPTURE_VIEW_FROM_SLOT&&pl.x==500&&pl.y==600&&pl.width==60&&pl.height==60&&pl.viewport_width==1200);}
 field(source+3,uint32_t(4321));prepare_capture(registers,bx,by);
 assert(capture_result(registers)->placement.viewport_width==0);
 // An attached layer gets the same placement; the adapter uses it only when
 // the full scene cannot reach the target, such as a dock below the viewport.
 field(source+3,uint32_t(1234));field(source+0x1f,uint32_t(layer));
 prepare_capture(registers,bx,by);assert(capture_result(registers)->placement.view==HG_CAPTURE_VIEW_FROM_SLOT);
 resting_layers.clear();
 // A dock icon rebuilt while the app was in front has no mounted capture and
 // lies below the viewport. The mounted icon at its column and size places it.
 static HgCapturePlacement off_report{},off_seen{};static unsigned off_calls=0;
 original_dispatch=[](uintptr_t*r){
  auto*res=capture_result(r);off_calls++;
  if(!res->placement.viewport_width){res->placement=off_report;res->identity=HG_CAPTURE_OFF_VIEWPORT;r[15]=0;return;}
  off_seen=res->placement;res->view_slot=&dispatch_view_slot;r[15]=2;
 };
 auto off_return=[&]{off_calls=0;off_seen={};registers[15]=0;export_dispatch(registers);};
 mounted_captures.clear();resting_layers.clear();field(source+3,uint32_t(0));
 off_report=HgCapturePlacement{861.2,2893,2.85,2.85,132,132,0,1880,3008};
 remember_mounted_capture({HgCapturePlacement{861.1,2779.9,2.85,2.85,132,132,0,1880,3008},registers[25],arena+0x11c1,555,0,0});
 remember_mounted_capture({HgCapturePlacement{705.6,2779.9,2.85,2.85,132,132,0,1880,3008},registers[25],arena+0x1181,556,0,0});
 off_return();assert(off_calls==2&&off_seen.x==861.1&&off_seen.y==2779.9);
 // Earlier layers of the same icon leave identical placements.
 remember_mounted_capture({HgCapturePlacement{861.1,2779.9,2.85,2.85,132,132,0,1880,3008},registers[25],arena+0x1191,557,0,0});
 off_return();assert(off_calls==2&&off_seen.x==861.1);
 // A layer above its mounted place, another size or another column stays stock.
 off_report.y=2700;off_return();assert(off_calls==1);
 off_report.y=2893;off_report.width=173;off_return();assert(off_calls==1);
 off_report.width=132;off_report.x=1016.7;off_return();assert(off_calls==1);
 // Distinct icons at the same column and size are ambiguous.
 off_report.x=861.2;
 remember_mounted_capture({HgCapturePlacement{861.1,1500,2.85,2.85,132,132,0,1880,3008},registers[25],arena+0x11a1,558,0,0});
 off_return();assert(off_calls==1&&capture_result(registers)->placement.viewport_width==0);
 mounted_captures.clear();field(source+3,uint32_t(1234));
 test_viewport=nullptr;original_dispatch=nullptr;test_capture_queue=false;
 puts("PASS workspace scrolling preserves resting captures, recovers scrolled openings and invalidates on viewport resize per view");
 // Each child starts with the real runtime's uncached setting. Failed reads
 // must keep the worker pending; storage recovery must install the badge group.
 for(int scenario=0;scenario<6;scenario++){
  pid_t child=fork();assert(child>=0);
  if(!child){
   settings_test=true;
   for(auto&s:images){s.base=0x1000;s.attempted=true;}
   images[0].result.ready=32;
   hg_patch badge{};badge.group=32;images[0].result.patches.push_back(badge);
   storage_available=scenario>=2;
   setting_error=scenario==1?EACCES:ENOENT;
   setting_absent=scenario==3;
   setting_byte=scenario==4?'0':scenario==5?EOF:'1';
   unsigned first=hg_runtime_tick("settings-first");
   bool pending=scenario<2||scenario==5;
   assert(hg_runtime_complete()!=pending);
   assert(first==(scenario==2?32u:0u));
   assert(setting_reads==1&&setting_batches==(scenario==2?1:0));
   if(pending){
    assert(hg_runtime_tick("settings-still-pending")==0);
    assert(!hg_runtime_complete()&&setting_reads==2&&setting_batches==0);
    storage_available=true;setting_byte='1';
    assert(hg_runtime_tick("settings-recovered")==32);
    assert(hg_runtime_complete()&&setting_reads==3&&setting_batches==1);
   }else{
    // A successfully read off setting (or a default after storage is ready)
    // retains the documented restart semantics.
    storage_available=false;
    assert(badge_monochrome()==(scenario==2?1:0)&&setting_reads==1);
   }
   _exit(0);
  }
  int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 }
 puts("PASS cold-start ENOENT/EACCES and incomplete settings remain pending, recover in the same process and install the badge group; ready defaults and explicit settings stay stable");
 Layout layout;Elf64_Phdr code{};code.p_type=PT_LOAD;code.p_flags=PF_R|PF_X;code.p_vaddr=0x1000;code.p_filesz=0x3000;layout.segments.push_back(code);
 std::vector<Mapping>split={{0x11000,0x12000,0,"/target.apk","r-xp"},{0x12000,0x13000,0,"/target.apk","--xp"},{0x13000,0x14000,0,"/target.apk","r-xp"}};
 assert(executable_mapping(0x10000,"/target.apk",layout,split));
 auto incomplete=split;incomplete.erase(incomplete.begin()+1);assert(!executable_mapping(0x10000,"/target.apk",layout,incomplete));
 incomplete=split;incomplete[1].permission="r--p";assert(!executable_mapping(0x10000,"/target.apk",layout,incomplete));
 incomplete=split;incomplete[1].path="/other.apk";assert(!executable_mapping(0x10000,"/target.apk",layout,incomplete));
 std::thread worker([]{assert(hg_runtime_tick("watch")==0);});
 {std::unique_lock<std::mutex>lock(pause_mutex);assert(pause_cv.wait_for(lock,2s,[]{return worker_inside;}));}
 // The callback represents a thread holding the loader lock. It must return
 // while the worker owns the runtime gate and is waiting on that loader.
 std::thread callback([]{
  assert(hg_runtime_tick("library-loaded")==0);
  std::lock_guard<std::mutex>lock(pause_mutex);callback_returned=true;pause_cv.notify_all();
 });
 {std::unique_lock<std::mutex>lock(pause_mutex);assert(pause_cv.wait_for(lock,2s,[]{return callback_returned;}));
  assert(guard_calls.load()==1);release_worker=true;pause_cv.notify_all();}
 callback.join();worker.join();
 assert(hg_runtime_tick("watch-retry")==0&&guard_calls.load()==2);
 puts("PASS split executable mappings retain ELF identity and complete coverage; loader callback never waits for the worker runtime gate and deferred work retries");
}
