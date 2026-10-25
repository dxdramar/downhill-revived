/* The PS2 address space on the Mac (finding 075): the wasm module's memory is it. The native
   host maps it before the module starts (port/mac/native/space.c): the RAM at 0, again at
   0x20000000 and 0x30000000 (the uncached views), the EE registers, VU memory and the
   scratchpad at their own addresses. So there is nothing to map here, and a PS2 address is the
   pointer. */
#include "memory.h"
#include "ps2asm.h"

#include <string.h>

/* {PS2 address, value} of every word of the image that is not the retail one: on the Mac only
   code addresses, which are the module's function table indices (port/tools/link_mac.py
   fixups.c). Data pointers are the retail PS2 addresses already. */
extern const uint32_t ps2_fixups[][2];
extern const uint32_t ps2_fixup_count;

int ps2_memory_init(void)
{
    ps2.vu0_code = (uint8_t *)(uintptr_t)0x11000000;
    ps2.vu0_mem = (ps2_q128 *)(uintptr_t)0x11004000;
    return 1;
}

int ps2_low_view_reserved(void) { return 1; }

void ps2_image_load(const uint8_t *retail)
{
    memcpy(PS2_PTR(PS2_IMAGE_START), retail, PS2_IMAGE_END - PS2_IMAGE_START);
    for (uint32_t i = 0; i < ps2_fixup_count; i++)
        *(uint32_t *)PS2_PTR(ps2_fixups[i][0]) = ps2_fixups[i][1];
    ps2_vu_aot_load(PS2_PTR(PS2_IMAGE_START));
}
