#include "elf_image.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
extern "C" {
#include "xz.h"
}
#include <limits>
namespace hg {
static bool region(size_t total,uint64_t offset,uint64_t size){return offset<=total&&size<=total-offset;}
static bool header(const std::vector<uint8_t>&data,const Elf64_Ehdr **out){
 if(data.size()<sizeof(Elf64_Ehdr))return false;
 const auto *eh=reinterpret_cast<const Elf64_Ehdr*>(data.data());
 if(memcmp(eh->e_ident,ELFMAG,SELFMAG)||eh->e_ident[EI_CLASS]!=ELFCLASS64||eh->e_ident[EI_DATA]!=ELFDATA2LSB||eh->e_machine!=EM_AARCH64||(eh->e_shnum&&eh->e_shentsize!=sizeof(Elf64_Shdr))||!region(data.size(),eh->e_shoff,uint64_t(eh->e_shnum)*sizeof(Elf64_Shdr)))return false;
 *out=eh;return true;
}
bool ElfImage::parse_symbols(const std::vector<uint8_t>&data,std::string&error){
 const Elf64_Ehdr *eh=nullptr;if(!header(data,&eh)){error="invalid symbol ELF";return false;}
 const auto *sections=reinterpret_cast<const Elf64_Shdr*>(data.data()+eh->e_shoff);
 for(unsigned i=0;i<eh->e_shnum;i++){
  const auto &s=sections[i];if((s.sh_type!=SHT_SYMTAB&&s.sh_type!=SHT_DYNSYM)||s.sh_link>=eh->e_shnum||s.sh_entsize!=sizeof(Elf64_Sym)||!region(data.size(),s.sh_offset,s.sh_size))continue;
  const auto &strings=sections[s.sh_link];if(!region(data.size(),strings.sh_offset,strings.sh_size))continue;
  for(size_t n=0;n<s.sh_size/sizeof(Elf64_Sym);n++){
   Elf64_Sym symbol{};memcpy(&symbol,data.data()+s.sh_offset+n*sizeof(symbol),sizeof(symbol));
   if(!symbol.st_value||symbol.st_name>=strings.sh_size)continue;
   const char *name=reinterpret_cast<const char*>(data.data()+strings.sh_offset+symbol.st_name);size_t limit=strings.sh_size-symbol.st_name;
   const void *end=memchr(name,0,limit);if(!end||end==name)continue;
   symbols_.push_back({symbol.st_value,symbol.st_size,std::string(name,static_cast<const char*>(end)-name)});
  }
 }
 return true;
}
static uint16_t u16(const uint8_t*p){uint16_t n;memcpy(&n,p,2);return n;}
static uint32_t u32(const uint8_t*p){uint32_t n;memcpy(&n,p,4);return n;}
static FILE*open_image(const char*path,uint64_t&offset,size_t&size,std::string&error){
 std::string name=path;size_t bang=name.find('!');std::string entry;
 if(bang!=std::string::npos){entry=name.substr(bang+1);if(!entry.empty()&&entry[0]=='/')entry.erase(0,1);name.resize(bang);}
 FILE*f=fopen(name.c_str(),"rb");if(!f){error="cannot read mapped image";return nullptr;}
 auto fail=[&](const char*message)->FILE*{error=message;fclose(f);return nullptr;};
 if(fseek(f,0,SEEK_END)||ftell(f)<0)return fail("image length unavailable");
 uint64_t total=uint64_t(ftell(f));offset=0;size=size_t(total);
 if(!entry.empty()){
  size_t tail_size=size_t(std::min<uint64_t>(total,65557));std::vector<uint8_t>tail(tail_size);
  if(fseek(f,long(total-tail_size),SEEK_SET)||fread(tail.data(),1,tail_size,f)!=tail_size)return fail("APK directory unavailable");
  const uint8_t*eocd=nullptr;
  for(size_t pos=tail_size;pos>=22;pos--)if(u32(tail.data()+pos-22)==0x06054b50&&pos+u16(tail.data()+pos-2)==tail_size){eocd=tail.data()+pos-22;break;}
  if(!eocd)return fail("APK directory missing");
  uint32_t bytes=u32(eocd+12),at=u32(eocd+16);if(bytes>8*1024*1024||!region(size_t(total),at,bytes))return fail("APK directory outside bounds");
  std::vector<uint8_t>directory(bytes);if(fseek(f,at,SEEK_SET)||fread(directory.data(),1,bytes,f)!=bytes)return fail("APK directory unreadable");
  bool found=false;
  for(size_t pos=0;pos+46<=directory.size();){
   const uint8_t*d=directory.data()+pos;if(u32(d)!=0x02014b50)return fail("invalid APK directory record");
   size_t len=u16(d+28),extra=u16(d+30),comment=u16(d+32),next=pos+46+len+extra+comment;
   if(next>directory.size())return fail("truncated APK directory");
   if(entry.size()==len&&!memcmp(d+46,entry.data(),len)){
    if(found||u16(d+10)||u16(d+8)&1||u32(d+20)!=u32(d+24))return fail("mapped APK library must be unique and stored");
    uint8_t local[30];uint32_t local_offset=u32(d+42);
    if(!region(size_t(total),local_offset,sizeof(local))||fseek(f,local_offset,SEEK_SET)||fread(local,1,sizeof(local),f)!=sizeof(local)||u32(local)!=0x04034b50)return fail("invalid APK local entry");
    offset=uint64_t(local_offset)+30+u16(local+26)+u16(local+28);size=u32(d+24);found=true;
   }
   pos=next;
  }
  if(!found)return fail("mapped library missing from APK");
 }
 if(size<64||size>128*1024*1024||!region(size_t(total),offset,size))return fail("image size outside bounds");
 if(fseek(f,long(offset),SEEK_SET))return fail("image seek failed");return f;
}
bool image_layout(const char*path,std::vector<Elf64_Phdr>&segments,uint64_t&offset,std::string&error){
 size_t size=0;FILE*f=open_image(path,offset,size,error);if(!f)return false;
 Elf64_Ehdr eh{};bool ok=fread(&eh,1,sizeof(eh),f)==sizeof(eh)&&!memcmp(eh.e_ident,ELFMAG,SELFMAG)&&eh.e_machine==EM_AARCH64&&eh.e_phnum&&eh.e_phnum<=64&&eh.e_phentsize==sizeof(Elf64_Phdr)&&region(size,eh.e_phoff,size_t(eh.e_phnum)*sizeof(Elf64_Phdr));
 if(ok){segments.resize(eh.e_phnum);ok=!fseek(f,long(offset+eh.e_phoff),SEEK_SET)&&fread(segments.data(),sizeof(Elf64_Phdr),eh.e_phnum,f)==eh.e_phnum;}
 fclose(f);if(!ok)error="invalid mapped image headers";return ok;
}
bool ElfImage::load(const char *path,std::string&error){
 uint64_t offset=0;size_t size=0;FILE*file=open_image(path,offset,size,error);if(!file)return false;
 data_.resize(size);bool read=fread(data_.data(),1,data_.size(),file)==data_.size();fclose(file);
 const Elf64_Ehdr *eh=nullptr;if(!read||!header(data_,&eh)||eh->e_phentsize!=sizeof(Elf64_Phdr)||eh->e_phnum>64||!region(data_.size(),eh->e_phoff,uint64_t(eh->e_phnum)*sizeof(Elf64_Phdr))){error="invalid arm64 image";return false;}
 for(unsigned i=0;i<eh->e_phnum;i++){
  Elf64_Phdr ph{};memcpy(&ph,data_.data()+eh->e_phoff+i*sizeof(ph),sizeof(ph));
  if(ph.p_type!=PT_LOAD)continue;
  if(!region(data_.size(),ph.p_offset,ph.p_filesz)||ph.p_vaddr>0x10000000||ph.p_memsz>0x10000000-ph.p_vaddr){error="invalid load range";return false;}
  segments_.push_back(ph);span_=std::max<uint64_t>(span_,ph.p_vaddr+ph.p_memsz);
 }
 if(segments_.empty()){error="no image load ranges";return false;}
 parse_symbols(data_,error);
 const auto *sections=reinterpret_cast<const Elf64_Shdr*>(data_.data()+eh->e_shoff);
 if(eh->e_shstrndx<eh->e_shnum){
  const auto &strings=sections[eh->e_shstrndx];
  if(region(data_.size(),strings.sh_offset,strings.sh_size))for(unsigned i=0;i<eh->e_shnum;i++){
   const auto &s=sections[i];if(s.sh_name>=strings.sh_size||!region(data_.size(),s.sh_offset,s.sh_size))continue;
   const char *name=reinterpret_cast<const char*>(data_.data()+strings.sh_offset+s.sh_name);
   if(!memchr(name,0,strings.sh_size-s.sh_name)||strcmp(name,".gnu_debugdata"))continue;
   static std::once_flag checks;
   std::call_once(checks,[]{xz_crc32_init();xz_crc64_init();});
   for(size_t cap=4*1024*1024;cap<=64*1024*1024;cap*=2){
    std::vector<uint8_t> expanded(cap);xz_buf buffer{};
    buffer.in=data_.data()+s.sh_offset;buffer.in_size=s.sh_size;buffer.out=expanded.data();buffer.out_size=cap;
    xz_dec *decoder=xz_dec_init(XZ_SINGLE,0);if(!decoder){error="debug decoder allocation failed";break;}
    xz_ret rc=xz_dec_run(decoder,&buffer);xz_dec_end(decoder);
    if(rc==XZ_STREAM_END){expanded.resize(buffer.out_pos);parse_symbols(expanded,error);break;}
    if(rc!=XZ_BUF_ERROR){error="debug symbols could not be decoded";break;}
   }
   break;
  }
 }
 std::sort(symbols_.begin(),symbols_.end(),[](const Symbol&a,const Symbol&b){return a.value<b.value||(a.value==b.value&&a.name<b.name);});
 symbols_.erase(std::unique(symbols_.begin(),symbols_.end(),[](const Symbol&a,const Symbol&b){return a.value==b.value&&a.name==b.name;}),symbols_.end());
 return true;
}
const uint8_t *ElfImage::at(uint64_t address,size_t size)const{
 for(const auto&p:segments_)if(address>=p.p_vaddr&&address-p.p_vaddr<=p.p_filesz&&size<=p.p_filesz-(address-p.p_vaddr))return data_.data()+p.p_offset+address-p.p_vaddr;
 return nullptr;
}
bool ElfImage::executable(uint64_t address,size_t size)const{
 for(const auto&p:segments_)if((p.p_flags&PF_X)&&address>=p.p_vaddr&&address-p.p_vaddr<=p.p_memsz&&size<=p.p_memsz-(address-p.p_vaddr))return true;
 return false;
}
uint32_t ElfImage::word(uint64_t address)const{uint32_t value=0;const auto*p=at(address,4);if(p)memcpy(&value,p,4);return value;}
std::vector<Symbol> ElfImage::named(const char*name)const{std::vector<Symbol>out;for(const auto&s:symbols_)if(s.name==name)out.push_back(s);return out;}
uint64_t ElfImage::symbol_end(const Symbol&s)const{
 uint64_t end=s.value+s.size;for(const auto&next:symbols_)if(next.value>s.value){end=std::max(end,next.value);break;}
 return std::min(end,span_);
}
uint64_t ElfImage::import_stub(const char *wanted)const{
 const Elf64_Ehdr *eh=nullptr;if(!header(data_,&eh))return 0;
 uint64_t symbols=0,strings=0,strings_size=0,relocations=0,relocations_size=0;
 const auto*ph=reinterpret_cast<const Elf64_Phdr*>(data_.data()+eh->e_phoff);
 for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC){
  const uint8_t*raw=at(ph[i].p_vaddr,size_t(ph[i].p_filesz));if(!raw)return 0;
  for(size_t j=0;j<ph[i].p_filesz/sizeof(Elf64_Dyn);j++){
   Elf64_Dyn entry{};memcpy(&entry,raw+j*sizeof(entry),sizeof(entry));if(entry.d_tag==DT_NULL)break;
   switch(entry.d_tag){case DT_SYMTAB:symbols=entry.d_un.d_ptr;break;case DT_STRTAB:strings=entry.d_un.d_ptr;break;case DT_STRSZ:strings_size=entry.d_un.d_val;break;case DT_JMPREL:relocations=entry.d_un.d_ptr;break;case DT_PLTRELSZ:relocations_size=entry.d_un.d_val;break;default:break;}
  }
 }
 if(!symbols||!strings||!relocations||!at(relocations,size_t(relocations_size))||!at(strings,size_t(strings_size)))return 0;
 std::vector<uint64_t>slots;
 for(size_t i=0;i<relocations_size/sizeof(Elf64_Rela);i++){
  Elf64_Rela relocation{};memcpy(&relocation,at(relocations+i*sizeof(relocation),sizeof(relocation)),sizeof(relocation));
  const uint8_t*p=at(symbols+ELF64_R_SYM(relocation.r_info)*sizeof(Elf64_Sym),sizeof(Elf64_Sym));if(!p)continue;
  Elf64_Sym symbol{};memcpy(&symbol,p,sizeof(symbol));if(symbol.st_name>=strings_size)continue;
  const char*name=reinterpret_cast<const char*>(at(strings+symbol.st_name,size_t(strings_size-symbol.st_name)));
  if(name&&memchr(name,0,size_t(strings_size-symbol.st_name))&&!strcmp(name,wanted))slots.push_back(relocation.r_offset);
 }
 std::vector<uint64_t>matches;
 for(const auto&segment:segments_)if(segment.p_flags&PF_X)for(uint64_t pc=segment.p_vaddr;pc+16<=segment.p_vaddr+segment.p_filesz;pc+=4){
  uint32_t a=word(pc),b=word(pc+4),c=word(pc+8),d=word(pc+12);
  if((a&0x9f00001f)!=0x90000010||(b&0xffc003ff)!=0xf9400211||(c&0xffc003ff)!=0x91000210||d!=0xd61f0220)continue;
  int64_t page=((a>>29)&3)|int64_t((a>>5)&0x7ffff)<<2;if(page&(1<<20))page-=1<<21;
  uint64_t slot=(pc&~uint64_t(4095))+page*4096+uint64_t((b>>10)&4095)*8;
  if(std::find(slots.begin(),slots.end(),slot)!=slots.end())matches.push_back(pc);
 }
 return matches.size()==1?matches[0]:0;
}
uint32_t instruction_mask(uint32_t w,bool frames){
 if((w&0x7c000000)==0x14000000)return 0xfc000000;
 if((w&0xff000010)==0x54000000||(w&0x7e000000)==0x34000000)return 0xff00001f;
 if((w&0x7e000000)==0x36000000)return 0xfff8001f;
 if((w&0x1f000000)==0x10000000)return 0x9f00001f;
 if((w&0x3b000000)==0x18000000)return 0xff00001f;
 unsigned rn=(w>>5)&31,rd=w&31;
 if((w&0x3b000000)==0x39000000)return frames&&(rn==15||rn==29||rn==31)?0xffffffff:0xffc003ff;
 if((w&0x3b200000)==0x38000000)return frames&&(rn==15||rn==29||rn==31)?0xffffffff:0xffe00fff;
 if((w&0xffc00000)==0x69400000)return frames&&(rn==15||rn==29||rn==31)?0xffffffff:0xffc07fff;
 if((w&0x1f800000)==0x12800000)return 0xffe0001f;
 if((w&0x1f000000)==0x11000000)return frames&&rn==rd&&(rn==15||rn==29||rn==31)?0xffffffff:0xff8003ff;
 return 0xffffffff;
}
uint64_t branch_target(uint32_t w,uint64_t pc){
 unsigned bits,shift;
 if((w&0x7c000000)==0x14000000){bits=26;shift=0;}
 else if((w&0xff000010)==0x54000000||(w&0x7e000000)==0x34000000){bits=19;shift=5;}
 else if((w&0x7e000000)==0x36000000){bits=14;shift=5;}
 else return 0;
 int64_t n=(w>>shift)&((1u<<bits)-1);if(n&(int64_t(1)<<(bits-1)))n-=int64_t(1)<<bits;return pc+n*4;
}
int64_t instruction_immediate(uint32_t w,bool *valid){
 if(valid)*valid=true;
 if((w&0x1f800000)==0x12800000){uint64_t n=uint64_t((w>>5)&65535)<<(((w>>21)&3)*16);if((w&0x60000000)==0)n=~n;if(!(w>>31))n&=0xffffffff;return static_cast<int64_t>(n);}
 if((w&0x1f000000)==0x11000000)return int64_t((w>>10)&4095)<<((w&(1<<22))?12:0);
 if((w&0x3b000000)==0x39000000)return int64_t((w>>10)&4095)<<(w>>30);
 if((w&0x3b200000)==0x38000000){int n=(w>>12)&511;return(n&256)?n-512:n;}
 if((w&0xffc00000)==0x69400000){int n=(w>>15)&127;return ((n&64)?n-128:n)*4;}
 if(valid)*valid=false;return 0;
}
}
