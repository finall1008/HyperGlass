#include "module.c"
unsigned hg_runtime_tick(const char *phase){(void)phase;return 0;}
bool hg_runtime_complete(void){return true;}
bool hg_page_guard_ready(void){return true;}
bool hg_protect_range(uintptr_t at,size_t n){return at&&n;}
#include <sys/stat.h>
static atomic_int finish, blocked_ready;
static atomic_ulong calls;
static int (*test_function)(void);
static void *draw_loop(void *unused) {
  (void)unused;
  while (!atomic_load(&finish)) {
    int n = test_function();
    if (n != 1 && n != 2)
      abort();
    atomic_fetch_add(&calls, 1);
  }
  return 0;
}
static void *blocked_loop(void *unused) {
  (void)unused;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, park_signal);
  pthread_sigmask(SIG_BLOCK, &set, 0);
  atomic_store(&blocked_ready, 1);
  while (!atomic_load(&finish))
    usleep(1000);
  return 0;
}
int main(void) {
  _Static_assert(ATOMIC_INT_LOCK_FREE == 2,
                 "signal handler requires lock-free integer atomics");
  int a = dynamic_fd(), b = dynamic_fd();
  if (a < 0 || b < 0)
    return 1;
  unsigned char first[16], second[16];
  struct stat st;
  if (read(a, first, 16) != 16 || read(b, second, 16) != 16 ||
      memcmp(first, second, 16) || fstat(a, &st) ||
      st.st_size != dynamic_end - dynamic_start)
    return 2;
  if (write(a, "x", 1) >= 0 || errno != EPERM)
    return 3;
  close(a);
  close(b);
  if (!resource("/product/media/theme/default/dynamicicons") ||
      resource("/product/media/theme/default/icons") || launcher() ||
      spawner())
    return 4;
  NativeAPI incomplete_api = {.version = 102, .hook = (Hook)1, .unhook = 0};
  if (native_init(&incomplete_api) != 0)
    return 4;
  park_signal = SIGRTMAX - 4;
  struct sigaction sa = {0};
  sa.sa_handler = park_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(park_signal, &sa, 0))
    return 5;
  size_t size = 16384;
  unsigned char *base =
      mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    return 6;
  Elf64_Ehdr *eh = (void *)base;
  memcpy(eh->e_ident, ELFMAG, SELFMAG);
  eh->e_machine = EM_AARCH64;
  eh->e_phnum = 1;
  eh->e_phoff = sizeof(*eh);
  Elf64_Phdr *ph = (void *)(base + eh->e_phoff);
  ph->p_type = PT_LOAD;
  ph->p_flags = PF_R | PF_X;
  ph->p_memsz = size;
  ph->p_filesz = size;
  const unsigned char one[] = {0x20, 0, 0x80, 0x52, 0xc0, 3, 0x5f, 0xd6},
                      two[] = {0x40, 0, 0x80, 0x52, 0xc0, 3, 0x5f, 0xd6};
  memcpy(base + 8192, one, 8);
  __builtin___clear_cache((char *)base + 8192, (char *)base + 8200);
  if (mprotect(base, size, PROT_READ | PROT_EXEC))
    return 7;
  test_function = (void *)(base + 8192);
  pthread_t threads[8];
  for (int i = 0; i < 8; ++i)
    if (pthread_create(&threads[i], 0, draw_loop, 0))
      return 8;
  struct hg_patch patch = {.address = (uintptr_t)base+8192, .size = 8};
  for (int i = 0; i < 100; ++i) {
    memcpy(patch.bytes, i % 2 ? one : two, 8);
    memcpy(patch.original, i % 2 ? two : one, 8);
    int rc = hg_apply_batch(&patch,1);
    if (rc != 1) {
      printf("FAIL concurrent patch %d rc=%d\n", i, rc);
      return 9;
    }
  }
  atomic_store(&finish, 1);
  for (int i = 0; i < 8; ++i)
    pthread_join(threads[i], 0);
  if (!atomic_load(&calls))
    return 10;
  atomic_store(&finish, 0);
  pthread_t blocked;
  if (pthread_create(&blocked, 0, blocked_loop, 0))
    return 11;
  while (!atomic_load(&blocked_ready))
    usleep(1000);
  memcpy(patch.bytes, two, 8);
  memcpy(patch.original,one,8);
  if (hg_apply_batch(&patch,1) != -2 || memcmp(base + 8192, one, 8))
    return 12;
  atomic_store(&finish, 1);
  pthread_join(blocked, 0);
  memcpy(patch.original,two,8);
  if(hg_apply_batch(&patch,1)!=-4) return 13;
  // A published ELF with inaccessible code must be deferred without a read.
  if(mprotect(base+8192,4096,PROT_NONE)||hg_apply_batch(&patch,1)!=-6)return 14;
  // Execute-only code can be verified and patched after parking, then remains RX.
  memcpy(patch.original,one,8);memcpy(patch.bytes,two,8);
  if(mprotect(base+8192,4096,PROT_EXEC)||hg_apply_batch(&patch,1)!=1||test_function()!=2)return 15;
  printf("PASS: independent sealed dynamic resources, target scope, 100 ARM64 "
         "patch batches with eight executing threads (%lu calls), "
         "blocked-thread deferral, unreadable mapping deferral, execute-only patching\n",
         atomic_load(&calls));
  munmap(base, size);
  return 0;
}
