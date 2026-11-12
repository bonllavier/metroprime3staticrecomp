// LD_PRELOAD helper: lets any process of the same user attach with gdb -p while
// kernel.yama.ptrace_scope=1 (Ubuntu default), without sudo or system changes.
// Build: gcc -O2 -shared -fPIC -o ptracer.so ptracer.c
#include <sys/prctl.h>
__attribute__((constructor)) static void allow_ptrace(void) { prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); }
