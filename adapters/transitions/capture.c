#include <stdint.h>
#include "capture_layout.h"
/* Launcher 7722 compressed Dart pointers, tagged object addresses. No Dart
 * allocation or suspension is introduced by these synchronous adapters. */
typedef uintptr_t P;
extern char async_scene_return[], scope_widget[], scope_precreate[], scope_close[], scope_widget_snapshot[], scope_current_icon[], scope_window_icon[], scope_maml_icon[], scope_window_maml[];
#ifdef HG_ADAPTIVE
__attribute__((section(".capture_config"),used))
volatile const uint32_t hg_capture_config[HG_CAPTURE_CONFIG_WORDS]={0x140f,0x1421,0x141e,0x141b,0x141f,0x1e11,0x14b,0x21fa,13,(uint32_t)-0x120,0x5b,0xb,0x2b,0xf,0x1f,0x1422,
0x178b,0x64a,0x64c,0x3b,0x37,0x227,0xab,7,15,11,11,15,15,15,0x1fef,
[HG_CAPTURE_LAYER_SLOT]=(uint32_t)-0x80,[HG_CAPTURE_BOUNDS_SLOT]=(uint32_t)-0x88,
[HG_CAPTURE_ROUND_SLOT]=(uint32_t)-0x90,[HG_CAPTURE_VIEW_SLOT]=(uint32_t)-0x98
};
#define CFG(n,fallback) hg_capture_config[n]
#else
#define CFG(n,fallback) (fallback)
#endif
#define SLOT(n,fallback) ((intptr_t)(int32_t)CFG(n,fallback))
#define META_BASE 1048576.0
static inline P ref(P p,unsigned o,P heap){return heap+*(uint32_t *)(p+o);}
static inline P load(P p){return *(P *)p;}
static inline unsigned cid(P p){return *(uint32_t *)(p-1)>>12;}
static inline void dirty(P p,P nil){*(uint32_t *)(p+0x23)=(uint32_t)(nil+0x20);}
/* A first, full-size shape ImageView is a background plane in RemoteViews.
 * Keep photographs, vector artwork, clickable controls and nested panels. */
__attribute__((section(".widget_helpers"),noinline))
static unsigned background_image(P state,P drawable,P heap,P nil){
 P view=ref(state,CFG(25,11),heap);if(view==nil)return 0;
 P control=ref(view,CFG(25,11),heap);
 if(control==nil||cid(control)!=CFG(16,0x178b))return 0;
 if(drawable==nil||!(drawable&1))return 0;
 unsigned type=cid(drawable);
 if(type!=CFG(17,0x64a)&&type!=CFG(18,0x64c))return 0;
 P layout=ref(control,CFG(19,0x3b),heap);
 if(layout==nil||*(double*)(layout+CFG(23,7))!=-1||*(double*)(layout+CFG(24,15))!=-1)return 0;
 if(ref(control,CFG(22,0xab),heap)!=nil)return 0;
 P parent=ref(control,CFG(20,0x37),heap);if(parent==nil)return 0;
 P owner=ref(parent,CFG(25,11),heap);if(owner==nil||ref(owner,CFG(20,0x37),heap)!=nil)return 0;
 P children=ref(owner,CFG(21,0x227),heap);if(children==nil)return 0;
 if(*(uint32_t*)(children+CFG(26,11))<4)return 0;
 P items=ref(children,CFG(27,15),heap);
 if(items==nil)return 0;
 P first=ref(items,CFG(28,15),heap);
 if(first==nil)return 0;
 if(cid(first)==CFG(30,0x1fef))return ref(first,CFG(29,15),heap)==view;
 return first==view||ref(first,CFG(25,11),heap)==control;
}
/* A centered workspace zoom moves the wallpaper sample during a return.
 * Export the resting material in its unscaled layout coordinates. */
__attribute__((section(".capture_root"),noinline))
static int workspace_zoom(P matrix,P heap,P nil){
 if(matrix==nil)return 0;
 const double *v=(double *)(ref(matrix,7,heap)+0x17);
 return v[0]>=0.5&&v[0]<1&&v[5]==v[0]&&v[1]==0&&v[4]==0&&v[12]>0&&v[13]>0;
}
__attribute__((section(".capture_root"),noinline))
static void dirty_tree(P source,P heap,P nil){
 P p=source;unsigned count=0;
 while(count++<8192){
  dirty(p,nil);P child=nil;unsigned tag=*(uint32_t *)(p-1);
  if(((tag>>8)&15)>=5){P q=ref(p,0x47,heap);if(q!=nil&&(q&1)&&cid(q)>=CFG(0,0x140f)&&cid(q)<=CFG(15,0x1422)&&ref(q,0x1f,heap)==p)child=q;}
  if(child!=nil){p=child;continue;}
  while(p!=source){P next=ref(p,0x37,heap);if(next!=nil){p=next;goto next_node;}p=ref(p,0x1f,heap);}
  return;
 next_node:;
 }
}
__attribute__((section(".capture_root"),noinline))
static int sf_capture(P fp){
 unsigned kind=0;
 for(unsigned i=0;i<80;i++){
  P ret=load(fp+8),parent=load(fp);
  if(ret==(P)scope_close)return 2;
  if(ret==(P)scope_widget||ret==(P)scope_precreate||ret==(P)scope_current_icon||ret==(P)scope_window_icon||ret==(P)scope_maml_icon||ret==(P)scope_window_maml)kind=1;
  /* Provider refreshes export a live image without replacing the material
   * saved for an app that is already open. An enclosing flight wins. */
  else if(ret==(P)scope_widget_snapshot&&!kind)kind=3;
  if(parent<=fp||parent-fp>65536)return kind;fp=parent;
 }
 return kind;
}
__attribute__((section(".capture_root"),noinline))
static unsigned full_scene(P fp,P data,P bounds,P heap,P nil){
 if(load(fp+8)!=(P)async_scene_return)return 0;
 unsigned kind=sf_capture(fp);
 if(!kind)return 0;
 P parent=load(fp);
 P *view_id=(P *)(parent+SLOT(HG_CAPTURE_VIEW_SLOT,-0x98));
 P id=*view_id;
 if(id&1)return 0;
 P source=load(fp-8),root=source,p=source;
 double a=1,b=0,c=0,d=1,x=0,y=0;
 for(unsigned i=0;i<80&&p!=nil;i++){
  unsigned id=cid(p);double aa=1,bb=0,cc=0,dd=1,xx=0,yy=0;
  if(id==CFG(2,0x141e)){P m=ref(p,0x57,heap);if(m==nil)m=ref(p,0x53,heap);const double *v=(double *)(ref(m,7,heap)+0x17);if(!workspace_zoom(ref(p,0x53,heap),heap,nil)){aa=v[0];bb=v[1];cc=v[4];dd=v[5];xx=v[12];yy=v[13];}}
  else if(id>=CFG(3,0x141b)&&id<=CFG(4,0x141f)){const double *v=(double *)(ref(p,0x4f,heap)+7);xx=v[0];yy=v[1];}
  double na=aa*a+cc*b,nb=bb*a+dd*b,nc=aa*c+cc*d,nd=bb*c+dd*d,nx=aa*x+cc*y+xx,ny=bb*x+dd*y+yy;
  a=na;b=nb;c=nc;d=nd;x=nx;y=ny;root=p;p=ref(p,0x1f,heap);
 }
 if(p!=nil)return 0;

 P view=ref(root,CFG(12,0x2b),heap),config=ref(view,CFG(10,0x5b),heap),physical=ref(config,CFG(11,0xb),heap);
 double fw=*(double *)(physical+CFG(13,15)),fh=*(double *)(physical+CFG(14,31));
 double *v=(double *)(data+0x17),*r=(double *)(bounds+7),ratio=v[0];
 double fx=a*r[0]+c*r[1]+x,fy=b*r[0]+d*r[1]+y;
 /* Keep layout and crop in the same resting viewport coordinates used by
  * the material. Non-workspace transforms retain their existing geometry. */
 double sx=a/ratio,sy=d/ratio;
 if(b!=0||c!=0||sx<0.5||sx>2||sy<0.5||sy>2)sx=sy=1;
 int ix=(int)(fx+0.5),iy=(int)(fy+0.5),w=(int)((r[2]-r[0])*ratio*sx+0.5),h=(int)((r[3]-r[1])*ratio*sy+0.5);
 if(ix<0||iy<0||w<=0||h<=0||ix+w>fw||iy+h>fh||fw>16383||fh>16383)return 0;
 /* Native raster dispatch consumes the private material-stage marker. */
 *view_id=id|((P)kind<<29);
 double epsilon=(load(parent+SLOT(HG_CAPTURE_ROUND_SLOT,-0x90))&16)?-0.001:0.001;
 r[0]=META_BASE+ix+0.5+w/131072.0;r[1]=META_BASE+iy+0.5+h/131072.0;
 r[2]=r[0]+(fw+epsilon)/ratio;r[3]=r[1]+(fh+epsilon)/ratio;
 for(unsigned i=0;i<16;i++)v[i]=(i%5==0)?1:0;
 dirty_tree(source,heap,nil);dirty(root,nil);*(P *)(fp-8)=root;
 return kind;
}
__attribute__((section(".capture_root"),noinline))
static void refresh_scene(P fp,P heap,P nil){
 if(load(fp+8)!=(P)async_scene_return)return;
 P parent=load(fp),bounds=load(parent+SLOT(HG_CAPTURE_BOUNDS_SLOT,-0x88));
 if(*(double *)(bounds+7)<META_BASE)return;
 P source=load(parent+SLOT(HG_CAPTURE_LAYER_SLOT,-0x80));dirty_tree(source,heap,nil);
 for(unsigned i=0;i<80&&source!=nil;i++){dirty(source,nil);source=ref(source,0x1f,heap);}
}
__attribute__((section(".capture_small"),noinline))
static void mark_image(P image,P bounds,P heap){
 double l=*(double *)(bounds+7),t=*(double *)(bounds+15);
 if(l<META_BASE||t<META_BASE)return;
 unsigned il=(unsigned)l,it=(unsigned)t,w=(unsigned)((l-il-0.5)*131072.0+0.5),h=(unsigned)((t-it-0.5)*131072.0+0.5),x=il-(unsigned)META_BASE,y=it-(unsigned)META_BASE;
 if(x>4095||y>4095||!w||!h)return;
 unsigned fw=(unsigned)load(image+15),fh=(unsigned)load(image+23);P native=ref(image,7,heap);
 /* The shared native wrapper uses its valid 30-bit header hash for the full
  * framebuffer size. Image padding carries a tagged Smi with the crop origin;
  * public width/height retain the target size and Image identity stays intact. */
 *(uint32_t *)(native+3)=(1u<<28)|fw|(fh<<14);
 *(uint32_t *)(image+11)=((0x15u<<24)|(x<<12)|y)*2;
 *(P *)(image+15)=w;*(P *)(image+23)=h;
}
__attribute__((section(".capture_small"),noinline))
static void crop_image(P image,P bytes,P heap,P nil){
 unsigned mark=*(uint32_t *)(image+11)>>1;
 if((mark>>24)!=0x15||bytes==nil)return;
 unsigned x=(mark>>12)&4095,y=mark&4095,size=*(uint32_t *)(ref(image,7,heap)+3),fw=size&16383;
 P w=load(image+15),h=load(image+23),n=w*h*4;
 if((*(uint32_t *)(bytes+0x13)>>1)==n)return;
 uint32_t *pixels=cid(bytes)==0x73?(uint32_t *)(bytes+0x17):*(uint32_t **)(bytes+7);
 for(P row=0;row<h;row++)for(P col=0;col<w;col++)pixels[row*w+col]=pixels[(y+row)*fw+x+col];
 *(uint32_t *)(bytes+0x13)=(uint32_t)(n*2);
}
__attribute__((section(".capture_meta")))
void capture_dispatch(P *r){
 P mode=r[15],fp=r[28],heap=r[27]<<32,nil=r[21];
 if(mode==7)r[15]=workspace_zoom(ref(r[1],0x53,heap),heap,nil);
 else if(mode==8)r[15]=background_image(r[1],r[2],heap,nil);
 else if(mode==0)r[15]=full_scene(fp,r[2],load(fp-16),heap,nil);
 else if(mode==1)mark_image(r[0],load(fp+SLOT(HG_CAPTURE_BOUNDS_SLOT,-0x88)),heap);
 else if(mode==2){P image=load(fp-16),bytes=r[1];crop_image(image,bytes,heap,nil);*(P *)(fp-0x18)=bytes;*(P *)(fp-0x30)=load(image+15);*(P *)(fp-0x28)=load(image+23);}
 else if(mode==3)crop_image(load(fp+SLOT(9,-0x120)),r[0],heap,nil);
 else if(mode==4){unsigned mark=*(uint32_t *)(r[1]+11);if((mark>>25)==0x15)r[2]=((P)mark<<32)|(uint32_t)r[2];}
 else if(mode==5){*(uint32_t *)(r[3]+11)=(uint32_t)(r[1]>>32);r[1]=(uint32_t)r[1];}
 else if(mode==6)refresh_scene(fp,heap,nil);
}
