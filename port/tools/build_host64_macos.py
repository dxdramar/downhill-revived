"""Build a wasm2c module and the native macOS (Apple Silicon) host as dhd64 (finding 091).

build_host64_linux.py's counterpart: the same portable core (build_wasm.py's dhd_core.wasm), the
POSIX host (port/host/host_posix.c, POSIX threads, fiber_posix.c's arm64 switch, guest memory
over an unlinked POSIX shared memory object) and SDL3 for the window, input and sound
(port/tools/sdl3.py --macos, a static library). No force-link or missing stubs. All generated C
is prepared before compilation; all output stays in build/.

Needs Apple's clang (Xcode's command line tools) for the native side, WABT (wasm2c and its
runtime; Homebrew's by default, --wabt), SIMDe's source (--simde, as build_wasm.py), and CMake,
Ninja and pkg-config for SDL3.

usage: python port/tools/build_host64_macos.py build/wasm/dhd_core.wasm
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_host64_imports import generate
from prepare_wasm2c import check_launcher_exports, module_units, prepare_units

ROOT = Path(__file__).resolve().parents[2]
WABT = Path('/opt/homebrew/opt/wabt')
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')
SOURCE = ROOT / 'port/host64'

# The host units, in link order: the Linux set (build_host64_linux.py), with SDL's window, input
# and sound always in, and the Rust renderer (gpu_rust.c over libdhd_render.a, port/render built by
# rustgs.py for aarch64-apple-darwin: wgpu on Metal). WebSocket and HTTPS through NSURLSession
# (ws_apple.m: ws_posix.c has no TLS).
HOST_UNITS = [
    'port/host/host_posix.c', 'port/host/sound_out_sdl.c', 'port/host/sdl_host.c', 'port/host/dialogs_apple.m',
    'port/runtime/sha256.c',
    'port/hle/host.c', 'port/hle/graphics_queue.c', 'port/hle/vif.c', 'port/runtime/vu0_interp.c',
    'port/gs/memory.c', 'port/gs/gs.c', 'port/gs/display.c', 'port/gs/present.c', 'port/gs/gpu_rust.c',
    'port/gs/texpack_linux.c', 'port/gs/font_sharp.c', 'port/gs/capture.c',
    'port/hle/gamepad.c', 'port/hle/keyboard.c', 'port/hle/text_input.c', 'port/hle/movie.c',
    'port/hle/splash.c', 'port/hle/update.c', 'port/hle/fps.c',
    'port/net/client.c', 'port/net/hash.c', 'port/net/json.c', 'port/net/lockstep.c', 'port/net/net.c',
    'port/net/ws_apple.m',
] + ['port/third_party/libmpeg2/%s.c' % n for n in (
    'alloc', 'cpu_accel', 'cpu_state', 'decode', 'header', 'idct', 'motion_comp', 'slice')]
# port/hle/gs_queue.inc is included by graphics_queue.c.


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('module', type=Path, nargs='+')
    p.add_argument('--out', type=Path, default=ROOT / 'build/host64-macos')
    p.add_argument('--jobs', type=int, default=os.cpu_count())
    p.add_argument('--num-outputs', type=int, default=16, help='WABT split C output count for .wasm input')
    p.add_argument('--wabt', type=Path, default=WABT)
    p.add_argument('--simde', type=Path, default=Path(os.environ.get('SIMDE_DIR') or DEVCACHES / 'simde-src'),
                   help='SIMDe headers (the wasm SIMD shim, as in build_host64.py)')
    a = p.parse_args()
    if sys.platform != 'darwin':
        p.error('macOS only (build_host64_linux.py builds the Linux host)')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    from sdl3 import macos_build
    sdl_lib, sdl_include, sdl_libs = macos_build()
    import rustgs  # cargo through rustgs only (its flags, version gate and cache)
    try:
        render = rustgs.build(rustgs.MAC)
    except rustgs.Refused as why:
        p.error('rustgs: refused: %s' % why)
    host_units = list(HOST_UNITS)
    env = dict(os.environ, TMPDIR=str(out / 'tmp'))
    (out / 'tmp').mkdir(exist_ok=True)
    commands = []

    def run(cmd, timeout=300):
        cmd = list(map(str, cmd))
        commands.append(cmd)
        r = subprocess.run(cmd, cwd=out, env=env, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout)
        if r.returncode:
            raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        return r.stdout + r.stderr

    rt = a.wabt / 'share/wabt/wasm2c'
    wasm = a.module[0].resolve() if len(a.module) == 1 and a.module[0].suffix == '.wasm' else None
    units = module_units(out, run, a.wabt / 'bin/wasm2c', wasm, a.module, a.num_outputs)
    rows, exports = generate(out / 'core.h', out, linux=True)  # the POSIX guards
    check_launcher_exports(exports)
    prepare_units(units, out)
    common = ['clang', '-c', '-O2', '-arch', 'arm64', '-mcpu=apple-m1', '-mmacosx-version-min=13.0',
              '-ffp-model=strict', '-ffp-contract=off',
              # wasm-rt.h defaults to guard-page checks, which our mirror allocator
              # replaces: keep every generated OOB access a trapping check (finding 082).
              '-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1',
              '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing',
              # The link below strips what nothing references (-dead_strip), as the Windows
              # build's /opt:ref: the host keeps legacy guest-extern helpers only some builds use.
              '-fwrapv', '-pthread', '-w',
              '-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_MAX_CALL_STACK_DEPTH=512',
              # Traps can come from any EE fiber: report and end (launcher.c).
              '-DWASM_RT_TRAP_HANDLER=host64_trap',
              # SSE through SIMDe on arm64 (port/host64/sse_arm64), ahead of the compiler's headers
              '-I' + str(SOURCE / 'sse_arm64'),
              '-I' + str(out), '-I' + str(a.wabt / 'include'), '-I' + str(rt), '-I' + str(SOURCE),
              '-I' + str(ROOT / 'port/runtime'),
              '-I' + str(ROOT / 'port/include'), '-I' + str(ROOT / 'port/host'),
              '-I' + str(a.simde)]
    objdir = out / 'obj'
    objdir.mkdir(exist_ok=True)
    # The units that include SDL3's headers (host64_census.SDL_UNITS).
    sdl_units = ('port/hle/gamepad.c', 'port/hle/text_input.c', 'port/gs/present.c',
                 'port/host/sdl_host.c', 'port/host/sound_out_sdl.c', 'port/host/dialogs_apple.m')

    # VU1's straight-line blocks as C (vu1_aot.inc and vu1_native_clip.inc, from the game's
    # .vutext: DHD_VUTEXT or assets/vutext.bin); vu0_interp.c runs them
    import vu1_aot
    gen = out / 'gen'
    gen.mkdir(exist_ok=True)
    vu1_aot.generate(gen / 'vu1_aot.inc')

    def compile_one(source):
        source = Path(source)
        rel = str(source.relative_to(ROOT)) if source.is_relative_to(ROOT) else source.name
        extra = ['-I' + str(gen)] if rel.startswith('port/') else []
        if rel in sdl_units:  # the SDL3 headers (sdl3.py)
            extra += ['-DDHD_HAVE_SDL', '-I' + str(Path(sdl_include).resolve())]
        if rel.endswith('gs/gpu_rust.c'):
            extra += ['-I' + str(ROOT / 'port/render/include')]  # dhd_render.h
        if rel.endswith('host64/launcher.c'):
            extra += ['-DDHD_MAIN_THREAD_SDL']  # the main thread is SDL's (Cocoa)
        if rel.endswith('vu0_interp.c'):
            extra += ['-DPS2_VU_AOT']
        if rel.endswith('.m'):
            extra += ['-fobjc-arc']  # Objective-C (ws_apple.m: NSURLSession)
        if 'libmpeg2/' in rel:
            extra += ['-I' + str(ROOT / 'port/third_party/libmpeg2')]
        obj = objdir / (rel.replace('/', '_') + '.o')
        run(common + extra + [str(source), '-o', str(obj)], timeout=900)
        return obj

    try:
        with ThreadPoolExecutor(a.jobs) as pool:
            host_objects = list(pool.map(compile_one, [ROOT / u for u in host_units]))
        library = out / 'libhost64.a'
        library.unlink(missing_ok=True)  # ar adds to an old archive: a unit left out must go
        run(['ar', 'rcs', str(library), *[str(o) for o in host_objects]])
        objects = []
        glue = [rt / 'wasm-rt-impl.c', out / 'imports_gen.c'] + [SOURCE / n for n in
                ('guest_memory.c', 'guest_memory_posix.c', 'fiber_posix.c', 'fp_mode.c', 'bridge.c',
                 'diagnostics.c', 'launcher.c')]
        with ThreadPoolExecutor(a.jobs) as pool:
            objects = list(pool.map(compile_one, units + glue))
        run(['clang', '-O2', '-arch', 'arm64', '-mmacosx-version-min=13.0', '-pthread', '-rdynamic',
             '-Wl,-dead_strip',
             *[str(o) for o in objects], str(library), render['lib'], *render['native_static_libs'],
             str(sdl_lib), *sdl_libs,
             '-o', str(out / 'dhd64'), '-lm'])
        (out / 'failure.log').unlink(missing_ok=True)
        print(f'PASS: {len(rows)} generated imports; {len(host_objects)} macOS host objects; {out / "dhd64"}',
              flush=True)
    except (RuntimeError, ValueError) as error:
        (out / 'failure.log').write_text(str(error))
        raise SystemExit(str(error))
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
