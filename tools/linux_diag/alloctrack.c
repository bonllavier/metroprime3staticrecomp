// LD_PRELOAD tracker: live bytes of allocations >= 16 KiB (malloc/calloc/realloc/memalign and anonymous mmap)
// grouped by an 8-frame call stack. Writes the top stacks to $ALLOCTRACK_OUT every 20 s.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define MIN_SIZE (16 * 1024)
#define FRAMES 8
#define TABLE (1 << 20)
#define SITES (1 << 14)
extern void *__libc_malloc(size_t); extern void __libc_free(void *); extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t); extern void *__libc_memalign(size_t, size_t);
struct site { void *pc[FRAMES]; int64_t live; int64_t count; };
struct rec { uintptr_t ptr; size_t size; int site; };
static struct site sites[SITES]; static struct rec table[TABLE];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static __thread int busy;
static void *(*real_mmap)(void *, size_t, int, int, int, off_t); static int (*real_munmap)(void *, size_t);
static int site_for(void **pc) {
  uint64_t h = 1469598103934665603ull; for (int i = 0; i < FRAMES; i++) h = (h ^ (uintptr_t)pc[i]) * 1099511628211ull;
  for (int i = 0; i < SITES; i++) { int j = (h + i) & (SITES - 1);
    if (sites[j].count == 0 && sites[j].live == 0 && sites[j].pc[0] == 0) { memcpy(sites[j].pc, pc, sizeof(void *) * FRAMES); return j; }
    if (!memcmp(sites[j].pc, pc, sizeof(void *) * FRAMES)) return j; }
  return -1; }
static void note_alloc(void *p, size_t n) {
  if (!p || n < MIN_SIZE || busy) return; busy = 1; void *pc[FRAMES + 2] = {0}; backtrace(pc, FRAMES + 2);
  pthread_mutex_lock(&mu); int s = site_for(pc + 2);
  if (s >= 0) { uintptr_t k = (uintptr_t)p; for (int i = 0; i < TABLE; i++) { int j = ((k >> 4) + i) & (TABLE - 1);
      if (table[j].ptr == 0 || table[j].ptr == 1) { table[j] = (struct rec){k, n, s}; sites[s].live += n; sites[s].count++; break; } } }
  pthread_mutex_unlock(&mu); busy = 0; }
static void note_free(void *p) {
  if (!p) return; uintptr_t k = (uintptr_t)p; pthread_mutex_lock(&mu);
  for (int i = 0; i < TABLE; i++) { int j = ((k >> 4) + i) & (TABLE - 1); if (table[j].ptr == 0) break;
    if (table[j].ptr == k) { sites[table[j].site].live -= table[j].size; sites[table[j].site].count--; table[j].ptr = 1; break; } }
  pthread_mutex_unlock(&mu); }
void *malloc(size_t n) { void *p = __libc_malloc(n); note_alloc(p, n); return p; }
void *calloc(size_t a, size_t b) { void *p = __libc_calloc(a, b); note_alloc(p, a * b); return p; }
void free(void *p) { note_free(p); __libc_free(p); }
void *realloc(void *o, size_t n) { note_free(o); void *p = __libc_realloc(o, n); note_alloc(p, n); return p; }
void *memalign(size_t a, size_t n) { void *p = __libc_memalign(a, n); note_alloc(p, n); return p; }
void *aligned_alloc(size_t a, size_t n) { return memalign(a, n); }
int posix_memalign(void **out, size_t a, size_t n) { void *p = __libc_memalign(a, n); if (!p) return 12; note_alloc(p, n); *out = p; return 0; }
void *mmap(void *a, size_t n, int pr, int fl, int fd, off_t o) {
  if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap"); void *p = real_mmap(a, n, pr, fl, fd, o);
  if (p != MAP_FAILED && (fl & MAP_ANONYMOUS) && n >= 256 * 1024) note_alloc(p, n); return p; }
void *mmap64(void *a, size_t n, int pr, int fl, int fd, off_t o) { return mmap(a, n, pr, fl, fd, o); }
int munmap(void *a, size_t n) { if (!real_munmap) real_munmap = dlsym(RTLD_NEXT, "munmap"); note_free(a); return real_munmap(a, n); }
static void *reporter(void *arg) { (void)arg; const char *out = getenv("ALLOCTRACK_OUT"); busy = 1;
  for (;;) { sleep(20); FILE *f = fopen(out, "w"); if (!f) continue;
    char maps[64]; snprintf(maps, sizeof maps, "/proc/%d/maps", getpid()); FILE *m = fopen(maps, "r"); char line[512];
    pthread_mutex_lock(&mu); int idx[30] = {0}; int64_t best[30] = {0};
    for (int s = 0; s < SITES; s++) for (int k = 0; k < 30; k++) if (sites[s].live > best[k]) {
        memmove(best + k + 1, best + k, sizeof(int64_t) * (29 - k)); memmove(idx + k + 1, idx + k, sizeof(int) * (29 - k)); best[k] = sites[s].live; idx[k] = s; break; }
    int64_t total = 0; for (int s = 0; s < SITES; s++) total += sites[s].live;
    fprintf(f, "total_live_MB %lld\n", (long long)(total >> 20));
    for (int k = 0; k < 30 && best[k] > 0; k++) { fprintf(f, "%lld MB %lld blocks:", (long long)(best[k] >> 20), (long long)sites[idx[k]].count);
      for (int i = 0; i < FRAMES; i++) fprintf(f, " %p", sites[idx[k]].pc[i]); fprintf(f, "\n"); }
    pthread_mutex_unlock(&mu);
    if (m) { fprintf(f, "MAPS\n"); while (fgets(line, sizeof line, m)) if (strstr(line, "r-xp") || strstr(line, " r--p 00000000")) fputs(line, f); fclose(m); }
    fclose(f); } return 0; }
__attribute__((constructor)) static void start(void) { if (!getenv("ALLOCTRACK_OUT")) return; pthread_t t; pthread_create(&t, 0, reporter, 0); pthread_detach(t); }
