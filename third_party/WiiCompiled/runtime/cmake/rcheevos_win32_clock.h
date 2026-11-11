/* Force-included into rcheevos on MinGW. MinGW's <time.h> defines
 * CLOCK_MONOTONIC, so rc_client.c would call clock_gettime, which lives in
 * libwinpthread-1.dll: the shipped exe does not carry that DLL and would not
 * start. Without the macro rcheevos takes its own QueryPerformanceCounter path.
 * <time.h> is included first so its guard keeps the macro from returning. */
#include <time.h>
#undef CLOCK_MONOTONIC
