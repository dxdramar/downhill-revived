"""Mac build, last stage (finding 075): the C that wasm2c made of the module (link_mac.py) and
the native host (port/mac/native) compiled for arm64 into build/port-mac/dhd.

    python port/tools/build_mac.py [-j N]

wasm2c's runtime (wasm-rt-impl.c, from wabt) is built with guard pages instead of bounds checks
(the host reserves 8 GB, port/mac/native/space.c), without its signal handler and its stack depth
count: the module runs on several threads and fibers, which that count can't follow."""
import argparse, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metal_shaders, simde  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINK = os.path.join(ROOT, 'build', 'port-mac', 'link')
OUT = os.path.join(ROOT, 'build', 'port-mac', 'native')
NATIVE = os.path.join(ROOT, 'port', 'mac', 'native')


def wabt():
    """wabt's install prefix (wasm2c's runtime sources are in share/wabt/wasm2c)."""
    exe = shutil.which('wasm2c')
    if not exe:
        sys.exit('wasm2c not found: brew install wabt')
    return os.path.dirname(os.path.dirname(os.path.realpath(exe)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=os.cpu_count())
    ap.add_argument('-O', dest='opt', default='2')
    a = ap.parse_args()
    prefix = wabt()
    runtime = os.path.join(prefix, 'share', 'wabt', 'wasm2c')
    cdir = os.path.join(LINK, 'c')
    flags = ['clang', '-c', '-O' + a.opt, '-g0', '-w', '-arch', 'arm64', '-mcpu=apple-m1', '-mmacosx-version-min=13.0',
             '-DWASM_RT_MEMCHECK_GUARD_PAGES=1', '-DWASM_RT_SKIP_SIGNAL_RECOVERY=1',
             '-DWASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1', '-DWASM_RT_TRAP_HANDLER=dhd_trap',
             # the module's float operations as written: no contraction into fused multiply-adds,
             # which the EE doesn't have and which would change results (finding 026)
             '-ffp-contract=off',
             '-I', os.path.join(prefix, 'include'), '-I', str(simde.root()), '-I', cdir, '-I', NATIVE]
    shaders = os.path.dirname(metal_shaders.build())
    flags += ['-I', shaders]
    sources = sorted(os.path.join(cdir, f) for f in os.listdir(cdir) if f.endswith('.c'))
    sources += [os.path.join(runtime, 'wasm-rt-impl.c'), os.path.join(runtime, 'wasm-rt-mem-impl.c')]
    sources += sorted(os.path.join(NATIVE, f) for f in os.listdir(NATIVE) if f.endswith(('.c', '.m', '.S')))
    os.makedirs(OUT, exist_ok=True)

    def one(src):
        obj = os.path.join(OUT, os.path.basename(src) + '.o')
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src) and src.startswith(cdir) \
                and os.path.getmtime(obj) > os.path.getmtime(os.path.join(cdir, 'dhd.h')):
            return obj
        extra = ['-fobjc-arc'] if src.endswith('.m') else []
        p = subprocess.run(flags + extra + [src, '-o', obj], capture_output=True, text=True)
        if p.returncode:
            raise SystemExit('%s:\n%s' % (src, p.stderr[-4000:]))
        return obj
    with ThreadPoolExecutor(a.j) as ex:
        objs = list(ex.map(one, sources))
    exe = os.path.join(ROOT, 'build', 'port-mac', 'dhd')
    p = subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=13.0', '-o', exe] + objs +
                       ['-framework', 'CoreGraphics', '-framework', 'CoreFoundation', '-framework', 'Metal',
                        '-framework', 'QuartzCore', '-framework', 'AppKit', '-framework', 'ImageIO', '-framework', 'AudioToolbox',
                        '-framework', 'UniformTypeIdentifiers', '-framework', 'GameController', '-framework', 'CoreHaptics'], capture_output=True, text=True)
    if p.returncode:
        sys.exit(p.stderr[-6000:])
    print(exe)


if __name__ == '__main__':
    main()
