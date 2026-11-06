#include "page_guard.cpp"
#include <cstdlib>
int main(){
 size_t page=pageSize();auto*p=(unsigned char*)mmap(nullptr,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED)return 1;
 memset(p,0x5a,page*3);gOriginalMadvise.store(madvise);
 if(!addProtectedPage((uintptr_t)p+page)||guardedMadvise(p,page*3,MADV_DONTNEED))return 2;
 for(size_t i=0;i<page*3;i++)if(p[i]!=(i>=page&&i<page*2?0x5a:0))return 3;
 if(mprotect(p,page*3,PROT_READ|PROT_EXEC))return 4;
 if(queryProtection((uintptr_t)p+page)!=(PROT_READ|PROT_EXEC))return 5;
 if(mprotect(p,page*3,PROT_READ))return 6;
 void*slot_target=(void*)0x1234,*old_value=nullptr;
 if(mprotect(p,page,PROT_READ|PROT_WRITE))return 7;
 *(void**)p=slot_target;
 if(mprotect(p,page,PROT_READ))return 8;
 if(!patchMadviseSlot((uintptr_t)p,&old_value)||old_value!=slot_target||*(void**)p!=(void*)guardedMadvise)return 9;
 if(!restoreMadviseSlot((uintptr_t)p,old_value)||*(void**)p!=slot_target)return 10;
 munmap(p,page*3);puts("PASS exact owned-page preservation; neighboring pages reclaimed; RX query; madvise slot patch and rollback");return 0;
}
