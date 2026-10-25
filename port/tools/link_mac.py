"""Mac build, link stage (finding 075): the wasm32 module, then C from it.

    DHD_PLATFORM=macos python port/tools/build.py -j 8 --out build/port-mac
    DHD_PLATFORM=macos python port/tools/link_mac.py

The same stages as link.py, for wasm32:
- the game's C++ units get their retail names (rename_cpp), in their IR, as llvm-objcopy can't
  rename wasm symbols (port/tools/wasmir.py);
- the port's C (port/runtime, port/hle, port/gs, port/net, and port/mac for what is Windows-only
  there) is compiled for wasm32 against the WASI C library, through the same IR rewrite;
- the functions that are still asm are recompiled to C (recomp.py), as on Windows;
- the data image's fixups (data.py) become fixups.c: on the Mac only code addresses differ from
  the retail words, and they are the module's function table indices;
- wasm-ld links the module (build/port-mac/link/dhd.wasm), with a stub for each missing
  function that names it when called (hle_missing), and wasm2c turns it into C
  (build/port-mac/link/c/).
port/tools/build_mac.py compiles that C with the native host (port/mac/native) into the Mac
program.

Needs LLVM 18+ with the WebAssembly target and wasm-ld, the WASI sysroot (wasi-libc,
wasi-runtimes) and wabt's wasm2c: on a Mac, brew install llvm lld wasi-libc wasi-runtimes wabt."""
import collections, functools, glob, itertools, json, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

os.environ.setdefault('DHD_PLATFORM', 'macos')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data, link, pctarget, recomp, simde, wasmir  # noqa: E402

if not pctarget.WASM:
    sys.exit('link_mac.py is the Mac build: DHD_PLATFORM=macos')

ROOT = data.ROOT
OBJ = os.path.join(ROOT, 'build', 'port-mac', 'obj')
OUT = os.path.join(ROOT, 'build', 'port-mac', 'link')
LLVM = os.environ.get('DHD_LLVM', '/opt/homebrew/opt/llvm/bin')
SYSROOT = os.environ.get('DHD_WASI_SYSROOT', '/opt/homebrew/share/wasi-sysroot')


def tool(name):
    path = os.path.join(LLVM, name)
    return path if os.path.exists(path) else shutil.which(name) or name


CLANG = tool('clang')


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)


def nm(paths, *flags):
    """{path: [symbol, ...]} via llvm-nm, in batches."""
    res = collections.defaultdict(list)
    for i in range(0, len(paths), 200):
        batch = paths[i:i + 200]
        cur = batch[0] if len(batch) == 1 else None
        for line in run([tool('llvm-nm'), '-f', 'just-symbols', *flags] + batch).stdout.splitlines():
            if line.endswith(':') and line[:-1] in batch:
                cur = line[:-1]
            elif line and cur:
                res[cur].append(line)
    return res


def compile_ir(ir, obj):
    """Compiles one unit's (rewritten) IR to a wasm object; the IR stays beside it (adapters)."""
    with open(ir_path(obj), 'w', encoding='utf-8', newline='\n') as f:
        f.write(ir)
    p = run([CLANG, '--target=' + pctarget.TARGET, *pctarget.ARCH_FLAGS, '-msimd128', '-O2', '-c', '-x', 'ir', '-',
             '-o', obj], input=ir)
    if p.returncode:
        raise RuntimeError('%s: %s' % (obj, p.stderr[-2000:]))
    return obj


def ir_path(obj):
    return obj[:-2] + '.ll'


# ---- calls with another type than the callee's ----
# The decompiled C sometimes declares a function with other parameters than it has (gcc 2.95 and
# the EE's calling convention didn't mind, nor does x86's cdecl). A wasm call must match the
# callee's type exactly: wasm-ld makes a mismatched call a trap, and so does LLVM where it can't
# convert (an i16 for an i32). So unify_declarations gives each declaration its definition's
# type, and each call of another type goes to an adapter of the call's own type, in the
# caller's unit, that calls the function as it is: the arguments it has, extra ones dropped,
# missing ones zero, as the EE's registers would have passed them.

WASM_OF = {'ptr': 'i32', 'i1': 'i32', 'i8': 'i32', 'i16': 'i32', 'i32': 'i32', 'i64': 'i64', 'float': 'f32',
           'double': 'f64', 'void': 'void', 'i128': 'i128'}
IR_OF = {'i32': 'i32', 'i64': 'i64', 'f32': 'float', 'f64': 'double', 'void': 'void', 'i128': 'i128'}


def split_params(text):
    """Top-level comma-separated items of a parameter or argument list."""
    out, depth, cur = [], 0, ''
    for c in text:
        if c in '([{<':
            depth += 1
        elif c in ')]}>':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def wasm_type(ir_type):
    return WASM_OF.get(ir_type, 'i32')


CALL = re.compile(r'\bcall\b(?P<pre>[^@\n]*?)\s(?P<ret>[\w.]+)\s*(?P<fnty>\([^@\n]*\))?\s*@"?(?P<name>[-\w.$]+)"?\((?P<args>[^\n]*)\)')


def call_ir_type(m):
    """(return, [params], variadic) of a call instruction in IR types: its explicit function
    type if it has one (a variadic call has), else its arguments'. None if not plain."""
    if m.group('fnty'):
        params = param_types(m.group('fnty')[1:-1])
        if params is None:
            return None
        return m.group('ret'), [p for p in params if p != '...'], '...' in params
    params = param_types(m.group('args'))
    if params is None or not PLAIN_TYPE.match(m.group('ret')):
        return None
    return m.group('ret'), params, False


def convert(have, want):
    return {('i32', 'i64'): 'zext', ('i64', 'i32'): 'trunc', ('i32', 'f32'): 'bitcast',
            ('f32', 'i32'): 'bitcast', ('i64', 'f64'): 'bitcast', ('f64', 'i64'): 'bitcast'}.get((have, want))


def zero(t):
    return '0.0' if t in ('f32', 'f64') else '0'


def ir_convert(value, have, want, temp):
    """Instructions and operand that turn value of IR type have into IR type want, through the
    wasm type each is passed as; None if they can't (an integer for a float)."""
    code, v = [], value
    hw, ww = wasm_type(have), wasm_type(want)
    if have == want:
        return code, v
    if have == 'ptr':
        code.append('  %s.a = ptrtoint ptr %s to i32' % (temp, v)); v = temp + '.a'
    elif have in ('i1', 'i8', 'i16'):
        code.append('  %s.a = sext %s %s to i32' % (temp, have, v)); v = temp + '.a'
    if hw != ww:
        op = convert(hw, ww)
        if not op:
            return None
        code.append('  %s.b = %s %s %s to %s' % (temp, op, IR_OF[hw], v, IR_OF[ww])); v = temp + '.b'
    if want == 'ptr':
        code.append('  %s.c = inttoptr i32 %s to ptr' % (temp, v)); v = temp + '.c'
    elif want in ('i1', 'i8', 'i16'):
        code.append('  %s.c = trunc i32 %s to %s' % (temp, v, want)); v = temp + '.c'
    return code, v


def null(t):
    return 'null' if t == 'ptr' else '0.0' if t in ('float', 'double') else '0'


def adapter(name, caller, callee, adapted):
    """IR of a function of the call's own IR type that calls name with its IR type: the
    arguments the call passed (or the varargs block holds), converted; missing ones zero; the
    result converted, or zero. caller and callee are (return, [params], variadic) in IR types.
    A variadic callee (the game's sprintf, called unprototyped) gets the call's further arguments
    as its varargs: LLVM's own fix for a mismatched call drops them."""
    cret, cparams, cvar = caller
    dret, dparams = callee[0], [p for p in callee[1] if p != '...']
    dvar = '...' in callee[1]
    head = ', '.join(['%s %%a%d' % (ty, i) for i, ty in enumerate(cparams)] + (['...'] if cvar else []))
    lines = ['define internal %s @"%s"(%s) {' % (cret, adapted, head)]
    if cvar:  # wasm32's va_list is the address of the block of extra arguments
        lines += ['  %ap = alloca ptr, align 4', '  call void @llvm.va_start.p0(ptr %ap)',
                  '  %vb = load ptr, ptr %ap, align 4', '  %vi = ptrtoint ptr %vb to i32']
    args, offset = [], 0
    for i, want in enumerate(dparams):
        if i < len(cparams):
            done = ir_convert('%%a%d' % i, cparams[i], want, '%%x%d' % i)
        elif cvar:
            w = wasm_type(want)
            size = 8 if w in ('i64', 'f64') else 4
            offset = (offset + size - 1) // size * size
            lines += ['  %%p%d = add i32 %%vi, %d' % (i, offset), '  %%q%d = inttoptr i32 %%p%d to ptr' % (i, i),
                      '  %%v%d = load %s, ptr %%q%d, align 4' % (i, IR_OF[w], i)]
            offset += size
            done = ir_convert('%%v%d' % i, IR_OF[w], want, '%%x%d' % i)
        else:
            done = None
        if done:
            lines += done[0]
            args.append('%s %s' % (want, done[1]))
        else:
            args.append('%s %s' % (want, null(want)))
    if dvar:
        for i in range(len(dparams), len(cparams)):
            args.append('%s %%a%d' % (cparams[i], i))
        call = 'call %s (%s, ...) @%s(%s)' % (dret, ', '.join(dparams), name, ', '.join(args))
    else:
        call = 'call %s @%s(%s)' % (dret, name, ', '.join(args))
    lines.append('  ' + call if dret == 'void' else '  %r = ' + call)
    if cvar:
        lines.append('  call void @llvm.va_end.p0(ptr %ap)')
    if cret == 'void':
        lines.append('  ret void')
    else:
        done = ir_convert('%r', dret, cret, '%rr') if dret != 'void' else None
        lines += done[0] + ['  ret %s %s' % (cret, done[1])] if done else ['  ret %s %s' % (cret, null(cret))]
    lines.append('}')
    return '\n'.join(lines)


HEAD = re.compile(r'^(?P<kind>define|declare)\b(?P<attrs>[^@\n]*?)\s(?P<ret>[\w.]+)\s+@"?(?P<name>[-\w.$]+)"?\(', re.M)


def function_lines(ir, kind):
    """(start, end, ret, name, params text, rest of the line) of each define or declare line;
    the parameter list ends at its matching parenthesis (attributes have their own)."""
    for m in HEAD.finditer(ir):
        if m.group('kind') != kind:
            continue
        depth, i = 1, m.end()
        while depth and i < len(ir) and ir[i] != '\n':
            depth += {'(': 1, ')': -1}.get(ir[i], 0)
            i += 1
        if depth:
            continue
        eol = ir.find('\n', i)
        eol = len(ir) if eol < 0 else eol
        yield m.start(), eol, m.group('ret'), m.group('name'), ir[m.end():i - 1], ir[i:eol], m.group('attrs')


PLAIN_TYPE = re.compile(r'^(?:ptr|i1|i8|i16|i32|i64|i128|float|double|void)$')


ALIAS = re.compile(r'^@"?(?P<name>[-\w.$]+)"? = [^\n]*?\balias (?P<ret>[\w.]+) \((?P<params>[^)]*)\), ptr @', re.M)
# The C library's functions the game declares its own way (wasi-libc has no IR here).
LIBC_TYPES = {n: ('ptr', ['ptr', 'ptr', 'i32']) for n in ('memcpy', 'memmove', 'strncpy', 'strncat')}
LIBC_TYPES.update({'memchr': ('ptr', ['ptr', 'i32', 'i32']), 'memcmp': ('i32', ['ptr', 'ptr', 'i32']),
                   'memset': ('ptr', ['ptr', 'i32', 'i32']), 'strcmp': ('i32', ['ptr', 'ptr']),
                   'strncmp': ('i32', ['ptr', 'ptr', 'i32']), 'strlen': ('i32', ['ptr']),
                   'strcpy': ('ptr', ['ptr', 'ptr']), 'strcat': ('ptr', ['ptr', 'ptr']),
                   'strchr': ('ptr', ['ptr', 'i32']), 'strrchr': ('ptr', ['ptr', 'i32']),
                   'strstr': ('ptr', ['ptr', 'ptr'])})


def param_types(text):
    """IR parameter types without names and attributes; None if one can't be copied to another
    unit (byval, sret and the like name types of their own)."""
    out = []
    for p in split_params(text):
        if p == '...':
            out.append('...')
            continue
        if re.search(r'\b(?:byval|byref|inalloca|preallocated)\b', p):
            return None  # sret is a plain pointer argument in wasm32's ABI, the others are not
        ty = p.split()[0]
        if not PLAIN_TYPE.match(ty):
            return None
        out.append(ty)
    return out


ARG_ATTRS = {'noundef', 'signext', 'zeroext', 'inreg', 'nonnull', 'noalias', 'nocapture', 'readonly', 'returned'}
VARIADIC_TEMPS = itertools.count()


def assigned(m):
    """Whether the call's result is assigned (%x = call ...)."""
    line_start = m.string.rfind('\n', 0, m.start()) + 1
    return '=' in m.string[line_start:m.start()]


def variadic_call(m, want, fixed):
    """A variadic call (its own fixed parameter types) as one of the definition's exact type:
    the same wasm call, without the wrapper LLVM puts on a mismatched one, which drops varargs.
    Arguments past the definition's fixed ones are its varargs; conversions go on lines before
    the call. A void call of a function with a result drops it; a call that wants a result the
    definition doesn't give gets zero (the EE's v0 held anything)."""
    args = split_params(m.group('args'))
    pre, out = [], []
    for i, a in enumerate(args):
        words = a.split()
        ty, rest = words[0], [w for w in words[1:] if w not in ARG_ATTRS and not w.startswith('align')]
        value = ' '.join(rest)
        if i < len(fixed) and ty != fixed[i]:
            done = ir_convert(value, ty, fixed[i], '%%vfix%d' % next(VARIADIC_TEMPS))
            pre += [c.strip() for c in done[0]]
            ty, value = fixed[i], done[1]
        out.append('%s %s' % (ty, value))
    call = 'call %s (%s, ...) @%s(%s)' % (want[0], ', '.join(fixed), m.group('name'), ', '.join(out))
    cret, lines = m.group('ret'), []
    if assigned(m):
        if want[0] == cret:
            if pre:
                return m.group(0)  # the conversions would land inside the assignment
            return call
        # the assignment before the match names a zero now; the call follows on its own
        lines.append({'ptr': 'bitcast ptr null to ptr', 'float': 'fadd float 0.0, 0.0',
                      'double': 'fadd double 0.0, 0.0'}.get(cret, 'add %s 0, 0' % cret))
        if want[0] != 'void':
            call = '%%vres%d = %s' % (next(VARIADIC_TEMPS), call)
    elif want[0] != 'void':
        call = '%%vres%d = %s' % (next(VARIADIC_TEMPS), call)  # unnamed, it would take a numbered slot
    return '\n  '.join(lines + pre + [call])


@functools.lru_cache(maxsize=None)
def text_aliases():
    """{PS2 address: function} of the labels inside code that only forward to a function (a
    compiler's callback thunk, data.forwarding_aliases): on Windows the label is an alias of the
    function, so C that calls through it and the data that holds it reach the function. Here a
    call through the bare address would find no function in the table."""
    names = set()
    for path in glob.glob(os.path.join(recomp.ASM, 'func_*.s')):
        names |= set(re.findall(r'^\s*alabel (D_[0-9A-F]{8})\s*$', open(path, encoding='utf-8').read(), re.M))
    return {int(n[2:], 16): f for n, f in data.forwarding_aliases(names).items()}


def with_text_aliases(ir):
    """The IR with each forwarding label's address (absolute_globals made it a number) as its
    function."""
    for address, name in text_aliases().items():
        number = 'inttoptr (i32 %d to ptr)' % address
        if number not in ir:
            continue
        ir = ir.replace(number, '@' + name)
        if not re.search(r'^(?:define|declare)\b[^@\n]*@"?%s"?\(' % re.escape(name), ir, re.M):
            ir += '\ndeclare void @%s()\n' % name
    return ir


def unify_declarations(objs):
    """Every unit's declaration of a function defined in another takes the definition's type.
    The decompiled C often declares a function with other parameters than it has; wasm-ld would
    make each such use a trap (and the function's address a trap's). With the definition's type,
    LLVM's wasm backend adapts mismatched calls inside the unit (FixFunctionBitcasts: extra
    arguments dropped, missing ones undefined, as on the EE), and an address is the function's.
    Returns the object list with the changed units compiled again."""
    irs = {o: open(ir_path(o), encoding='utf-8').read() for o in objs if os.path.exists(ir_path(o))}
    originals = dict(irs)
    irs = {o: with_text_aliases(ir) for o, ir in irs.items()}
    types = dict(LIBC_TYPES)
    for ir in irs.values():
        for _, _, ret, name, params, _, attrs in function_lines(ir, 'define'):
            params = param_types(params)
            if params is not None and PLAIN_TYPE.match(ret) and not re.search(r'\b(?:internal|private)\b', attrs):
                types[name] = (ret, params)
        for m in ALIAS.finditer(ir):  # C++ constructors and destructors: an alias of another variant
            params = param_types(m.group('params'))
            if params is not None and PLAIN_TYPE.match(m.group('ret')):
                types[m.group('name')] = (m.group('ret'), params)
    changed, bodies, decls = {}, [], {}
    for o, ir in irs.items():
        pieces, last = [], 0
        for start, end, ret, name, params, rest, _ in function_lines(ir, 'declare'):
            want, have = types.get(name), param_types(params)
            if not want or have is None or (ret, have) == want:
                continue
            pieces += [ir[last:start], 'declare %s @%s(%s)%s' % (want[0], name, ', '.join(want[1]),
                                                                  re.sub(r'#\d+', '', rest))]
            last = end
        new = ''.join(pieces) + ir[last:] if pieces else ir
        # the type each called function has here: as declared (now its definition's) or defined
        local = {}
        for kind in ('declare', 'define'):
            for _, _, ret, name, params, _, _ in function_lines(new, kind):
                params = param_types(params)
                if params is not None and PLAIN_TYPE.match(ret):
                    local[name] = (ret, params)
        mine = {}  # caller type -> adapter, in this unit

        def site(m):
            name = m.group('name')
            want = local.get(name)
            if not want:
                return m.group(0)
            caller = call_ir_type(m)
            if caller is None or (caller[0], caller[1], caller[2]) == (want[0], want[1], False):
                return m.group(0)
            if '...' in want[1]:
                fixed = [p for p in want[1] if p != '...']
                # a variadic call that names its type is already right; a plain one goes through
                # an adapter that passes its further arguments on as varargs
                if caller[2] and len(caller[1]) >= len(fixed) and \
                        [wasm_type(t) for t in caller[1][:len(fixed)]] == [wasm_type(t) for t in fixed] and \
                        (caller[0] == want[0] or want[0] == 'void' or (caller[0] == 'void' and not assigned(m))):
                    return variadic_call(m, want, fixed)
                if caller[2] or (m.group('fnty') and caller[0] == want[0] and caller[1][:len(fixed)] == fixed):
                    return m.group(0)
            key = (name, caller[0], tuple(caller[1]), caller[2])
            if key not in mine:
                mine[key] = adapter(name, caller, want, '%s.via%d' % (name, len(mine) + 1))
            adapted = re.search(r'@"([^"]+)"', mine[key]).group(1)
            s = m.group(0)
            return s[:m.start('name') - m.start(0)].rstrip('"') + '"%s"' % adapted + \
                s[m.end('name') - m.start(0):].lstrip('"')
        new = CALL.sub(site, new)
        if mine:
            new += '\n' + '\n\n'.join(mine.values()) + '\n'
            if any('llvm.va_start' in body for body in mine.values()) and 'declare void @llvm.va_start.p0' not in new:
                new += 'declare void @llvm.va_start.p0(ptr)\ndeclare void @llvm.va_end.p0(ptr)\n'
            bodies.extend(mine.values())
        if new != originals[o]:
            changed[o] = new

    def one(o):
        dst = os.path.join(OUT, 'unified', os.path.relpath(o, ROOT).replace(os.sep, '_'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        return o, compile_ir(changed[o], dst)
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        done = dict(ex.map(one, changed))
    out = [done.get(o, o) for o in objs]
    print('%d units declare or call a function with another type than its definition: %d adapters' % (
        len(done), len(bodies)))
    return out


# ---- the game's C++ units: retail names ----

def rename_cpp(objs):
    cpp = [o for o in objs if o.endswith('.cpp.o')]
    defined = {s for v in nm(cpp, '--defined-only').values() for s in v}
    syms = sorted({s for v in nm(cpp).values() for s in v
                   if s.startswith('_Z') and not (link.VARIANT.search(s) and s in defined)})
    dem = run([tool('llvm-cxxfilt')], input=''.join(s + '\n' for s in syms)).stdout.splitlines()
    mapping = {m: link.retail_name(d) for m, d in zip(syms, dem) if link.retail_name(d)}

    def one(o):
        dst = os.path.join(OUT, 'obj', os.path.relpath(o, OBJ))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        ir = open(o[:-2] + '.ll', encoding='utf-8').read()
        return compile_ir(wasmir.rename(ir, mapping), dst)
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        renamed = dict(zip(cpp, ex.map(one, cpp)))
    return [renamed.get(o, o) for o in objs], len(mapping)


# ---- the port's C ----

# port/ units as on Windows (link.py runtime()), with the Mac's own where theirs are Windows'.
# The network, the controllers, texture packs and the updater are not here yet
# (port/mac/headless.c, and the link's stubs).
MAC_INSTEAD = {'runtime/memory.c': 'mac/memory_mac.c', 'runtime/fp_trap.c': 'mac/fp_trap_mac.c',
               'hle/splash.c': 'mac/splash_mac.c'}
NOT_YET = {'gs/texpack.c', 'hle/update.c', 'net/ws_winhttp.c', 'hle/gamepad.c'}
PORT_UNITS = ['runtime/ps2asm.c', 'runtime/vu0_micro.c', 'runtime/vu0_interp.c', 'runtime/recomp.c',
              'runtime/fp_trap.c', 'runtime/libc_shims.c', 'runtime/memory.c', 'hle/host.c', 'hle/autopad.c',
              'hle/timer.c', 'hle/kernel.c', 'hle/boot.c', 'hle/sif.c', 'hle/cdvd.c', 'hle/game_data.c', 'hle/iop.c',
              'hle/mc.c', 'hle/dma.c', 'hle/vif.c', 'hle/graphics.c', 'hle/graphics_queue.c', 'hle/pad.c',
              'hle/menu_page.c', 'hle/pc_settings.c', 'hle/widescreen.c', 'hle/exit_menu.c', 'hle/online_menu.c',
              'hle/online.c', 'hle/puppet.c', 'hle/sound.c', 'gs/memory.c', 'gs/gs.c', 'gs/display.c',
              'gs/present.c', 'gs/gpu.c', 'gs/texpack.c', 'gs/font_sharp.c', 'gs/capture.c', 'hle/update.c',
              'hle/game_options.c', 'hle/fps.c', 'hle/splash.c', 'hle/movie.c', 'net/lockstep.c', 'net/hash.c',
              'net/json.c', 'net/client.c', 'net/ws_winhttp.c', 'net/net.c', 'hle/keyboard.c', 'hle/text_input.c',
              'hle/gamepad.c']
MAC_UNITS = ['mac/win32.c', 'mac/host_mac.c', 'mac/headless.c', 'mac/malloc_lock.c', 'mac/d3d11_mac.c', 'mac/gamepad_mac.c', 'mac/ws_mac.c']
UNIT_DEFINES = {}


def port_cflags():
    return ['--target=' + pctarget.TARGET, '--sysroot=' + SYSROOT, *pctarget.ARCH_FLAGS, '-msimd128',
            '-fms-extensions', '-w', '-ffp-contract=off', '-fno-strict-aliasing', '-fwrapv',
            # volatile with acquire/release semantics, as MSVC gives it: the port hands data between
            # its threads through volatile flags, which x86 keeps in order and arm64 doesn't
            '-fms-volatile',
            '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-D_WASI_EMULATED_MMAN',
            '-I', os.path.join(ROOT, 'port', 'mac', 'include'), '-include', 'msvc_compat.h',
            '-I', str(simde.include()), '-I', OUT, '-I', os.path.join(ROOT, 'port', 'runtime'),
            '-I', os.path.join(ROOT, 'port', 'hle'), '-I', os.path.join(ROOT, 'port', 'include')]


@functools.lru_cache(maxsize=None)
def headers_time():
    """The newest port header's time: a unit older than it is compiled again."""
    paths = [p for d in ('runtime', 'hle', 'gs', 'net', 'include', 'mac/include')
             for p in glob.glob(os.path.join(ROOT, 'port', d, '*.h')) + glob.glob(os.path.join(ROOT, 'port', d, '*.inc'))]
    paths += [os.path.join(ROOT, 'port', 'tools', n) for n in ('wasmir.py', 'link_mac.py', 'pctarget.py')]
    return max(os.path.getmtime(p) for p in paths)


def compile_c(src, obj, flags, opt='-O2'):
    """C to a wasm object, through the IR rewrite (port code may name image labels too); kept
    when it is newer than its source and every port header."""
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    if os.path.exists(obj) and os.path.exists(ir_path(obj)) and \
            os.path.getmtime(obj) > max(os.path.getmtime(src), headers_time()):
        return obj
    p = run([CLANG, *flags, opt, '-S', '-emit-llvm', src, '-o', '-'])
    if p.returncode:
        raise RuntimeError('%s: %s' % (src, p.stderr[-3000:]))
    return compile_ir(wasmir.absolute_globals(p.stdout), obj)


def runtime():
    aot = os.environ.get('DHD_BUILD_VU_AOT', '1') == '1'
    if aot:
        from vu1_aot import generate
        print('VU1 AOT: %d straight-line blocks' % generate(os.path.join(OUT, 'vu1_aot.inc')))
    # gpu.c embeds raster.hlsl for D3DCompile; on the Mac the shaders come precompiled (port/tools/
    # metal_shaders.py, port/mac/d3d11_mac.c), so the source isn't needed
    with open(os.path.join(OUT, 'gs_shader.h'), 'w') as f:
        f.write('static const char gs_shader[] = "";\nstatic const char gs_shader_path[] = "port/gs/raster.hlsl";\n')
    units = [MAC_INSTEAD.get(u, u) for u in PORT_UNITS if u not in NOT_YET] + MAC_UNITS
    flags = port_cflags()

    def one(name):
        extra = UNIT_DEFINES.get(name, []) + (['-DPS2_VU_AOT'] if aot and name == 'runtime/vu0_interp.c' else [])
        return compile_c(os.path.join(ROOT, 'port', name), os.path.join(OUT, 'port', name + '.o'), flags + extra)
    lib = os.path.join(ROOT, 'port', 'third_party', 'libmpeg2')

    def mpeg(name):
        return compile_c(os.path.join(lib, name + '.c'), os.path.join(OUT, 'libmpeg2', name + '.c.o'),
                         flags + ['-I', lib])
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        out = list(ex.map(one, units))
        out += list(ex.map(mpeg, ('alloc', 'cpu_accel', 'cpu_state', 'decode', 'header', 'idct', 'motion_comp',
                                  'slice')))
    return out


# ---- the functions that are still asm ----

RECOMP_FAILED = {}


def recompiled(objs):
    """Objects of the recompiled asm-only functions the link needs, to a fixed point (link.py)."""
    asm = link.asm_functions()
    in_data = set()
    for p in glob.glob(os.path.join(ROOT, 'asm', 'data', '*.s')):
        in_data |= set(re.findall(r'\bfunc_[0-9A-F]{8}\b', open(p, encoding='utf-8').read()))
    in_data |= set(recomp.data_entries())
    asm.update({n: e for n, (_, e) in recomp.data_entries().items()})
    # recomp.h includes no C library header: recompiled code calls the game's strcmp and the like
    flags = [f for f in port_cflags() if f not in ('-include', 'msvc_compat.h')]
    names, out = set(), []
    while True:
        defined = {s for v in nm(objs + out, '--defined-only', '-g').values() for s in v}
        wanted = in_data | {s.removeprefix('rc_') for v in nm(objs + out, '--undefined-only').values() for s in v}
        new = sorted(n for n in wanted - defined - names - link.LIBC
                     if n in asm and link.region(asm[n]) in ('game', 'engine', 'crt0', 'libgcc', 'sdk'))
        if not new:
            return out, sorted(names)
        names |= set(new)
        paths, failures = recomp.recompile(sorted(names))
        game = {n: e for n, e in failures.items() if link.region(asm[n]) != 'sdk'}
        if game:
            sys.exit('recompilation failed: %r' % game)
        RECOMP_FAILED.update(failures)
        with ThreadPoolExecutor(os.cpu_count()) as ex:
            out = list(ex.map(lambda c: compile_c(c, c + '.wasm.o', flags, '-O1'), paths))


# ---- the data image ----

FIXUP_LINE = re.compile(r'^\.long 0x([0-9A-F]+), (.+)$')
OPERAND = re.compile(r'^([A-Za-z_.$][\w.$]*)\s*(?:\+\s*(0x[0-9A-Fa-f]+|\d+))?$')


def fixups_c(wanted):
    """fixups.c: {PS2 address, value} for each image word the Mac changes. The image is
    assembled for the Windows target only to check it against the retail bytes (data.py)."""
    sources, labels, fixups = data.convert_all(wanted)
    objs = []
    for s in sources:
        p = run([CLANG, '--target=i686-pc-windows-msvc', '-c', s, '-o', s + '.o'])
        if p.returncode:
            sys.exit(p.stderr)
        objs.append(s + '.o')
    image_nm = run([tool('llvm-nm'), objs[0]]).stdout.splitlines()
    bad = data.check_image(objs[0], labels, image_nm)
    if bad:
        sys.exit('PS2 data image misplaced: %s' % '; '.join(bad[:10]))
    fixups_s, _ = data.write_fixups(objs[0], fixups, image_nm)
    retail = data.retail_image()
    entries, functions = [], set()
    for line in open(fixups_s, encoding='utf-8'):
        m = FIXUP_LINE.match(line.strip())
        if not m:
            continue
        address, operand = int(m.group(1), 16), m.group(2).strip()
        retail_word = int.from_bytes(retail[address - data.VUTEXT[0]:address - data.VUTEXT[0] + 4], 'little')
        if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', operand):
            value = int(operand, 0)
        else:
            o = OPERAND.match(operand)
            if not o:
                sys.exit('fixup 0x%X: operand %r' % (address, operand))
            target = wasmir.address(o.group(1))
            if target is not None and target in text_aliases():  # a forwarding label: its function
                target, operand = None, text_aliases()[target]
                o = OPERAND.match(operand)
            if target is None:  # a function: its table index
                functions.add(o.group(1))
                entries.append('{0x%X, (uint32_t)(uintptr_t)&%s%s}' % (
                    address, o.group(1), ' + %s' % o.group(2) if o.group(2) else ''))
                continue
            value = (target + int(o.group(2) or '0', 0)) & 0xFFFFFFFF
        if value != retail_word:
            entries.append('{0x%X, 0x%X}' % (address, value))
    path = os.path.join(OUT, 'fixups.c')
    with open(path, 'w', newline='\n') as f:
        f.write('/* Generated by port/tools/link_mac.py: the data image words the Mac build changes. */\n'
                '#include <stdint.h>\n')
        f.writelines('extern void %s(void);\n' % n for n in sorted(functions))
        f.write('const uint32_t ps2_fixups[][2] = {\n%s\n};\n' % ',\n'.join(entries))
        f.write('const uint32_t ps2_fixup_count = %d;\n' % len(entries))
    return path, len(entries), len(labels)


# ---- the module ----

def wasm_link(objs, extra=()):
    rsp = os.path.join(OUT, 'objs.rsp')
    with open(rsp, 'w', newline='\n') as f:
        f.write(''.join('"%s"\n' % o for o in list(objs) + list(extra)))
    return run([CLANG, '--target=' + pctarget.TARGET, '--sysroot=' + SYSROOT, '@' + rsp,
                '-o', os.path.join(OUT, 'dhd.wasm'),
                '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks', '-lwasi-emulated-mman',
                # the module's own memory from MODULE_BASE up; PS2 RAM below it (finding 075)
                '-Wl,--import-memory', '-Wl,--no-stack-first', '-Wl,--global-base=%d' % pctarget.MODULE_BASE,
                # The module's declared memory ends at MODULE_END, so its heap (the C library's
                # malloc, up to the memory's end) stays below the EE registers, VU memory, the RAM
                # mirrors and the scratchpad; the host maps those above it (native/space.c), and
                # wasm2c's guard pages don't bounds-check against the declared size.
                '-Wl,-z,stack-size=%d' % (8 << 20), '-Wl,--initial-memory=%d' % pctarget.MODULE_END,
                '-Wl,--max-memory=%d' % pctarget.MODULE_END, '-Wl,--export=mac_call', '-Wl,--export=mac_call_void',
                '-Wl,--export=__stack_pointer', '-Wl,--error-limit=0', '-Wl,-Map=' + os.path.join(OUT, 'dhd.map'),
                # the allocator under a lock (port/mac/malloc_lock.c)
                *['-Wl,--wrap=' + f for f in ('malloc', 'free', 'calloc', 'realloc', 'aligned_alloc', 'posix_memalign')]])


def main():
    os.makedirs(OUT, exist_ok=True)
    listing = os.path.join(OUT, 'objects.json')
    if '--relink' in sys.argv:  # the objects of the last run as they are: the link and wasm2c again
        objs, counts = json.load(open(listing))
        nrenamed, nrecompiled, nlabels, nfix = counts
    else:
        objs = sorted(glob.glob(os.path.join(OBJ, '**', '*.o'), recursive=True))
        if not objs:
            sys.exit('no objects in build/port-mac/obj: run DHD_PLATFORM=macos port/tools/build.py --out build/port-mac')
        objs, nrenamed = rename_cpp(objs)
        objs += runtime()
        robjs, rnames = recompiled(objs)
        objs += robjs
        nrecompiled = len(rnames)
        wanted = {s for v in nm(objs, '--undefined-only').values() for s in v}
        fix, nfix, nlabels = fixups_c(wanted)
        objs.append(compile_c(fix, fix + '.o', port_cflags()))
        json.dump([objs, [nrenamed, nrecompiled, nlabels, nfix]], open(listing, 'w'))
    objs = unify_declarations(objs)
    p = wasm_link(objs)

    undefined = sorted(set(re.findall(r'undefined symbol: (\S+)', p.stderr)))
    asm, lib = link.asm_functions(), link.library_functions()
    kinds = collections.defaultdict(list)
    for name in undefined:
        kinds[link.kind(name, asm, lib)].append(name)
    json.dump({'undefined': undefined, 'kinds': kinds}, open(os.path.join(OUT, 'census.json'), 'w'), indent=1)
    other = [l for l in p.stderr.splitlines() if 'error:' in l and 'undefined symbol' not in l]
    print('%d objects; %d image labels, %d fixups; %d C++ symbols renamed; %d asm-only functions recompiled' % (
        len(objs), nlabels, nfix, nrenamed, nrecompiled))
    print('%d undefined symbols, %d other errors' % (len(undefined), len(other)))
    for k, v in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print('%6d  %s   e.g. %s' % (len(v), k, ', '.join(v[:6])))
    for l in other[:20]:
        print(l)
    if undefined:  # each missing function names itself when called (kernel.c hle_missing)
        data_names = set()
        for path, syms in nm(objs, '--undefined-only').items():
            for s in set(syms) & set(undefined):
                if os.path.exists(ir_path(path)) and re.search(
                        r'^@"?%s"? = external' % re.escape(s), open(ir_path(path), encoding='utf-8').read(), re.M):
                    data_names.add(s)
        c = os.path.join(OUT, 'missing.c')
        with open(c, 'w', newline='\n') as f:
            f.write('/* Generated by port/tools/link_mac.py: what the link is missing. */\n'
                    'void hle_missing(const char *name);\n')
            f.writelines('char %s[64] __attribute__((aligned(16)));\n' % n for n in undefined if n in data_names)
            f.writelines('void %s(void) { hle_missing("%s"); }\n' % (n, n) for n in undefined if n not in data_names)
        p = wasm_link(objs, [compile_c(c, c + '.o', port_cflags())])
    if p.returncode:
        sys.exit(p.stderr[-5000:])
    print('linked %s (%.1f MB)' % (os.path.join(OUT, 'dhd.wasm'), os.path.getsize(os.path.join(OUT, 'dhd.wasm')) / 1e6))
    cdir = os.path.join(OUT, 'c')
    shutil.rmtree(cdir, ignore_errors=True)
    os.makedirs(cdir)
    p = run([shutil.which('wasm2c') or 'wasm2c', os.path.join(OUT, 'dhd.wasm'), '-n', 'dhd', '--enable-threads', '--num-outputs=32',
             '-o', os.path.join(cdir, 'dhd.c')])
    if p.returncode:
        sys.exit('wasm2c: ' + p.stderr[-3000:])
    relax_call_indirect(os.path.join(cdir, 'dhd-impl.h'))
    real_atomics(os.path.join(cdir, 'dhd-impl.h'))
    calls_on_own_instance(cdir)
    print('wasm2c: %d C files in %s' % (len(os.listdir(cdir)), cdir))


# The bodies of wasm2c's atomic operations on memory that isn't shared (after their alignment
# check): real atomics instead of plain copies.
ATOMIC_BODIES = {
    'DEFINE_ATOMIC_LOAD': ['t1 result = __atomic_load_n((t1 *)MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), __ATOMIC_SEQ_CST);',
                           't3 ret = (t3)(t2)result;', 'force_read(ret);', 'return ret;'],
    'DEFINE_ATOMIC_STORE': ['__atomic_store_n((t1 *)MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), (t1)value, __ATOMIC_SEQ_CST);'],
    'DEFINE_ATOMIC_RMW': ['return (t2)__atomic_##opname((t1 *)MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), (t1)value, '
                          '__ATOMIC_SEQ_CST);'],
    'DEFINE_ATOMIC_XCHG': ['return (t2)__atomic_exchange_n((t1 *)MEM_ADDR_MEMOP(mem, addr, sizeof(t1)), (t1)value, '
                           '__ATOMIC_SEQ_CST);'],
    'DEFINE_ATOMIC_CMP_XCHG': ['t2 expected_wrapped = (t2)expected;',
                               '__atomic_compare_exchange_n((t2 *)MEM_ADDR_MEMOP(mem, addr, sizeof(t2)), &expected_wrapped, '
                               '(t2)replacement, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);',
                               'return (t1)expected_wrapped;'],
}


def real_atomics(header):
    """wasm2c makes the atomic operations on memory that isn't shared plain copies: wasm says
    such memory has one thread. The module's has several (finding 075), and arm64 reorders
    plain accesses where x86 wouldn't, so they become real atomics."""
    text = open(header, encoding='utf-8').read()
    for macro, body in ATOMIC_BODIES.items():
        start = text.index('#define %s(' % macro)
        check = text.index('ATOMIC_ALIGNMENT_CHECK(addr, ', start)
        line_end = text.index('\n', check) + 1
        end = re.compile(r'^  \}[ \t]*\\$', re.M).search(text, line_end).start()  # the function's own brace
        if text.index('DEF_MEM_CHECKS', start) < end:
            sys.exit('%s: %s is not as link_mac.py expects' % (header, macro))
        text = text[:line_end] + ''.join('    %s \\\n' % line for line in body) + text[end:]
    open(header, 'w', encoding='utf-8').write(text)


TABLE_INSTANCE = re.compile(r'instance->w2c_T0\.data\[([^\]]+)\]\.module_instance')


def calls_on_own_instance(cdir):
    """A call through a function pointer runs on the caller's instance. wasm2c passes the instance
    the table entry was made with: the main thread's, whose stack pointer is the game's, so another
    thread's indirect calls put their frames on the game's stack (finding 075). There is one module,
    so the caller's instance is the function's; host functions in the table ignore theirs."""
    count = 0
    for name in os.listdir(cdir):
        if not name.endswith('.c'):
            continue
        path = os.path.join(cdir, name)
        text = open(path, encoding='utf-8').read()
        text, n = TABLE_INSTANCE.subn('instance', text)
        count += n
        open(path, 'w', encoding='utf-8').write(text)
    if not count:
        sys.exit('%s: no indirect calls with a table instance: wasm2c changed' % cdir)


TYPE_CHECK = '''  if (!expected_type || !func_ref->func || !func_ref->func_type ||
      memcmp(expected_type, func_ref->func_type, sha256size) != 0) {'''


def relax_call_indirect(header):
    """A call through a function pointer runs the function whatever its type, as on the PS2
    and x86: the game and the port call through casts (a thread's entry, callbacks, vtables).
    wasm2c's code passes the caller's arguments in the host's registers; only a missing function
    (a null or out-of-range table index) still traps."""
    text = open(header, encoding='utf-8').read()
    if TYPE_CHECK not in text:
        sys.exit('%s: wasm2c\'s call_indirect type check is not where link_mac.py expects it' % header)
    text = text.replace(TYPE_CHECK, '''  (void)expected_type; (void)sha256size;
  if (!func_ref->func) {
    dhd_bad_call(index, table->size);''')
    text = text.replace('''  if (index >= table->size) {
    // Table index out of bounds. Raise Wasm trap.''', '''  if (index >= table->size) {
    dhd_bad_call(index, table->size);''')
    text = text.replace('W2C_COLD_FUNC static bool func_types_eq_slowpath(',
                        'void dhd_bad_call(uint32_t index, uint32_t size);  /* port/mac/native/main.c */\n'
                        'W2C_COLD_FUNC static bool func_types_eq_slowpath(')
    open(header, 'w', encoding='utf-8').write(text)


if __name__ == '__main__':
    main()
