/* SSE on arm64 hosts (finding 091): the host's VU0 interpreter, graphics queue and EE fast paths
   use SSE intrinsics. On an arm64 host (macOS) this directory comes first on the include path and
   its xmmintrin.h / emmintrin.h / intrin.h give them SIMDe's (MIT; NEON underneath). MXCSR is
   the host API's: host_fp_csr / host_fp_set_csr keep FPCR and FPSR in MXCSR's encoding
   (host_posix.c), so flush-to-zero and the rounding modes reach the arm64 FPU. */
#ifndef DHD_SSE_ARM64_H
#define DHD_SSE_ARM64_H
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/sse2.h>
#include "host_api.h"
#undef _mm_getcsr
#undef _mm_setcsr
#undef _MM_GET_ROUNDING_MODE
#undef _MM_SET_ROUNDING_MODE
#undef _MM_GET_FLUSH_ZERO_MODE
#undef _MM_SET_FLUSH_ZERO_MODE
#undef _mm_pause
#define _mm_getcsr() host_fp_csr()
#define _mm_setcsr(x) host_fp_set_csr(x)
#define _MM_GET_ROUNDING_MODE() (host_fp_csr() & 0x6000u)
#define _MM_SET_ROUNDING_MODE(m) host_fp_set_csr((host_fp_csr() & ~0x6000u) | (m))
#define _MM_GET_FLUSH_ZERO_MODE() (host_fp_csr() & 0x8000u)
#define _MM_SET_FLUSH_ZERO_MODE(m) host_fp_set_csr((host_fp_csr() & ~0x8000u) | (m))
#define _mm_pause() __asm__ volatile("yield")
#endif
