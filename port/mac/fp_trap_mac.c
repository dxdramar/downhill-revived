/* port/runtime/fp_trap.c's diagnostics are x86 (SSE exception traps, x87 state): on the Mac
   (finding 075) they are off, as they are on Windows unless DHD_FP_TRAP=1. */
#include "fp_trap.h"

void ps2_fp_trap_init(void) {}
void ps2_fp_trap_enable(void) {}
void ps2_x87_checkpoint(unsigned site, unsigned after) { (void)site; (void)after; }
