"""The PC target, shared by port/tools/build.py, data.py and link.py.

32-bit Windows with the Itanium C++ ABI (clang's windows-itanium): structs and bitfields are laid
out as by gcc, as on the PS2 (the MSVC ABI packs bitfields differently and 149 units use them),
and C++ member functions use the normal C calling convention, not thiscall, so C callers and the
game's function tables can call them. Objects are COFF and link with lld-link against the MSVC
C runtime.

Win32 C symbols carry a leading underscore (func_00123456 is _func_00123456 in the objects);
asm labels and the converted data add it by hand (LABEL_PREFIX)."""

import os

# DHD_PLATFORM=macos: the same 32-bit program as wasm32 (32-bit pointers, one flat memory that
# stands in for the 32-bit Windows address space), turned into C by wasm2c and compiled for the
# Mac (finding 075). wasm32 lays out structs and bitfields as the Itanium ABI does.
PLATFORM = os.environ.get('DHD_PLATFORM', 'windows')
WASM = PLATFORM == 'macos'
BUILD = 'port-mac' if WASM else 'port'  # build/<BUILD>: each platform's own build tree
if WASM:
    TARGET = HOST_TARGET = 'wasm32-wasip1'
    LABEL_PREFIX = ''
    # wasm32 already aligns 64-bit fields to 8 and has single-precision floats without excess
    # precision. Every object gets the same features (wasm-ld refuses a mix): atomics for the
    # port's threads, and float-to-int conversions that saturate instead of trapping.
    ARCH_FLAGS = ['-matomics', '-mbulk-memory', '-mnontrapping-fptoint', '-msign-ext']
else:
    TARGET = 'i686-pc-windows-itanium'
    # The asm runtime and other host code use the platform's headers: plain C, same C ABI.
    HOST_TARGET = 'i686-pc-windows-msvc'
    LABEL_PREFIX = '_'
    ARCH_FLAGS = ['-malign-double', '-msse2', '-mfpmath=sse']

# The emulated PS2 RAM (32 MB) sits at this host address, with the uncached mirrors at
# 0x20000000/0x30000000 + it (port/runtime/memory.c). Below 0x10000000, so a PS2 DMA address
# (pointer & 0x0FFFFFFF) is still the host pointer; the exe is linked at IMAGE_BASE, above it all.
# On the Mac the wasm memory is the PS2 address space itself: the RAM is at 0, and the module's
# own data, stack and heap start at MODULE_BASE (port/tools/link_mac.py).
PS2_RAM_BASE = 0 if WASM else 0x08000000
IMAGE_BASE = 0x40000000
MODULE_BASE = 0x02000000
MODULE_END = 0x10000000
