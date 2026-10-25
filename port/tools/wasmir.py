"""Mac build (DHD_PLATFORM=macos, finding 075): LLVM IR rewrites for wasm32 units.

A wasm object can't define an absolute symbol, so the PS2 data image's labels (absolute symbols
on Windows, port/tools/data.py symbols.s) can't be linked. On the Mac the wasm linear memory is
the PS2 address space (PS2_RAM_BASE 0), so a label's address is its PS2 address: every external
global that names one becomes that number in the unit's IR, and its declaration goes.

llvm-objcopy can't rename wasm symbols either, so the C++ retail names (link.py rename_cpp) are
applied to the IR as well, and the unit is compiled again."""
import functools, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data  # noqa: E402

EXTERNAL = re.compile(r'^@("?)([-\w.$]+)\1 = external (?:(?:dso_local|hidden|protected|default|local_unnamed_addr|unnamed_addr) )*(?:constant|global) [^\n]*\n', re.M)
HEX_NAME = re.compile(r'^(?:D_|\.L)([0-9A-F]+)$')


@functools.lru_cache(maxsize=None)
def image_labels():
    labels = data.image_symbols()
    labels['_gp'] = data.GP
    return labels


def address(name):
    """The PS2 address an external global stands for, or None (a real symbol)."""
    labels = image_labels()
    if name in labels:
        return labels[name]
    m = HEX_NAME.match(name)
    if m:
        addr = int(m.group(1), 16)
        if addr & 0xCFFFFFFF < data.RAM_END or data.SCRATCHPAD[0] <= addr < data.SCRATCHPAD[1]:
            return addr
    return None


def _symbol(name):
    return re.compile(r'@(?:"%s"|%s)(?![-\w.$])' % (re.escape(name), re.escape(name)))


CODE_NAME = re.compile(r'^func_[0-9A-F]{8}$')


def absolute_globals(ir):
    """The IR with each image label replaced by its PS2 address. A function the C declares as an
    array (extern char func_X[], for its address) becomes a function: in wasm its address is its
    table index, and the linker won't take a function for data."""
    names = {}
    for m in EXTERNAL.finditer(ir):
        a = address(m.group(2))
        if a is not None:
            names[m.group(2)] = a
    ir = EXTERNAL.sub(lambda m: 'declare void @%s()\n' % m.group(2)
                      if m.group(2) not in names and CODE_NAME.match(m.group(2)) else m.group(0), ir)
    if not names:
        return ir
    ir = EXTERNAL.sub(lambda m: '' if m.group(2) in names else m.group(0), ir)
    pattern = re.compile(r'@("?)(%s)\1(?![-\w.$])' % '|'.join(re.escape(n) for n in names))
    return pattern.sub(lambda m: 'inttoptr (i32 %d to ptr)' % names[m.group(2)], ir)


def rename(ir, mapping):
    """The IR with symbols renamed ({old: new}); only names that occur are touched."""
    found = {n for n in mapping if '@' + n in ir or '@"' + n + '"' in ir}
    if not found:
        return ir
    pattern = re.compile(r'@("?)(%s)\1(?![-\w.$])' % '|'.join(re.escape(n) for n in sorted(found, key=len, reverse=True)))
    ir = pattern.sub(lambda m: '@' + mapping[m.group(2)], ir)
    # Two C++ names can get one retail name (a function of another unit, declared in another
    # namespace): one symbol, as in a COFF object, so the declaration goes where it is defined.
    defined = set(DEFINE.findall(ir))
    seen = set()

    def declaration(m):
        name = m.group(1)
        if name in defined or name in seen:
            return ''
        seen.add(name)
        return m.group(0)
    return DECLARE.sub(declaration, ir)


DEFINE = re.compile(r'^define [^@\n]*@([-\w.$]+)\(', re.M)
DECLARE = re.compile(r'^declare [^@\n]*@([-\w.$]+)\([^\n]*\n', re.M)
