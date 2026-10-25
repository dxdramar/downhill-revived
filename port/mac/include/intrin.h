/* Mac build: the one MSVC intrinsic the runtime uses beyond SSE. */
#include "simde_sse.h"
#define __rdtsc() mac_time_ns()
