#include "resolver.hpp"
#include "adaptive_profiles.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void hex(FILE*f,const uint8_t*bytes,size_t size){for(size_t i=0;i<size;i++)fprintf(f,"%02x",bytes[i]);}
int main(int argc,char**argv){
 if(argc<3)return 2;unsigned index=unsigned(strtoul(argv[1],nullptr,10));if(index>=3)return 2;
 const auto&program=*hg_programs[index];hg::Resolved result;uintptr_t live=0x40000000;
 bool ok=hg::resolve_program(argv[2],live,program,result,argc>3);
 printf("image=%s ready=%u patches=%zu anchors=%zu shadow=%llx diagnostic=%s\n",program.image,result.ready,result.patches.size(),result.addresses.size(),(unsigned long long)result.shadow,result.diagnostic.c_str());
 for(size_t i=0;i<result.addresses.size();i++)if(!result.addresses[i])printf("UNRESOLVED source=%x owner=%s\n",program.anchors[i].source,program.anchors[i].owner);
 if(argc>3&&result.addresses.size()==program.anchor_count){
  FILE*f=fopen(argv[3],"w");if(!f)return 3;
  fprintf(f,"{\"base\":%llu,\"shadow\":%llu,\"ready\":%u,\"anchors\":[",(unsigned long long)live,(unsigned long long)result.shadow,result.ready);
  for(size_t i=0;i<program.anchor_count;i++)fprintf(f,"%s[%u,%u]",i?",":"",program.anchors[i].source,result.addresses[i]);
  fprintf(f,"],\"patches\":[");
  for(size_t i=0;i<result.patches.size();i++){const auto&p=result.patches[i];fprintf(f,"%s[%llu,\"",i?",":"",(unsigned long long)p.address);hex(f,p.bytes,p.size);fprintf(f,"\",%u]",p.group);}
  fprintf(f,"],\"adapters\":[");bool first=true;
  for(size_t i=0;i<program.unit_count;i++){
   const auto&u=program.units[i];if(!u.shadow||!(result.ready&u.group))continue;
   if(program.widget_wrap&&i==program.widget_wrap->unit&&!(result.adapted_widget_paths&HG_WIDGET_COMPOUND))continue;
   if(u.guard>=0&&(result.addresses[size_t(u.guard)]!=0)!=u.when_guard_matches)continue;
   fprintf(f,"%s[%u,%llu,\"",first?"":",",u.source,(unsigned long long)(hg::adapter_address(result,u.source)));hex(f,reinterpret_cast<const uint8_t*>(hg::adapter_address(result,u.source)),u.size);fprintf(f,"\",%u]",u.group);first=false;
  }
  fprintf(f,"],\"adapted_widget_paths\":%u}\n",result.adapted_widget_paths);fclose(f);
 }
 uint32_t expected=getenv("HG_EXPECT_READY")?uint32_t(strtoul(getenv("HG_EXPECT_READY"),nullptr,10)):(index==0?(39|HG_WIDGET_MONOCHROME):index==1?2:8);return (ok||expected==0)&&result.ready==expected?0:1;
}
