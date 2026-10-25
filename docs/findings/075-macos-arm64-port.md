# A native macOS (Apple Silicon) port: the 32-bit program as wasm32 (2026-10-25)

Goal: a `dhd` that runs natively on arm64 macOS, not through Wine or Rosetta.

## The blocker: the port assumes a 32-bit address space at fixed low addresses

The Windows port is built on one idea (port/tools/pctarget.py, port/runtime/memory.h): a PS2
pointer and a host pointer are the same 32-bit value. PS2 RAM sits at host 0x08000000, the RAM
from 0x00400000 up is also mapped at its own PS2 addresses (the game's data files hold absolute
PS2 pointers), the data image's pointers are rewritten to host addresses that fit in 32 bits, and
the decompiled C in `src/` is compiled for i686 so every struct with a pointer has the PS2 layout.

None of that is possible on arm64 macOS. Checked on macOS 27.0.1, M-series, Apple clang 21:

- `mmap(0x08000000, 32 MB, MAP_FIXED)` and `mmap(0x00400000, ...)` fail: the low 4 GB is
  `__PAGEZERO`.
- Linking with `-Wl,-pagezero_size,0x4000` gives a binary the kernel kills at exec (SIGKILL,
  exit 137). arm64 processes must keep the 4 GB page zero.
- There is no 32-bit process on macOS (no i386, and `arm64_32` is watchOS only).

## The way through: compile the same 32-bit program for wasm32, then to C

wasm32 is a 32-bit target with one flat linear memory. Compiled for it, the decompiled C and the
port keep 4-byte pointers and the EE's struct layout, and every pointer is an offset into one
block of memory that the Mac host owns. `wasm2c` (wabt) turns the linked module into plain C,
which clang compiles for arm64 with the host code. Nothing interprets wasm at run time.

The linear memory is the PS2 address space itself (`PS2_RAM_BASE` is 0 on the Mac):

| wasm address | what |
|---|---|
| 0x00000000-0x01FFFFFF | PS2 RAM, so a PS2 address is the pointer, with no low view to keep |
| 0x02000000-0x0FFFFFFF | the port's own data, stack and heap (`wasm-ld --global-base`) |
| 0x10000000-0x1100FFFF | EE registers and VU memory, plain memory as on Windows |
| 0x20000000, 0x30000000 | the uncached RAM mirrors: the same shared memory object mapped again |
| 0x70000000 | scratchpad |

The host reserves 8 GB (4 GB plus wasm2c's guard region, so wasm2c needs no bounds checks) and
maps the RAM with `shm_open` + `mmap(MAP_SHARED | MAP_FIXED)` three times.

The data image's labels (`D_...`, defined absolute on Windows) cannot be absolute symbols in a
wasm object. `build.py` already passes every unit through LLVM IR; on the Mac it replaces each
`@D_...` by `inttoptr (i32 <PS2 address> to ptr)` and drops the declaration.

Spike (scratch, not committed), all on a native arm64 binary: a struct `{ptr; long long; short;
fnptr}` is 24 bytes with the function pointer at 20 as on the EE; 32-bit function pointers,
including calls through casts to another signature; an image label at its PS2 address; 128-bit
integers (`mode(TI)`); a write through 0x20100004 read back at 0x00100004.

`src/` compiles for wasm32 with two source fixes (port/tools/transform.py `host_types`: the
PS2 `va_list` and `operator new[]`'s size are spelled with the host's types) and the x86 asm
in port/runtime/ee_native_float.h replaced by host imports: 3815 of 3815 units.

## Floats

EE code runs with round-toward-zero and flushed denormals (kernel.c `ee_float_mode`). wasm has
no rounding modes, but wasm2c emits float operations as plain C, so on the Mac they run in the
mode the host sets in FPCR (RMode = toward zero, FZ = flush), as SSE code runs in MXCSR's mode
on Windows. The few operations that need round-to-nearest (ee_native_float.h's doubles) are
host functions that switch FPCR around one instruction.

## Threads

wasm2c 1.0.42 does not implement `memory.atomic.wait32` / `memory.atomic.notify`, which
wasi-libc's thread build uses for TLS setup and futexes. So the module uses the plain
`wasm32-wasip1` libc and the host provides threads, locks and events through imports (a Win32
subset on pthreads, port/mac/include/windows.h and port/mac/win32.c). Each host thread runs the
module on its own copy of wasm2c's instance struct: memory and function table shared, its own
`__stack_pointer` (the module's only mutable global). Fibers swap that field with the host
context (port/mac/native/switch_arm64.S keeps FPCR too).

Three things the single-threaded wasm world got wrong, found on the way:
- wasi-libc's malloc takes no lock: every allocator call is wrapped in a host lock
  (`wasm-ld --wrap`, port/mac/malloc_lock.c).
- wasm2c compiles atomic operations on memory that isn't shared as plain copies (wasm says such
  memory has one thread). link_mac.py `real_atomics` makes them `__atomic` operations.
- wasm2c's indirect calls go through the table's `module_instance`, which is the main
  instance: a call through a pointer on another thread ran on the game thread's stack pointer
  and corrupted it (the GS packets the VU1 worker wrote, texture sizes). `calls_on_own_instance`
  passes the caller's own instance.
- x86 keeps stores in order; arm64 doesn't. The port's units are built with `-fms-volatile`
  (volatile as acquire/release), as MSVC would give it.

## Calls with another type

The decompiled C often declares a function with other parameters than its definition, and
calls through casts. A wasm call must match exactly; wasm-ld makes a mismatched call a trap.
link_mac.py `unify_declarations` gives every declaration its definition's type and sends each
call of another type to an adapter in the caller's unit (arguments converted, extra ones dropped,
missing ones zero). Calls through function pointers skip wasm2c's type check
(`relax_call_indirect`): the game's vtables and thread entries are called through casts.

Variadic functions need care. LLVM's own fix for a mismatched call (FixFunctionBitcasts, the
`.L<name>_bitcast` wrappers in dhd.map) drops every vararg. The game's `sprintf`
(func_00254D68, defined `i32 (i32, i32, ...)`) and `printf` (func_00254D08) are called
unprototyped or as `void (ptr, ptr, ...)` from 32 units, so every `%s` read NULL. Track and
rider file names came out as `\LVL\(NULL).NGP`, and the race load waited forever. Adapters now
pass a fixed-argument caller's extra arguments on as varargs. A variadic call is rewritten
inline to the definition's exact type (`variadic_call`). dhd.map should list no `_bitcast`.

Labels inside code that only forward to a function (a callback thunk:
`addiu sp; sd ra; jal f; nop; ld ra; jr ra; addiu sp`, data.py `forwarding_aliases`) are aliases
of that function on Windows. On the Mac, absolute_globals made them plain PS2 numbers, so C that
called through one (D_00260898, a timer callback, `TimerCallback` in 0011C6F0.c) found no
function in the table. link_mac.py `text_aliases` rewrites those numbers in the IR, and the
image fixups, to the function (7 labels).

## Status (2026-10-26)

Playable. The game boots from the disc image to the title, menus and a full race. It draws in a
Retina AppKit window through Metal (native renderer at 2x), plays sound through Core Audio and
reads a DualSense (or any pad macOS knows) at 59.94 fields a second on an M1 Pro. The keyboard
goes through the same keyboard.c mapping as on Windows.

Build (macOS 13+, Apple Silicon; `brew install llvm lld wasi-libc wasi-runtimes wabt`):

    export PATH=/opt/homebrew/opt/llvm/bin:$PATH
    DHD_PLATFORM=macos python port/tools/build.py -j 8 --out build/port-mac
    DHD_PLATFORM=macos python port/tools/link_mac.py
    python port/tools/build_mac.py
    DHD_ISO=<your iso> DHD_RENDER=native DHD_SCALE=2 build/port-mac/dhd

`python port/tools/package_mac.py` then makes build/port-mac/Downhill Revived.app (ad-hoc
signed, no game data). It keeps dhd.ini, saves/ and dhd.log in ~/Library/Application
Support/Downhill Revived, starting from the bundle's dhd.ini template (native renderer, 3x,
borderless, widescreen). The first start asks for the disc image.

`link_mac.py --relink` links the objects of its last run again (seconds instead of minutes); it
skips the image fixups and the unit list, so a new port unit or a fixup change needs a full run.

Speed (the native renderer, 2x, sound on; 43 fields/s at first):
- The VU1/VIF worker thread is the busiest (about 90%): the VU1 AOT blocks are the shared
  interpreter's steps inlined, run as wasm2c's C.
- `msr fpcr` serializes an Apple core and the port sets MXCSR around much of its work, mostly
  to what it already is: fenv.c writes FPCR/FPSR only when they change.
- SetEvent wakes a condition only when a thread waits on it (the VU kicks the GS per packet).
- Game threads are `QOS_CLASS_USER_INTERACTIVE`; wasm2c's C is built for `-mcpu=apple-m1`.
- Frame pacing (kernel.c's just-in-time frames) waits to the millisecond. `mach_wait_until`
  woke ~6 ms late for a process started from a background shell (timer coalescing), so a field
  came late every frame (57 fields/s). system.c's wait makes the thread a time-constraint
  thread for the wait alone (16 us late), host_mac.c sleeps to 1 ms before the time and spins
  as on Windows, and the app holds a latency-critical activity (no App Nap).

Controllers: GameController.framework instead of SDL3 (port/mac/native/pads.m behind
mac_pad.h, port/mac/gamepad_mac.c), the same slots and positional mapping as gamepad.c, the
pads read with the window in the background too. Vibration plays on the pad's haptics engine.
DHD_PAD_TRACE=1 logs each change.

Online: ws_winhttp.h's WebSocket client over NSURLSession (port/mac/ws_mac.c keeps
ws_winhttp.c's receive and send threads; port/mac/native/net.m behind mac_net.h). The ONLINE
menu reaches wss://dhd.skuza.app: it lists public races and creates rooms. The lobby's build
identity is the SHA-256 of the executable (online.c `build_version`). On the Mac,
GetModuleFileNameA names the data folder, so it hashes `mac_exe_path` instead. Lockstep races
need the same executable on every side, so a Mac build only races the same Mac build. Windows
players' rooms show "(other version)", and the Mac's float results haven't been checked against
Windows' anyway (see Open).

Debugging aids: DHD_SNAP=path.png with DHD_SNAP_AT=20,40,60 saves the window's picture at
those seconds. The `mac_backtrace` import prints the native stack (wasm2c names the module's
functions). `sample <pid>` works where lldb isn't allowed.

Open:
- Float rounding: wasm2c's C is compiled without `-frounding-math`, so clang may fold or move
  float operations across FPCR changes, as on Windows. Not checked against the Windows build.
- The exact renderer's double-precision depth runs in float on Metal (no doubles): approximate.
- Not yet: texture packs (WIC PNG), the updater; the `.app` is ad-hoc signed, not notarized. An online race between two
  Macs hasn't been played yet.
  Movies, saves and the file panel are only lightly tested. Vibration hasn't been checked by
  hand yet.
- The Windows build has not been rebuilt with this branch's shared changes (transform.py's
  `host_types`, `PS2_HOST`, `RETAIL`, the `#ifdef`s in host.c, present.c and the runtime); they
  keep the Windows values but are untested there.

## What stays native on the Mac

Window and Metal renderer (raster.hlsl's compute shaders as MSL through glslang and
spirv-cross), audio output, controllers (GameController), the file dialog and message boxes;
HTTP and WebSocket (NSURLSession). The module calls them through narrow imports with plain integers and
(address, length) buffers.

## Next

1. Texture packs (ImageIO for WIC), the updater; an online race between two Macs.
2. A Developer ID signature and notarization for the `.app`, to run on other Macs without Gatekeeper's warning.
3. Checks against the Windows build's game-state gate (race_compare.py) for float rounding.

Every build needs the player's own `SCUS_971.77` (USA disc), as on Windows.
