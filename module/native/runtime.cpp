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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <vector>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"HyperGlass",__VA_ARGS__)
namespace {
std::atomic<bool> runtime_complete{false};
using Dispatch=void(*)(uintptr_t*);Dispatch original_dispatch=nullptr;
static_assert(sizeof(HgCaptureResult)==HG_CAPTURE_RESULT_SIZE);
struct MountedCapture {
 HgCapturePlacement placement;
 uintptr_t thread=0,layer=0;
 unsigned identity=0;
 double bounds_x=0,bounds_y=0;
};
struct RestingLayer {
 uintptr_t thread=0,layer=0;
 unsigned hash=0;
 double x=0,y=0,scale_x=0,scale_y=0;
 unsigned viewport_width=0,viewport_height=0;
};
constexpr size_t max_mounted_captures=32,max_resting_layers=256;
thread_local std::vector<MountedCapture> mounted_captures;
thread_local std::vector<RestingLayer> resting_layers;
// Physical size per FlutterView. A tablet's dock is a second view on the same
// UI thread, so alternating views must not clear each other's placements.
struct ViewportSize {uintptr_t view;unsigned width,height;};
constexpr size_t max_viewports=8;
thread_local std::vector<ViewportSize> viewport_sizes;
thread_local bool overview_entered=false,workspace_scrolling=false,home_scene_ready=true;
thread_local unsigned resting_snapshot_retries=12;
template<class T> T dart_value(uintptr_t address){
 T value;
 memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));
 return value;
}
HgCaptureResult* capture_result(uintptr_t*r){
 return reinterpret_cast<HgCaptureResult*>(reinterpret_cast<char*>(r)+HG_CAPTURE_RESULT_OFFSET);
}
const MountedCapture* find_mounted_capture(uintptr_t thread,uintptr_t layer,uint32_t identity){
 if(!identity)return nullptr;
 for(auto it=mounted_captures.rbegin();it!=mounted_captures.rend();++it)
  if(it->thread==thread&&it->layer==layer&&it->identity==identity)return &*it;
 return nullptr;
}
void remember_mounted_capture(const MountedCapture&entry){
 if(!entry.identity||!entry.placement.viewport_width||!entry.placement.viewport_height)return;
 mounted_captures.erase(std::remove_if(mounted_captures.begin(),mounted_captures.end(),[&](const MountedCapture&item){
  return item.thread==entry.thread&&item.layer==entry.layer;
 }),mounted_captures.end());
 if(mounted_captures.size()>=max_mounted_captures)mounted_captures.erase(mounted_captures.begin());
 mounted_captures.push_back(entry);
}
void remember_resting_layer(const RestingLayer&entry){
 resting_layers.erase(std::remove_if(resting_layers.begin(),resting_layers.end(),[&](const RestingLayer&item){
  return item.thread==entry.thread&&item.layer==entry.layer;
 }),resting_layers.end());
 if(resting_layers.size()>=max_resting_layers)resting_layers.erase(resting_layers.begin());
 resting_layers.push_back(entry);
 for(auto&item:mounted_captures){
  if(item.thread!=entry.thread||item.layer!=entry.layer)continue;
  if(entry.hash&&item.identity!=entry.hash)continue;
  if(item.placement.viewport_width!=entry.viewport_width||item.placement.viewport_height!=entry.viewport_height)continue;
  if(std::abs(item.placement.scale_x-entry.scale_x)>=0.001||std::abs(item.placement.scale_y-entry.scale_y)>=0.001)continue;
  double rx=entry.x+entry.scale_x*item.bounds_x,ry=entry.y+entry.scale_y*item.bounds_y;
  if(rx>=-0.5&&ry>=-0.5&&rx+item.placement.width*entry.scale_x<=entry.viewport_width+0.5&&
     ry+item.placement.height*entry.scale_y<=entry.viewport_height+0.5){
   item.placement.x=rx;
   item.placement.y=ry;
  }
 }
}
std::optional<HgCapturePlacement> recover_resting_placement(
  uintptr_t thread,uintptr_t layer,uint32_t identity,const HgCapturePlacement&current,double bounds_x,double bounds_y){
 for(auto it=resting_layers.rbegin();it!=resting_layers.rend();++it){
  const auto&e=*it;
  if(e.thread!=thread||e.layer!=layer)continue;
  if(e.hash&&e.hash!=identity)continue;
  if(e.viewport_width!=current.viewport_width||e.viewport_height!=current.viewport_height)continue;
  if(std::abs(e.scale_x-current.scale_x)>=0.001||std::abs(e.scale_y-current.scale_y)>=0.001)continue;
  double rx=e.x+e.scale_x*bounds_x,ry=e.y+e.scale_y*bounds_y;
  if(std::abs(ry-current.y)>2.0)continue;
  if(rx<-0.5||ry<-0.5||rx+current.width*e.scale_x>e.viewport_width+0.5||
     ry+current.height*e.scale_y>e.viewport_height+0.5)continue;
  return HgCapturePlacement{rx,ry,e.scale_x,e.scale_y,current.width,current.height,
                            current.view,e.viewport_width,e.viewport_height};
 }
 return {};
}
void prepare_capture(uintptr_t*r,double&bounds_x,double&bounds_y){
 auto*result=capture_result(r);
 *result={};
 bounds_x=0;bounds_y=0;
 const auto bounds=dart_value<uintptr_t>(r[28]-16);
 if((bounds&1)&&bounds!=r[21]){
  bounds_x=dart_value<double>(bounds+7);
  bounds_y=dart_value<double>(bounds+15);
 }
 const auto source=dart_value<uintptr_t>(r[28]-8);
 if(!(source&1)||source==r[21])return;
 // The Dart hash is stable across collections. Also compare the current
 // object address, so a moved or unrelated object simply takes stock capture.
 if(const auto*found=find_mounted_capture(r[25],source,dart_value<uint32_t>(source+3)))
  result->placement=found->placement;
}
void select_maml_repaint_key(uintptr_t*registers);
void observe_viewport_density(const uintptr_t*registers);
void observe_material_scene(const uintptr_t*registers);
uintptr_t keep_icon_boundary(uintptr_t*registers);
uintptr_t keep_icon_registration(uintptr_t*registers);
void observe_workspace_scroll(const uintptr_t*r){
 const uintptr_t value=r[2],nil=r[21];
 if(value!=nil+0x20&&value!=nil+0x30)return;
 const bool scrolling=value==nil+0x20;
 if(workspace_scrolling&&!scrolling)resting_snapshot_retries=12;
 workspace_scrolling=scrolling;
 hg::set_workspace_scrolling(workspace_scrolling);
}
void export_dispatch(uintptr_t*r){
 uintptr_t mode=r[15];
 if(mode==9){select_maml_repaint_key(r);return;}
 if(mode==10){observe_viewport_density(r);return;}
 if(mode==11){observe_material_scene(r);return;}
 if(mode==12){observe_workspace_scroll(r);return;}
 if(mode==13){r[15]=keep_icon_boundary(r);return;}
 if(mode==14){r[15]=keep_icon_registration(r);return;}
 double bounds_x=0,bounds_y=0;
 if(mode==0)prepare_capture(r,bounds_x,bounds_y);
 original_dispatch(r);
 // The Dart adapter reports the stage through the saved scratch register
 // after marking the view ID in its current frame slot.
 if(mode==0&&r[15]){uintptr_t fp=r[28],bounds=*(uintptr_t*)(fp-16);const double*rect=(const double*)(bounds+7);
  const auto&capture=*capture_result(r);
  if(capture.identity){
   if(!workspace_scrolling){
    remember_mounted_capture({capture.placement,r[25],capture.layer,capture.identity,bounds_x,bounds_y});
   }else if(!find_mounted_capture(r[25],capture.layer,capture.identity)){
    if(auto resting=recover_resting_placement(r[25],capture.layer,capture.identity,capture.placement,bounds_x,bounds_y))
     remember_mounted_capture({*resting,r[25],capture.layer,capture.identity,bounds_x,bounds_y});
   }
  }
  unsigned ticket=hg::queue_material_target(unsigned(r[15]),unsigned(rect[0])-1048576,unsigned(rect[1])-1048576,
                                             capture.transform);
  *capture.view_slot|=uintptr_t(ticket)<<(HG_CAPTURE_VIEW_BITS+1);
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
// The engine maps libapp and then creates the root isolate group. Dart code
// that runs before the libapp batch lands builds state the adapters never see,
// such as the tablet dock's icons. Isolate creation waits for that batch.
std::atomic<bool> app_mapped{false},app_settled{false};
using CreateGroup=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
CreateGroup original_create_group=nullptr;
uintptr_t create_group_gate(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h){
 using namespace std::chrono;
 const auto start=steady_clock::now();
 while(!app_settled.load(std::memory_order_acquire)){
  const auto waited=steady_clock::now()-start;
  // The snapshot arguments already point into libapp. A mapping the worker
  // still has not recognized after its polling interval will not settle.
  if((!app_mapped.load(std::memory_order_acquire)&&waited>=milliseconds(300))||waited>=milliseconds(2000)){
   LOG("Dart startup gate released before libapp adapters settled (mapped=%d)",app_mapped.load()?1:0);break;
  }
  usleep(2000);
 }
 static std::atomic<bool> reported{false};
 if(!reported.exchange(true))LOG("Dart startup gate waited %lld ms",(long long)duration_cast<milliseconds>(steady_clock::now()-start).count());
 return original_create_group(a,b,c,d,e,f,g,h);
}

std::string_view dart_enum_name(uintptr_t type,uintptr_t heap,uintptr_t nil){
 if(!(type&1)||type==nil)return {};
 uintptr_t name=heap+dart_value<uint32_t>(type+0xf);
 const auto string_class=images[0].result.maml_repaint.string_class;
 if(!(name&1)||name==nil||dart_value<uint32_t>(name-1)>>12!=string_class)return {};
 return {reinterpret_cast<const char*>(name+15),dart_value<uint32_t>(name+7)>>1};
}
void observe_material_scene(const uintptr_t*r){
 const uintptr_t signal=r[2],heap=r[27]<<32,nil=r[21];
 if(!(signal&1)||signal==nil)return;
 const auto type=dart_enum_name(heap+dart_value<uint32_t>(signal+7),heap,nil);
 const bool entering_overview=type=="enterOverviewState";
 if(type=="closingRemoteAnimationClose"||(type=="exitOverviewState"&&overview_entered)){
  overview_entered=false;
  home_scene_ready=true;
  resting_snapshot_retries=12;
  hg::set_home_material_ready(true);
 }else if(type=="openingRemoteAnimationClose"||entering_overview){
  overview_entered=entering_overview;
  home_scene_ready=false;
  hg::set_home_material_ready(false);
 }else if(type=="openingRemoteAnimationOpen"){
  overview_entered=false;
  home_scene_ready=false;
  hg::set_home_material_ready(false);
 }else if(type=="closingRemoteAnimationOpen"){
  home_scene_ready=false;
  hg::set_home_material_ready(false);
 }
}
void observe_viewport_density(const uintptr_t*r){
 const auto&viewport=images[0].result;
 const auto*config=reinterpret_cast<const uint32_t*>(hg::adapter_address(viewport,hg_programs[0]->config_source));
 if(!config)return;
 uintptr_t layer=r[1],heap=r[27]<<32,nil=r[21];
 auto ref=[&](uintptr_t object,unsigned offset){return heap+dart_value<uint32_t>(object+offset);};
 auto object=[&](uintptr_t value){return (value&1)&&value!=nil;};
 auto cid=[&](uintptr_t value){return dart_value<uint32_t>(value-1)>>12;};
 if(cid(layer)!=config[2])return;
 uintptr_t view=ref(layer,config[12]);
 if(!object(view))return;
 uintptr_t viewport_config=ref(view,config[10]);
 if(!object(viewport_config))return;
 uintptr_t physical=ref(viewport_config,config[11]);
 if(!object(physical))return;
 double width=dart_value<double>(physical+config[13]),height=dart_value<double>(physical+config[14]);
 if(!(width>=1&&width<=16383&&height>=1&&height<=16383))return;
 const unsigned vw=unsigned(width),vh=unsigned(height);
 auto known=std::find_if(viewport_sizes.begin(),viewport_sizes.end(),[&](const ViewportSize&item){return item.view==view;});
 if(known==viewport_sizes.end()){
  if(viewport_sizes.size()>=max_viewports)viewport_sizes.erase(viewport_sizes.begin());
  viewport_sizes.push_back({view,vw,vh});
  resting_snapshot_retries=12;
 }else if(known->width!=vw||known->height!=vh){
  // A resized view drops only the placements recorded at its former size.
  const unsigned old_width=known->width,old_height=known->height;
  known->width=vw;known->height=vh;
  mounted_captures.erase(std::remove_if(mounted_captures.begin(),mounted_captures.end(),[&](const MountedCapture&item){
   return item.placement.viewport_width==old_width&&item.placement.viewport_height==old_height;
  }),mounted_captures.end());
  resting_layers.erase(std::remove_if(resting_layers.begin(),resting_layers.end(),[&](const RestingLayer&item){
   return item.viewport_width==old_width&&item.viewport_height==old_height;
  }),resting_layers.end());
  resting_snapshot_retries=12;
 }
 uintptr_t matrix=ref(layer,0x53);
 if(!object(matrix))return;
 uintptr_t storage=ref(matrix,7);
 if(!object(storage))return;
 auto transform=dart_value<std::array<double,16>>(storage+0x17);
 double density=transform[0];
 if(!(density>0&&std::isfinite(density)&&density==transform[5]&&transform[1]==0&&transform[4]==0&&
      transform[12]==0&&transform[13]==0&&transform[15]==1))return;
 hg::set_material_density(float(density));
 if(!resting_snapshot_retries||!home_scene_ready||workspace_scrolling||!config[3]||config[0]>config[15])return;
 auto is_workspace_zoom=[](double xx,double yy,const std::array<double,16>&v){
  return xx==0&&yy==0&&v[0]>=0.5&&v[0]<1&&v[5]==v[0]&&v[12]>0&&v[13]>0&&
         v[1]==0&&v[3]==0&&v[4]==0&&v[7]==0&&
         v[8]==0&&v[9]==0&&v[11]==0&&v[15]>0.99&&v[15]<1.01;
 };
 struct Node{uintptr_t layer;double a,b,c,d,x,y;unsigned depth;};
 std::vector<Node> stack;
 stack.push_back({layer,1,0,0,1,0,0,0});
 bool saw_workspace_zoom=false;
 size_t visited=1;
 while(!stack.empty()){
  Node node=stack.back();stack.pop_back();
  uintptr_t p=node.layer;
  uint32_t tag=dart_value<uint32_t>(p-1);
  unsigned type=tag>>12;
  double aa=1,bb=0,cc=0,dd=1,xx=0,yy=0;
  if(type>=config[3]&&type<=config[4]){
   uintptr_t off_obj=ref(p,0x4f);
   if(object(off_obj)){xx=dart_value<double>(off_obj+7);yy=dart_value<double>(off_obj+15);}
  }
  if(type==config[2]){
   uintptr_t mat=ref(p,0x53);if(!object(mat))continue;
   uintptr_t mat_storage=ref(mat,7);if(!object(mat_storage))continue;
   auto v=dart_value<std::array<double,16>>(mat_storage+0x17);
   if(is_workspace_zoom(xx,yy,v)){saw_workspace_zoom=true;continue;}
   aa=v[0];bb=v[1];cc=v[4];dd=v[5];xx+=v[12];yy+=v[13];
  }
  double na=node.a*aa+node.c*bb,nb=node.b*aa+node.d*bb;
  double nc=node.a*cc+node.c*dd,nd=node.b*cc+node.d*dd;
  double nx=node.a*xx+node.c*yy+node.x,ny=node.b*xx+node.d*yy+node.y;
  if(type==config[3]&&std::abs(na-density)<0.001&&std::abs(nd-density)<0.001&&nb==0&&nc==0&&
     nx>=-0.5&&ny>=-0.5&&nx<width&&ny<height){
   remember_resting_layer({r[25],p,dart_value<uint32_t>(p+3),nx,ny,na,nd,vw,vh});
  }
  if(type!=config[15]&&((tag>>8)&15)>=5&&node.depth+1<80){
   for(uintptr_t c=ref(p,0x47);visited<4096&&object(c);c=ref(c,0x37)){
    unsigned c_type=cid(c);
    if(c_type<config[0]||c_type>config[15]||ref(c,0x1f)!=p)break;
    stack.push_back({c,na,nb,nc,nd,nx,ny,node.depth+1});
    ++visited;
   }
  }
 }
 if(saw_workspace_zoom)--resting_snapshot_retries;else resting_snapshot_retries=0;
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
// A Hero flight builds copies of an icon's subtree in its overlay shuttle and
// in MultiFlyUnderlay, outside the ShortcutIconWidget that owns the capture
// GlobalKey. The tablet dock also shows a controller that another attached
// ShortcutIconWidget uses. GlobalKey reparenting would move the one
// keyed boundary between them and leave the visible one empty. Keep a boundary
// in place while its element is attached, and keep the attached icon's
// registration; moves inside one icon keep stock reparenting.
struct IconKeyScope {
 const hg::IconRetakeLayout&abi;
 uintptr_t heap,nil,dispatch,sentinel;
 bool object(uintptr_t value)const{return (value&1)&&value!=nil;}
 uintptr_t ref(uintptr_t value,uint32_t offset)const{return heap+dart_value<uint32_t>(value+offset);}
 uintptr_t root(uintptr_t element)const{
  for(unsigned depth=0;depth<4096;depth++){
   uintptr_t parent=ref(element,abi.parent);
   if(!object(parent))return element;
   element=parent;
  }
  return 0;
 }
 // The nearest ShortcutIconWidget ancestor decides. Its class dispatches
 // build to ShortcutIconWidget.build; only then is its controller read.
 // Returns that icon element when its controller owns the key.
 uintptr_t owner(uintptr_t element,uintptr_t key)const{
  for(unsigned depth=0;depth<64&&object(element);depth++,element=ref(element,abi.parent)){
   const uintptr_t widget=ref(element,abi.widget);
   if(!object(widget))continue;
   const uintptr_t cid=dart_value<uint32_t>(widget-1)>>12;
   if(dart_value<uintptr_t>(dispatch+8*(cid+abi.build_selector))!=abi.icon_build)continue;
   const uintptr_t controller=ref(widget,abi.controller);
   return object(controller)&&controller!=sentinel&&ref(controller,abi.capture_key)==key?element:0;
  }
  return 0;
 }
};
IconKeyScope icon_key_scope(const uintptr_t*r){
 const auto&abi=images[0].result.icon_retake;
 return {abi,r[27]<<32,r[21],r[20],abi.icon_build?dart_value<uintptr_t>(r[26]+abi.sentinel):0};
}
// Element._retakeInactiveElement: x3 is the key's current element, x4 its
// parent, FP-8 the inflating parent and FP-0x10 the new widget. A nonzero
// result inflates a separate element instead of moving the current one.
uintptr_t keep_icon_boundary(uintptr_t*r){
 const auto scope=icon_key_scope(r);
 if(!scope.abi.icon_build)return 0;
 const uintptr_t holder=r[3],element=dart_value<uintptr_t>(r[28]-8),widget=dart_value<uintptr_t>(r[28]-0x10);
 if(!scope.object(holder)||!scope.object(r[4])||!scope.object(element)||!scope.object(widget))return 0;
 const uintptr_t key=scope.ref(widget,scope.abi.key);
 // A detached holder is a legitimate move out of a deactivated subtree.
 const uintptr_t live=scope.root(element);
 if(!scope.object(key)||!live||scope.root(holder)!=live)return 0;
 // Only the icon capture key is guarded; other GlobalKeys keep stock moves.
 const uintptr_t from=scope.owner(holder,key),to=scope.owner(element,key);
 return (from||to)&&from!=to;
}
// Element.mount: x2 is the GlobalKey and FP-8 the element. Its BuildOwner is
// loaded from the element after this class-range test.
// A copy leaves an attached icon's registration in place, so the copy's
// unmount cannot unregister the key that SF captures resolve.
uintptr_t keep_icon_registration(uintptr_t*r){
 const auto scope=icon_key_scope(r);
 const auto&registry_abi=images[0].result.maml_repaint;
 if(!scope.abi.icon_build||!registry_abi.element_map)return 0;
 const uintptr_t key=r[2],element=dart_value<uintptr_t>(r[28]-8);
 if(!scope.object(key)||!scope.object(element))return 0;
 const uintptr_t owner=scope.ref(element,scope.abi.owner);
 if(!scope.object(owner))return 0;
 auto entries=dart_map_entries(scope.ref(owner,registry_abi.element_map),scope.heap,scope.nil);
 uintptr_t holder=0;
 for(uint32_t i=0;i<entries.size;i+=2)
  if(dart_value<uint32_t>(entries.data+0xf+4*i)==uint32_t(key)){holder=scope.heap+dart_value<uint32_t>(entries.data+0x13+4*i);break;}
 if(!scope.object(holder)||holder==element)return 0;
 const uintptr_t live=scope.root(element);
 if(!live||scope.root(holder)!=live)return 0;
 const uintptr_t attached=scope.owner(holder,key);
 return attached&&scope.owner(element,key)!=attached;
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
   images[i].base=m.begin;images[i].path=path;if(i==0)app_mapped.store(true,std::memory_order_release);break;
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
 static bool gate_attempted=false;
 if(!gate_attempted&&images[1].result.isolate_group_entry&&!images[0].attempted){
  gate_attempted=true;
  if(hg_install_native_hook((void*)images[1].result.isolate_group_entry,(void*)create_group_gate,(void**)&original_create_group)!=0||!original_create_group){
   original_create_group=nullptr;LOG("Dart startup gate unavailable");
  }
 }
 // Marked Dart exports and their native scheduler share one commit batch.
 bool snapshots=(images[0].result.ready&2)&&(images[1].result.ready&2);
 if(snapshots)snapshots=hg::install_material_hooks(images[1].base,*hg_programs[1],images[1].result)&&hg::install_backdrop_stream_hooks();
 if(snapshots&&!original_dispatch){
  uintptr_t target=hg::adapter_address(images[0].result,0x2004770);
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
 if(images[0].attempted&&!snapshots_pending&&(rc>=0||rc==-4))app_settled.store(true,std::memory_order_release);
 if(all_attempted&&!snapshots_pending&&!settings_pending&&(rc>=0||rc==-4))runtime_complete.store(true,std::memory_order_release);
 return rc>=0?ready:0;
}
extern "C" bool hg_runtime_complete(){
 return runtime_complete.load(std::memory_order_acquire);
}
