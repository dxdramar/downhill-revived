/* Mac build (finding 075): SSE intrinsics through SIMDe (MIT, fetched by port/tools/simde.py),
   which builds them from wasm SIMD; wasm2c's output then compiles them to NEON. MXCSR is the
   host's: mac_getcsr/mac_setcsr keep arm64's FPCR in its encoding. */
#ifndef DHD_SIMDE_SSE_H
#define DHD_SIMDE_SSE_H
#define SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_FLOAT16_API 1 /* SIMDE_FLOAT16_API_PORTABLE: wasm32 has no _Float16 */
#include <x86/sse2.h>
#include "mac_host.h"
#undef _mm_getcsr
#undef _mm_setcsr
#undef _MM_GET_ROUNDING_MODE
#undef _MM_SET_ROUNDING_MODE
#undef _MM_GET_FLUSH_ZERO_MODE
#undef _MM_SET_FLUSH_ZERO_MODE
#undef _mm_pause
#define _mm_getcsr() mac_getcsr()
#define _mm_setcsr(x) mac_setcsr(x)
#define _MM_GET_ROUNDING_MODE() (mac_getcsr() & 0x6000u)
#define _MM_SET_ROUNDING_MODE(m) mac_setcsr((mac_getcsr() & ~0x6000u) | (m))
#define _MM_GET_FLUSH_ZERO_MODE() (mac_getcsr() & 0x8000u)
#define _MM_SET_FLUSH_ZERO_MODE(m) mac_setcsr((mac_getcsr() & ~0x8000u) | (m))
#define _mm_pause() ((void)0)
#endif
