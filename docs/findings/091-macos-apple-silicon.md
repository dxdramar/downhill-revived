# 091: macOS on Apple Silicon

The portable 64-bit core (findings 081-083) and the Rust renderer (084-086) run natively on an
Apple Silicon Mac. The program is the same `dhd_core.wasm`, wasm2c'd and compiled for arm64.
Around it sit the POSIX host, SDL3 for the window, input and sound, and wgpu on Metal. Tested on
an M1 Pro (macOS, a 2560x1440 monitor):
- boot, menus, the race load and races at 59.9 fields/s (native renderer, 2x windowed and 3x
  fullscreen);
- the startup movies;
- Core Audio through SDL;
- a DualSense through SDL3;
- the memory card (save, overwrite prompt on the next save);
- online: Public Races lists the official server's rooms, and hosting a room works.

## Build

```
export WASI_SYSROOT=/opt/homebrew/opt/wasi-libc/share/wasi-sysroot WABT_DIR=/opt/homebrew/opt/wabt
export WASI_BUILTINS=/opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a
export CARGO_HOME=~/.cargo RUSTUP_HOME=~/.rustup PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/rustup/bin:$PATH
python port/tools/build_wasm.py                                   # the core, as for Windows
python port/tools/build_host64_macos.py build/wasm/dhd_core.wasm  # build/host64-macos/dhd64
python port/tools/package_mac.py                                  # build/host64-macos/Downhill Revived.app
```

Homebrew: `llvm lld wasi-libc wasi-runtimes wabt rustup cmake ninja pkg-config`. Xcode's command
line tools compile the native side. SIMDe's git source goes in `~/DevCaches/simde-src`, as
BUILDING.md says: release 0.8.2 types `_Float16` on wasm32, which LLVM refuses there; the source
has a guard for it. `build_wasm.py` takes `WASI_BUILTINS` because Homebrew's sysroot isn't laid
out as a wasi-sdk.

## The pieces

- **`build_host64_macos.py`**: `build_host64_linux.py` for arm64 macOS.
  - The host units: Linux's, plus `gpu_rust.c` and `font_sharp.c` (Windows' renderer units),
    `ws_apple.m` in place of `ws_posix.c`, and `dialogs_apple.m`. SDL is always in.
  - Mach-O differences: `-dead_strip` instead of `--gc-sections`, and no `-rdynamic`. Exporting
    every symbol keeps the module-side code in `fps.c` alive, which names guest data the host
    doesn't have.
  - The archive is made anew each build: `ar rcs` added to an old one, and a stale `gpu_null.o`
    won the link once.
- **SSE on arm64** (`port/host64/sse_arm64`): the host's VU0 interpreter, graphics queue and EE
  fast paths use SSE intrinsics. On arm64 that directory comes first on the include path and
  gives them SIMDe's, on NEON. `_mm_getcsr`/`_mm_setcsr` go to `host_fp_csr`/`host_fp_set_csr`.
- **The FP control word**: host_api.h says MXCSR, but `host_posix.c`'s aarch64 branch read and
  wrote raw FPCR bits. The VU and EE modes (`(csr & ~0x603F) | 0xFFC0`: round toward zero,
  FTZ/DAZ) then set the wrong bits and none of the right ones. The aarch64 branch now
  translates:
  - RC (bits 13-14) ↔ FPCR RMode (bits 22-23);
  - FZ/DAZ ↔ FPCR FZ (bit 24);
  - the six flags ↔ FPSR.

  It writes FPCR and FPSR only when they change: `msr fpcr` serializes an Apple core, and the
  modes are set around much of the VU work.
- **Fibers** (`fiber_posix.c`):
  - The aarch64 switch now saves d8-d15, which AAPCS64 makes callee-saved.
  - A new fiber's first `ret` read the trampoline one slot past where it was stored (to NULL).
  - Mach-O symbols take a leading underscore and have no `.type`.
  - The guard page is 16 KiB on Apple Silicon.
- **Pages**: Apple Silicon's are 16 KiB. `host64_memory_commit` and `host_mem_protect` aligned
  to 4 KiB, so `mprotect` failed on unaligned starts, and `host_fiber_create` failed with it (an
  abort in `host_fiber_switch` at boot). Both align to `sysconf(_SC_PAGESIZE)` now.
- **Guest RAM** (`guest_memory_posix.c`): macOS has no `memfd_create`. The RAM section is an
  unlinked POSIX shared memory object (`shm_open`, then `shm_unlink`).
- **`host_posix.c`**:
  - `/proc/self/exe` becomes `_NSGetExecutablePath`; the code range comes from the Mach-O
    `__TEXT` segment.
  - Guest paths change `\` to `/`: mc.c builds `saves\card1\BASCUS-97177\...`, which on POSIX
    made single files named with backslashes. Linux had the same problem.
  - `host_wait_wall` sleeps as a time-constraint thread for the sleep alone. macOS coalesces the
    timers of a process it doesn't treat as interactive: one started from a background shell
    woke ~6 ms late, and the just-in-time pacing lost fields (57/s).
- **"64-bit host" in host code**: host.c, vif.c and keyboard.c tested
  `defined(_WIN64) || defined(__x86_64__)` for "the native host, not the wasm module"; arm64 now
  counts too. Thread names and thread CPU times use macOS's calls (`pthread_setname_np(name)`,
  Mach `thread_info`); the crash report reads the arm64 ucontext.
- **The main thread is SDL's**: Cocoa's windows and events belong to the main thread.
  - `launcher.c` (with `DHD_MAIN_THREAD_SDL`) runs the game on its own thread: 64 MB stack,
    user-interactive QoS.
  - The main thread runs `sdl_host_main`, `sdl_host.c`'s loop.
  - present.c makes every window call there through `sdl_host_run`.
- **The renderer**:
  - `rustgs.py` builds `libdhd_render.a` for `aarch64-apple-darwin` with wgpu's `metal`
    feature. Cargo.lock gains `wgpu-core-deps-apple` and `block2`; no pinned version moves. The
    COFF release checks stay Windows-only.
  - The surface accepts `DHDR_WINDOW_METAL_LAYER`: the CAMetalLayer of an SDL Metal view, from
    a `SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY` window.
  - The backend is Metal.
  - A CAMetalLayer has no RGBA8, so on Apple the surface and the two present pipelines use
    BGRA8 (`device::SURFACE_FORMAT`).
  - gpu_rust.c skips DXGI there (adapter memory unknown, so the scale limit is the maximum).
  - `native_thread.h` gains the pthread implementation its plan named.
- **Online**: `ws_posix.c` has no TLS, so `wss://dhd.skuza.app` can't connect through it.
  `ws_apple.m` implements `ws_winhttp.h` over NSURLSession, threaded like `ws_winhttp.c`. The
  lobby's build identity hashes the executable, so Macs race Macs on the same build; Windows
  rooms show "(other version)".
- **The `.app`**:
  - From a bundle, `launcher.c` points `DHD_SETTINGS` and `DHD_MEMCARD_DIR` at
    `~/Library/Application Support/Downhill Revived`, and sends the log there when started from
    Finder.
  - It copies the bundle's `dhd.ini` template (native, 3x, borderless, widescreen, memory card)
    on the first start.
  - `host_disc_ready` asks for the ISO with an SDL message box and an `NSOpenPanel`
    (`dialogs_apple.m`) on the main thread.
  - "Borderless" and "fullscreen" are macOS's own fullscreen.

## Open

- Controllers: SDL3 finds a DualSense over Bluetooth as player 1 ("ps5, vibration"); the buttons
  go through gamepad.c as on Windows. Rumble and play by hand on the Mac aren't checked yet.
- The disc-image dialog (first start without an `iso` line) is built but not clicked through.
- Float results aren't compared with the Windows build's game-state gate, and NEON through SIMDe
  may round where SSE does not. So online races are Mac to Mac.
- Texture packs: `texpack_linux.c`'s stub (none load). The updater isn't wired for macOS.
- The `.app` is ad-hoc signed, not notarized.
- The Windows and Linux builds weren't rebuilt here. The shared changes keep their values there
  (`#if defined(__APPLE__)` / `__aarch64__` branches, `SURFACE_FORMAT` RGBA8, 4 KiB pages,
  `rustgs`'s Windows path untouched), and posix_path changes Linux's card paths to folders.
