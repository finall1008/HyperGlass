#define _GNU_SOURCE
#include "runtime.h"
#include "page_guard.h"
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef int (*Hook)(void *, void *, void **);
typedef int (*Unhook)(void *);
typedef struct {
  uint32_t version;
  Hook hook;
  Unhook unhook;
} NativeAPI;
typedef void (*Loaded)(const char *, void *);
#define LOG(...)                                                               \
  __android_log_print(ANDROID_LOG_INFO, "HyperGlass", __VA_ARGS__)
extern const unsigned char dynamic_start[], dynamic_end[];
static Hook install_hook;
static Unhook remove_hook;
static atomic_int started, park_request, handler_active;
static int park_signal;
struct slot {
  atomic_int tid, parked;
};
static struct slot slots[2048];
static atomic_int slot_count;
static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static FILE *(*real_fopen)(const char *, const char *);

static int launcher(void) {
  char name[64] = {0};
  int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline",
                        O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return 0;
  ssize_t n = syscall(SYS_read, fd, name, sizeof(name) - 1);
  syscall(SYS_close, fd);
  return n > 0 && !strcmp(name, "com.miui.home");
}
static int spawner(void) {
  char path[128] = {0};
  ssize_t n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", path,
                      sizeof(path) - 1);
  return n > 0 && !strcmp(path, "/system_ext/bin/hyos_spawner");
}
static int resource(const char *path) {
  return path && !strcmp(path, "/product/media/theme/default/dynamicicons");
}
static int dynamic_fd(void) {
  int fd = (int)syscall(SYS_memfd_create, "hyperglass-dynamicicons", 3);
  if (fd < 0)
    return -1;
  size_t len = (size_t)(dynamic_end - dynamic_start), off = 0;
  while (off < len) {
    ssize_t n = syscall(SYS_write, fd, dynamic_start + off, len - off);
    if (n <= 0) {
      syscall(SYS_close, fd);
      return -1;
    }
    off += (size_t)n;
  }
  syscall(SYS_fcntl, fd, F_ADD_SEALS,
          F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
  syscall(SYS_lseek, fd, 0, SEEK_SET);
  return fd;
}
static int needs_mode(int flags) {
  return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE);
}
static int open_hook(const char *path, int flags, ...) {
  int mode = 0;
  if (needs_mode(flags)) {
    va_list a;
    va_start(a, flags);
    mode = va_arg(a, int);
    va_end(a);
  }
  if (resource(path) && (flags & O_ACCMODE) == O_RDONLY) {
    int fd = dynamic_fd();
    if (fd >= 0)
      return fd;
  }
  return real_open(path, flags, mode);
}
static int openat_hook(int dir, const char *path, int flags, ...) {
  int mode = 0;
  if (needs_mode(flags)) {
    va_list a;
    va_start(a, flags);
    mode = va_arg(a, int);
    va_end(a);
  }
  if (resource(path) && (flags & O_ACCMODE) == O_RDONLY) {
    int fd = dynamic_fd();
    if (fd >= 0)
      return fd;
  }
  return real_openat(dir, path, flags, mode);
}
static FILE *fopen_hook(const char *path, const char *mode) {
  if (resource(path) && mode && mode[0] == 'r' && !strchr(mode, '+')) {
    int fd = dynamic_fd();
    if (fd >= 0) {
      FILE *f = fdopen(fd, mode);
      if (f)
        return f;
      syscall(SYS_close, fd);
    }
  }
  return real_fopen(path, mode);
}
int hg_install_native_hook(void *target,void *replacement,void **original){
 if(!install_hook||!target||!replacement||!original||!hg_protect_range((uintptr_t)target,32))return -1;
 *original=0;
 int rc=install_hook(target,replacement,original);
 if(rc||!*original){if(!rc&&remove_hook)remove_hook(target);*original=0;return -1;}
 return 0;
}
int hg_remove_native_hook(void *target){
 return remove_hook&&target?remove_hook(target):-1;
}
void hg_install_resources(void) {
  static int done;
  if(done || !hg_page_guard_ready()) return;
  if (!install_hook)
    return;
  struct entry {
    const char *name;
    void *replace;
    void **backup;
    void *target;
  } entries[] = {{"open", (void *)open_hook, (void **)&real_open, 0},
                 {"openat", (void *)openat_hook, (void **)&real_openat, 0},
                 {"fopen", (void *)fopen_hook, (void **)&real_fopen, 0}};
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
    entries[i].target = dlsym(RTLD_DEFAULT, entries[i].name);
    int rc = entries[i].target
                 ? hg_install_native_hook(entries[i].target, entries[i].replace,
                                          entries[i].backup)
                 : -1;
    LOG("dynamicicons hook %s rc=%d", entries[i].name, rc);
    if (rc) {
      for (size_t j = i; j > 0; --j) {
        hg_remove_native_hook(entries[j - 1].target);
        *entries[j - 1].backup = 0;
      }
      return;
    }
  }
  done = 1;
}
static void park_handler(int sig) {
  (void)sig;
  int saved = errno, tid = (int)syscall(SYS_gettid);
  atomic_fetch_add(&handler_active, 1);
  if (atomic_load_explicit(&park_request, memory_order_acquire)) {
    int n = atomic_load_explicit(&slot_count, memory_order_acquire);
    for (int i = 0; i < n; ++i)
      if (atomic_load(&slots[i].tid) == tid) {
        atomic_store_explicit(&slots[i].parked, 1, memory_order_release);
        break;
      }
    while (atomic_load_explicit(&park_request, memory_order_acquire))
      syscall(SYS_futex, &park_request, FUTEX_WAIT_PRIVATE, 1, 0, 0, 0);
  }
  atomic_fetch_sub(&handler_active, 1);
  errno = saved;
}
static void resume_threads(void) {
  atomic_store_explicit(&park_request, 0, memory_order_release);
  syscall(SYS_futex, &park_request, FUTEX_WAKE_PRIVATE, INT32_MAX, 0, 0, 0);
  while (atomic_load(&handler_active)) {
    struct timespec delay = {0, 100000};
    syscall(SYS_nanosleep, &delay, 0);
  }
}
struct linux_dirent64 {
  uint64_t ino;
  int64_t off;
  unsigned short reclen;
  unsigned char type;
  char name[];
};
static int enumerate_threads(int add) {
  int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  char data[8192];
  int added = 0, me = (int)syscall(SYS_gettid), pid = getpid();
  for (;;) {
    long len = syscall(SYS_getdents64, fd, data, sizeof(data));
    if (len < 0) {
      syscall(SYS_close, fd);
      return -1;
    }
    if (!len)
      break;
    for (long off = 0; off < len;) {
      struct linux_dirent64 *e = (void *)(data + off);
      if (!e->reclen) {
        syscall(SYS_close, fd);
        return -1;
      }
      off += e->reclen;
      int tid = 0;
      for (char *p = e->name; *p >= '0' && *p <= '9'; ++p)
        tid = tid * 10 + *p - '0';
      if (!tid || tid == me)
        continue;
      int count = atomic_load(&slot_count), found = 0;
      for (int i = 0; i < count; ++i)
        if (atomic_load(&slots[i].tid) == tid) {
          found = 1;
          break;
        }
      if (found)
        continue;
      if (!add || count == 2048) {
        syscall(SYS_close, fd);
        return -1;
      }
      atomic_store(&slots[count].tid, tid);
      atomic_store(&slots[count].parked, 0);
      atomic_store_explicit(&slot_count, count + 1, memory_order_release);
      if (syscall(SYS_tgkill, pid, tid, park_signal) < 0) {
        if (errno != ESRCH) {
          syscall(SYS_close, fd);
          return -1;
        }
        atomic_store(&slots[count].tid, 0);
        atomic_store(&slots[count].parked, 1);
      }
      ++added;
    }
  }
  syscall(SYS_close, fd);
  return added;
}
static int stop_threads(void) {
  atomic_store(&slot_count, 0);
  atomic_store_explicit(&park_request, 1, memory_order_release);
  if (enumerate_threads(1) < 0) {
    resume_threads();
    return 0;
  }
  for (int attempt = 0; attempt < 200; ++attempt) {
    int all = 1, n = atomic_load(&slot_count);
    for (int i = 0; i < n; ++i)
      if (!atomic_load_explicit(&slots[i].parked, memory_order_acquire)) {
        int tid = atomic_load(&slots[i].tid);
        if (syscall(SYS_tgkill, getpid(), tid, 0) < 0 && errno == ESRCH)
          atomic_store(&slots[i].parked, 1);
        else
          all = 0;
      }
    if (all) {
      int added = enumerate_threads(1);
      if (!added)
        return 1;
      if (added < 0)
        break;
    }
    struct timespec delay = {0, 500000};
    syscall(SYS_nanosleep, &delay, 0);
  }
  resume_threads();
  return 0;
}
static int same(const unsigned char *a, const unsigned char *b, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i])
      return 0;
  return 1;
}
static void copy(unsigned char *a, const unsigned char *b, size_t n) {
  for (size_t i = 0; i < n; ++i)
    ((volatile unsigned char *)a)[i] = b[i];
}
struct code_page {uintptr_t address; int protection;};
static int page_permissions(struct code_page *pages,size_t count,size_t page){
 FILE*f=fopen("/proc/self/maps","re");if(!f)return 0;
 char line[1536],perm[8];unsigned long lo,hi;
 while(fgets(line,sizeof(line),f)){
  if(sscanf(line,"%lx-%lx %7s",&lo,&hi,perm)!=3)continue;
  int protection=(perm[0]=='r'?PROT_READ:0)|(perm[1]=='w'?PROT_WRITE:0)|(perm[2]=='x'?PROT_EXEC:0);
  for(size_t i=0;i<count;i++)if(pages[i].address>=lo&&pages[i].address+page<=hi)pages[i].protection=protection;
 }
 fclose(f);
 for(size_t i=0;i<count;i++)if(!(pages[i].protection&PROT_EXEC))return 0;
 return 1;
}
int hg_apply_batch(const struct hg_patch *patches, size_t count) {
 if(!count||count>192)return -1;
 struct code_page pages[256]={0};size_t num=0,page=(size_t)sysconf(_SC_PAGESIZE);
 unsigned char saved[192][256];
 for(size_t i=0;i<count;i++){
  const struct hg_patch*p=patches+i;
  if(!p->address||!p->size||p->size>256||p->size%4||p->address>UINTPTR_MAX-p->size)return -1;
  uintptr_t last=(p->address+p->size-1)&~(page-1);
  for(uintptr_t at=p->address&~(page-1);at<=last;at+=page){
   size_t j=0;for(;j<num&&pages[j].address!=at;j++){}
   if(j==num){if(num==256)return -1;pages[num++].address=at;}
  }
 }
 // HYOS may publish an ELF header before its code pages are mapped or readable.
 if(!page_permissions(pages,num,page))return -6;
 int readable=1;for(size_t i=0;i<num;i++)if(!(pages[i].protection&PROT_READ))readable=0;
 if(readable){
  int active=1;for(size_t i=0;i<count;i++)if(!same((void*)patches[i].address,patches[i].bytes,patches[i].size)){active=0;break;}
  if(active)return 0;
 }
 for(size_t i=0;i<num;i++)if(!hg_protect_range(pages[i].address,page))return -5;
 if(!stop_threads())return -2;
 size_t writable=0;
 for(;writable<num;writable++)if(syscall(SYS_mprotect,(void*)pages[writable].address,page,PROT_READ|PROT_WRITE|PROT_EXEC))break;
 int ok=writable==num,result=-3,wrote=0;
 if(ok){
  for(size_t i=0;i<count;i++){
   const struct hg_patch*p=patches+i;
   if(!same((void*)p->address,p->bytes,p->size)&&!same((void*)p->address,p->original,p->size)){ok=0;result=-4;break;}
   copy(saved[i],(void*)p->address,p->size);
  }
  if(ok){
   for(size_t i=0;i<count;i++)copy((void*)patches[i].address,patches[i].bytes,patches[i].size);
   wrote=1;
   for(size_t i=0;i<count;i++)if(!same((void*)patches[i].address,patches[i].bytes,patches[i].size))ok=0;
   if(!ok)for(size_t i=0;i<count;i++)copy((void*)patches[i].address,saved[i],patches[i].size);
   for(size_t i=0;i<count;i++)__builtin___clear_cache((char*)patches[i].address,(char*)(patches[i].address+patches[i].size));
  }
 }
 for(size_t i=0;i<writable;i++)if(syscall(SYS_mprotect,(void*)pages[i].address,page,wrote?PROT_READ|PROT_EXEC:pages[i].protection))ok=0;
 resume_threads();return ok?1:result;
}
static void *worker(void *arg) {
  (void)arg;
  pthread_setname_np(pthread_self(), "hyperglass");
  for (;;) {
    unsigned ready = hg_runtime_tick("watch");
    if (hg_runtime_complete())
      break;
    usleep(ready ? 250000 : 10000);
  }
  return 0;
}
static void start(void) {
  if (!launcher() || atomic_exchange(&started, 1))
    return;
  struct sigaction action = {0}, previous = {0};
  action.sa_handler = park_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  park_signal = SIGRTMAX - 4;
  if (sigaction(park_signal, 0, &previous) || previous.sa_handler != SIG_DFL ||
      sigaction(park_signal, &action, 0)) {
    LOG("Thread parking unavailable; adapters remain inactive");
    return;
  }
  hg_runtime_tick("native-init");
  if (hg_runtime_complete())
    return;
  pthread_t thread;
  if (pthread_create(&thread, 0, worker, 0))
    LOG("Worker creation failed");
  else
    pthread_detach(thread);
}
static void loaded(const char *name, void *handle) {
  (void)handle;
  start();
  if (!launcher() || !name)
    return;
  const char *leaf = strrchr(name, '/');
  leaf = leaf ? leaf + 1 : name;
  if (!strcmp(leaf, "libapp.so") || !strcmp(leaf, "librust_maml_sdk.so") ||
      !strcmp(leaf, "libhyper_os_flutter.so") ||
      !strcmp(leaf, "libhyper_surface_context.so"))
    hg_runtime_tick("library-loaded");
}
__attribute__((visibility("default"), used)) Loaded
native_init(const NativeAPI *api) {
  if (!api || !api->hook || !api->unhook) {
    LOG("Native framework API unavailable");
    return 0;
  }
  install_hook = api->hook;
  remove_hook = api->unhook;
  int is_launcher = launcher(), is_spawner = spawner();
  LOG("native_init pid=%d API=%u launcher=%d spawner=%d", getpid(),
      api->version, is_launcher, is_spawner);
  if (!is_launcher && !is_spawner)
    return 0;
  start();
  return loaded;
}
