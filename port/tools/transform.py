"""Source transform for the PC build: rewrites one matching source file into host C/C++.

- MIPS inline asm statements become calls to the port runtime (port/runtime/ps2asm.c), which
  executes the instruction sequence on an emulated VU0/FPU/GPR register file;
- file-scope asm (`.extern`, `.globl`, INCLUDE_ASM) is dropped (port/include overrides INCLUDE_ASM);
- asm labels on declarations (`int x asm("D_x")`) stay: they only name the symbol;
- string literals with raw newlines (gcc 2.95) get `\\n` escapes;
- g++ `a <? b` / `a >? b` (minimum/maximum) become PS2_MIN(a, b) / PS2_MAX(a, b).

The matching sources are never modified; the output goes to a separate tree.
usage: python port/tools/transform.py SRC OUT
"""
import re, sys

ASM_WORD = re.compile(r'(?:asm|__asm|__asm__)\b')
CONT = '\\\n'  # backslash-newline: a line continuation
QUAL = re.compile(r'\s*(?:(?:volatile|__volatile|__volatile__|goto)\s*)*\(')


def scan(text):
    """Yield (kind, start, end) for every comment, string, char literal and code run."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            yield 'comment', i, j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            yield 'comment', i, j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            yield ('string' if c == '"' else 'char'), i, j
        else:
            j = i
            while j < n and text[j] not in '"\'' and not text.startswith(('//', '/*'), j):
                j += 1
            yield 'code', i, j
        i = j


def fix_strings(text):
    """Raw newlines inside string literals become \\n (a backslash-newline stays a continuation);
    the line breaks move to just after the literal so line numbers don't shift."""
    out = []
    for kind, a, b in scan(text):
        s = text[a:b]
        if kind == 'string' and '\n' in s:
            s, k = re.subn(r'(?<!\\)\n', r'\\n', s.replace('\\\n', '\0'))
            s = s.replace('\0', '\\\n') + '\n' * k
        out.append(s)
    return ''.join(out)


def code_mask(text):
    """Copy of text with comments, strings and chars blanked (same length), for structural search."""
    m = list(text)
    for kind, a, b in scan(text):
        if kind != 'code':
            for k in range(a, b):
                if m[k] != '\n':
                    m[k] = ' '
            if kind in ('string', 'char') and b - a >= 2:
                m[a] = m[b - 1] = '"'  # keep the delimiters visible
    return ''.join(m)


def close_paren(mask, i):
    depth = 0
    for j in range(i, len(mask)):
        if mask[j] in '([{':
            depth += 1
        elif mask[j] in ')]}':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced parenthesis at %d' % i)


def split_top(text, mask, a, b, sep):
    """Split text[a:b] at top-level occurrences of sep (':' skips '::')."""
    parts, depth, start, j = [], 0, a, a
    while j < b:
        ch = mask[j]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif depth == 0 and ch == sep:
            if sep == ':' and mask[j + 1:j + 2] == ':' and re.match(r'\w', mask[j + 2:j + 3]) \
                    and re.search(r'\w$', mask[a:j]):
                j += 2  # A::B scope operator (an empty asm section is `"..." :: "r"(x)`)
                continue
            parts.append((start, j))
            start = j + 1
        j += 1
    parts.append((start, b))
    return parts


def c_string(s):
    out = '"'
    for ch in s:
        if ch == '\\':
            out += '\\\\'
        elif ch == '"':
            out += '\\"'
        elif ch == '\n':
            out += '\\n'
        elif ch == '\t':
            out += '\\t'
        elif ch == '?':
            out += '\\?'  # no trigraphs
        else:
            out += ch
    return out + '"'


ESCAPES = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '?': '?', 'r': '\r'}


def literal_value(src):
    """Value of a run of adjacent C string literals."""
    def esc(e):
        g = e.group(1)
        if g[0] == 'x':
            return chr(int(g[1:], 16))
        if g[0] in '01234567':
            return chr(int(g, 8))
        return ESCAPES.get(g, g)
    vals = []
    for m in re.finditer(r'"((?:\\.|[^"\\])*)"', src, re.S):
        v = m.group(1).replace('\\\n', '')
        vals.append(re.sub(r'\\(x[0-9A-Fa-f]+|[0-7]{1,3}|.)', esc, v, flags=re.S))
    return ''.join(vals)


OPERAND = re.compile(r'\s*(?:\[\s*\w+\s*\]\s*)?((?:"(?:\\.|[^"\\])*"\s*)+)\((.*)\)\s*$', re.S)
NOOP_LINE = re.compile(r'(?:[A-Za-z_.$][\w.$]*:|nop|sync(?:\.[lp])?|\.set\s.*|)$')


def asm_statement(tmpl, outs, ins, expr=None):
    """Host replacement for one asm statement (without the trailing semicolon). expr: the
    template as a C expression when a macro builds it from its arguments."""
    lines = [l.strip() for l in re.split(r'[\n;]', tmpl)]
    if expr is None and all(NOOP_LINE.match(l) for l in lines):
        return '((void)0)'  # labels, nop, sync: no effect on the host
    decls, ops, cons = [], [], []
    for c, e in outs:
        cons.append(c)
        cast = re.match(r'\(\s*[A-Za-z_][\w\s]*\**\s*\)\s*(\(.*\)|\w+)$', e, re.S)
        if cast:
            e = cast.group(1)  # gcc 2.95 cast-as-lvalue: write the variable itself
        ops.append('{(void *)&(%s), sizeof(%s)}' % (e, e))
    for k, (c, e) in enumerate(ins):
        cons.append(c)
        if 'm' in c and not re.search(r'[rdfl0-9]', c):
            ops.append('{(void *)&(%s), sizeof(%s)}' % (e, e))  # memory operand: its address
        else:
            decls.append('PS2_INTYPE(%s) __i%d = (%s);' % (e, k, e))
            ops.append('{(void *)&__i%d, sizeof(__i%d)}' % (k, k))
    if not ops:
        return 'ps2_asm(%s, "", 0, 0)' % (expr or c_string(tmpl))
    return 'do { %s ps2_op __o[] = {%s}; ps2_asm(%s, %s, %d, __o); } while (0)' % (
        ' '.join(decls), ', '.join(ops), expr or c_string(tmpl), c_string('|'.join(cons)), len(ops))


def in_directive(mask, i):
    """True if position i is inside a preprocessor directive (e.g. a #define body)."""
    start = mask.rfind('\n', 0, i) + 1
    while start > 1 and mask[start - 2] == '\\':  # previous line continues into this one
        start = mask.rfind('\n', 0, start - 1) + 1
    return mask[start:i].lstrip().startswith('#')


def rewrite_asm(text):
    mask = code_mask(text)
    out, last, pos, depth = [], 0, 0, 0
    for m in ASM_WORD.finditer(mask):
        s = m.start()
        if s < pos or (s and (mask[s - 1].isalnum() or mask[s - 1] == '_')):
            continue
        q = QUAL.match(mask, m.end())
        if not q:
            continue
        depth += mask.count('{', pos, s) - mask.count('}', pos, s)
        lp = q.end() - 1
        rp = close_paren(mask, lp)
        sections = split_top(text, mask, lp + 1, rp, ':')
        tmpl_src = text[sections[0][0]:sections[0][1]].replace(CONT, ' ')  # #define continuations
        expr = None
        if not re.fullmatch(r'\s*(?:"(?:\\.|[^"\\])*"\s*)+', tmpl_src, re.S):
            if in_directive(mask, s) and re.fullmatch(r'\s*(?:(?:"(?:\\.|[^"\\])*"|#?\s*\w+)\s*)+', tmpl_src, re.S):
                expr = ' '.join(tmpl_src.split())  # built from macro arguments: concatenates after expansion
            else:
                pos = rp + 1  # e.g. INCLUDE_ASM's own definition: leave it
                depth += mask.count('{', s, pos) - mask.count('}', s, pos)
                continue
        tmpl = literal_value(tmpl_src)
        end = rp + 1
        depth += mask.count('{', s, end) - mask.count('}', s, end)
        if len(sections) == 1 and re.fullmatch(r'\s*[A-Za-z_.$][\w.$]*\s*', tmpl):
            pos = end
            continue  # asm("name") label on a declaration
        if depth <= 0 and not in_directive(mask, s):
            new = ''  # file-scope asm: assembler directives, nothing to do on the host
        else:
            def operands(k):
                if len(sections) <= k or not mask[sections[k][0]:sections[k][1]].strip():
                    return []
                res = []
                for pa, pb in split_top(text, mask, sections[k][0], sections[k][1], ','):
                    om = OPERAND.match(text[pa:pb].replace(CONT, ' '))
                    if not om:
                        raise ValueError('bad asm operand %r' % text[pa:pb])
                    res.append((literal_value(om.group(1)), om.group(2).strip()))
                return res
            new = asm_statement(tmpl, operands(1), operands(2), expr)
        lines = text.count('\n', s, end)  # keep line numbers: pad with as many line breaks
        out.append(text[last:s])
        out.append(new + (' \\\n' if in_directive(mask, s) else '\n') * lines)
        last = pos = end
    out.append(text[last:])
    return ''.join(out)


MINMAX = re.compile(r'[<>]\?(?!=)')
STOP = set(';,{}=?:|^&')


def _right_end(mask, j):
    depth = 0
    while j < len(mask):
        ch = mask[j]
        if ch in '([':
            depth += 1
        elif ch in ')]':
            if depth == 0:
                return j
            depth -= 1
        elif depth == 0:
            if MINMAX.match(mask, j):
                return j
            if ch in STOP:
                if mask[j:j + 2] in ('::', '=='):
                    j += 2
                    continue
                return j
        j += 1
    return j


def _left_start(mask, i):
    depth = 0
    while i >= 0:
        ch = mask[i]
        if ch in ')]':
            depth += 1
        elif ch in '([':
            if depth == 0:
                return i + 1
            depth -= 1
        elif depth == 0:
            if ch in STOP:
                if mask[i - 1:i + 1] in ('::', '==', '!=', '<=', '>='):
                    i -= 2
                    continue
                return i + 1
            if mask.startswith('return', i - 5) and not (mask[i - 6:i - 5].isalnum() or mask[i - 6:i - 5] == '_') \
                    and not (mask[i + 1:i + 2].isalnum() or mask[i + 1:i + 2] == '_'):
                return i + 1
        i -= 1
    return 0


def rewrite_minmax(text):
    """a <? b -> PS2_MIN(a, b); left-associative, binds looser than + - * / shifts and compares."""
    while True:
        mask = code_mask(text)
        m = MINMAX.search(mask)
        if not m:
            return text
        a, b = _left_start(mask, m.start() - 1), _right_end(mask, m.end())
        left, right = text[a:m.start()], text[m.end():b]
        fn = 'PS2_MIN' if mask[m.start()] == '<' else 'PS2_MAX'
        lead = left[:len(left) - len(left.lstrip())]
        trail = right[len(right.rstrip()):]
        text = text[:a] + lead + '%s(%s, %s)' % (fn, left.strip(), right.strip()) + trail + text[b:]


def rewrite_long(text):
    """SN EE long is 64 bits and long long is 128, including literal suffixes.
    Keep the host's long double spelling (both compilers use 64-bit double).
    """
    mask = code_mask(text)
    out, last = [], 0
    for m in re.finditer(r'\b(?:unsigned\s+|signed\s+)?long\s+long\b|\blong\b(?!\s+double)|(?<![\w.])((?:0[xX][0-9a-fA-F]+|\d+)(?:[uU]?[lL]{1,2}|[lL]{1,2}[uU]))\b',
                         mask):
        out.append(text[last:m.start()])
        if m.group(1):
            digits = re.match(r'0[xX][0-9a-fA-F]+|\d+', m.group(1)).group(0)
            unsigned = 'u' in m.group(1).lower()
            value = digits + ('U' if unsigned else '') + 'LL'
            if m.group(1).lower().count('l') == 2:
                value = '((%s)%s)' % ('ps2_uint128' if unsigned else 'ps2_int128', value)
            out.append(value)
        elif len(re.findall(r'\blong\b', m[0])) == 2:
            out.append('ps2_uint128' if m[0].startswith('unsigned') else 'ps2_int128')
        else:
            out.append('long long')
        last = m.end()
    out.append(text[last:])
    return ''.join(out)


def sound_signatures(text):
    """The 989snd bank-name and callback-data slots are 64-bit EE values.
    Some matching callers declare them as int: one EE register either way,
    but a different x86 stack layout. Normalize these audited declarations,
    including merged-unit aliases, before translating integer types.
    """
    mask = code_mask(text)
    edits = []
    signatures = {'00201B68': (7, (1,)), '00201BD0': (9, (1, 8)), '00202048': (5, (4,))}
    for m in re.finditer(r'\bfunc_(00201B68|00201BD0|00202048)\w*\s*\(', mask):
        a = m.end(); b = close_paren(mask, a - 1)
        parts = [text[x:y] for x, y in split_top(text, mask, a, b, ',')]
        count, wide = signatures[m[1]]
        if len(parts) != count or not re.fullmatch(r'\s*int(?:\s+\w+)?\s*', parts[0]): continue
        for i in wide:
            parts[i] = re.sub(r'^(\s*)int\b', r'\1long', parts[i])
        edits.append((a, b, ','.join(parts)))
    for a, b, replacement in reversed(edits): text = text[:a] + replacement + text[b:]
    return text


def name_anonymous_structs(text):
    """C++: an object of an unnamed struct type has no linkage, so `extern struct {...} D_x;` can't
    refer to another unit's definition (g++ 2.95 allowed it). Tag the struct after the object."""
    mask = code_mask(text)
    out, last = [], 0
    for m in re.finditer(r'\b(extern\s+(?:const\s+)?(?:struct|union))\s*\{', mask):
        end = close_paren(mask, m.end() - 1)
        name = re.match(r'\s*\**\s*(\w+)', mask[end + 1:])
        if not name:
            continue
        out.append(text[last:m.start()] + '%s %s_t ' % (m.group(1), name.group(1)))
        last = m.end() - 1
    out.append(text[last:])
    return ''.join(out)


TYPE_WORDS = {'int', 'char', 'short', 'long', 'unsigned', 'signed', 'float', 'double', 'void', 'bool',
              'const', 'volatile', 'struct', 'class', 'union', 'enum'}
VIRTUAL = re.compile(r'\bvirtual\s+([^;{}()]*?)\b(\w+)\s*\(')


def parameter(p, k):
    """(type, name) of one parameter declaration; unnamed ones get the name a<k>."""
    p = p.strip()
    m = re.match(r'^(.*?[\s*&])(\w+)\s*$', p)
    if m and m.group(2) not in TYPE_WORDS and set(re.findall(r'\w+', m.group(1))) - {'const', 'volatile', 'struct',
                                                                                  'class', 'union', 'enum'}:
        return m.group(1).strip(), m.group(2)
    return p, 'a%d' % k


def gcc_member_pointers(text):
    """Keep SN member-pointer tables and object fields in their original format.
    The current game declarations all take no explicit arguments.
    """
    pattern = r'\b(void|int)\s*\(\s*\w+\s*::\s*\*\s*(\w+(?:\s*\[[^]]*\])?)\s*\)\s*\(\s*\)'
    mask = code_mask(text)
    for m in reversed(list(re.finditer(pattern, mask))):
        text = text[:m.start()] + 'ps2_member<%s> %s' % (m[1], m[2]) + text[m.end():]
    mask = code_mask(text)
    calls = r'\(\s*(\w+)\s*->\*\s*([^()]+?)\s*\)\s*\(\s*\)'
    for m in reversed(list(re.finditer(calls, mask))):
        text = text[:m.start()] + '(%s).invoke(%s)' % (m[2], m[1]) + text[m.end():]
    return text


def gcc_virtuals(text):
    """C++: g++ 2.95 lays a polymorphic class out its own way: the vtable pointer after the data
    members (clang's ABI puts it first), and each vtable slot is {short delta, short index, void
    *function} from slot 1 on. The game's classes declare virtuals only to call them through the
    retail vtables (no unit defines one or constructs such an object, and none has a base), so
    each class gets the pointer as a member, ps2_vptr, and each virtual becomes an inline
    function calling slot k+1 (port.h PS2_VCALL)."""
    mask = code_mask(text)
    edits = []  # (start, end, replacement)
    for c in re.finditer(r'\b(?:class|struct)\s+(\w+)\s*(:[^{;]*)?\{', mask):
        end = close_paren(mask, c.end() - 1)
        body = mask[c.end():end]
        if not VIRTUAL.search(body):
            continue
        if c.group(2):
            raise ValueError('class %s: a polymorphic class with a base is not handled' % c.group(1))
        for k, v in enumerate(VIRTUAL.finditer(mask, c.end(), end)):
            close = close_paren(mask, v.end() - 1)
            semi = mask.index(';', close)
            quals = text[close + 1:semi].strip()
            ret, name = text[v.start(1):v.end(1)].strip(), v.group(2)
            params = [] if not text[v.end():close].strip() or text[v.end():close].strip() == 'void' else \
                [parameter(text[a:b], i) for i, (a, b) in enumerate(split_top(text, mask, v.end(), close, ','))]
            fn_type = '%s (*)(void *%s)' % (ret, ''.join(', ' + t for t, _ in params))
            body_text = '{ return PS2_VCALL(%s, %d)(PS2_VTHIS(%d)%s); }' % (
                fn_type, k, k, ''.join(', ' + n for _, n in params))
            decl = '%s %s(%s)%s %s' % (ret, name, ', '.join('%s %s' % p for p in params), ' ' + quals if quals else '',
                                       body_text)
            edits.append((v.start(), semi + 1, decl))
        edits.append((end, end, '\n    void *ps2_vptr; /* g++ 2.95: after the data members */\n'))
    for a, b, s in sorted(edits, reverse=True):
        text = text[:a] + s + text[b:]
    return text


def port_headers(source, output):
    """Shared headers can own the same legacy virtual declarations as a TU.
    Copy the include tree so quoted nested includes also use port layouts.
    Matching headers remain untouched; virtual declarations and native assembly
    symbol labels receive the same rewrites as source units.
    """
    from pathlib import Path
    from pctarget import LABEL_PREFIX
    source, output = Path(source), Path(output)
    for path in source.rglob('*'):
        if not path.is_file(): continue
        target = output / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix == '.h':
            text = path.read_text(encoding='utf-8', errors='replace')
            target.write_text(prefix_asm_labels(gcc_virtuals(text), LABEL_PREFIX), encoding='utf-8')
        else:
            target.write_bytes(path.read_bytes())


SECTION = re.compile(r'^/\* ---- (?:merged from )?\S+ ---- \*/$', re.M)
EXTERN_VAR = re.compile(r'^[ \t]*extern\b(?:\s+"[ C]")?[^;(),={}"]*?\b(\w+)\s*(?:\[[^\]]*\]\s*)*;', re.M)


def split_merged_globals(text):
    """A merged unit (tools/unit_merge.py) keeps each former file's own extern declarations, so one
    global can be redeclared with another type or linkage further down (g++ 2.95 let it pass).
    From each redeclaration to the end of its section the name becomes a section-local alias
    bound to the same symbol by an asm label."""
    mask = code_mask(text)
    bounds = [0] + [m.start() for m in SECTION.finditer(text)] + [len(text)]
    if len(bounds) == 2:
        return text
    seen = set()  # identifiers of the sections before this one
    edits = []  # (start, end, replacement)
    for n, (a, b) in enumerate(zip(bounds, bounds[1:])):
        decls = [m for m in EXTERN_VAR.finditer(mask, a, b) if '__asm' not in m.group(0)]
        for i, m in enumerate(decls):
            name = m.group(1)
            if name in seen or re.search(r'\b%s\b' % name, mask[a:m.start()]):
                alias = '%s__s%d_%d' % (name, n, i)
                stop = next((d.start() for d in decls[i + 1:] if d.group(1) == name), b)
                for u in re.finditer(r'\b%s\b' % name, mask[m.start():stop]):
                    edits.append((m.start() + u.start(), m.start() + u.end(), alias))
                edits.append((m.end() - 1, m.end() - 1, ' __asm__("%s")' % name))
        seen.update(re.findall(r'\b(\w+)\b', mask[a:b]))
    for s, e, r in sorted(edits, reverse=True):
        text = text[:s] + r + text[e:]
    return text


FUNC_LABEL = re.compile(r'\b(\w+)\s*\([^()]*\)\s*(?:asm|__asm|__asm__)\s*\(\s*"(\w+)"\s*\)')
DECL_START = re.compile(r'\s*(?:extern|static|inline|const|volatile|unsigned|signed|void|char|short|int|long|'
                        r'float|double|struct|union|enum|[A-Z]\w*)\b')


def hoist_asm_labels(text):
    """gcc 2.95 takes an asm label on a later redeclaration; clang wants it from the first
    declaration on. Copy each function's label onto every unlabeled declaration of it."""
    mask = code_mask(text)
    labels = dict(FUNC_LABEL.findall(text))
    edits = []
    for name, label in labels.items():
        for m in re.finditer(r'\b%s\s*\(' % name, mask):
            end = close_paren(mask, m.end() - 1)
            after = mask[end + 1:].lstrip()
            if not after or after[0] not in ';,' :
                continue  # a definition, a call or already labeled
            seg = mask[max(mask.rfind(c, 0, m.start()) for c in ';{}') + 1:m.start()]
            if DECL_START.match(seg) and '=' not in seg and not re.search(r'\breturn\b', seg):
                edits.append((end + 1, ' __asm__("%s")' % label))
    for pos, s in sorted(edits, reverse=True):
        text = text[:pos] + s + text[pos:]
    return text


def brace_depth(mask):
    depth, d = [], 0
    for ch in mask:
        if ch == '{':
            d += 1
        depth.append(d)
        if ch == '}':
            d -= 1
    return depth


FUNC_DEF = re.compile(r'^([A-Za-z_][\w \t\*]*?)\b(\w+)\s*\([^()]*\)\s*\{', re.M)


def declare_before_use(text):
    """C: a function called before any declaration is implicitly `int f()`, which conflicts with a
    later `void f(...)` definition (gcc 2.95 only warned). Declare it, unprototyped, before the
    top-level item holding the first call, on the same line."""
    mask = code_mask(text)
    depth = brace_depth(mask)
    # directives (with their continuation lines) blanked, so a macro body's braces don't count
    nodir = re.sub(r'^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), mask, flags=re.M)
    edits = []
    for d in FUNC_DEF.finditer(mask):
        ret, name = d.group(1).strip(), d.group(2)
        if depth[d.start()] or ret in ('return', 'else') or re.search(r'\bstatic\b', ret):
            continue
        uses = [u for u in re.finditer(r'\b%s\s*\(' % name, mask[:d.start()])]
        if not uses:
            continue
        first = uses[0].start()
        seg = nodir[max(nodir.rfind(c, 0, first) for c in ';{}') + 1:first]
        if DECL_START.match(seg) and '=' not in seg:
            continue  # declared before it is used
        q = max((i for i in range(first) if depth[i] == 0 and mask[i] in ';}'), default=-1)
        edits.append((q + 1, '%s %s();' % (re.sub(r'\b(?:extern|inline)\b', '', ret).strip(), name)))
    for pos, s in sorted(edits, reverse=True):
        text = text[:pos] + s + text[pos:]
    return text


BLOCK_EXTERN = re.compile(r'\bextern\b[^;{}()"]*;')


def alias_block_externs(text):
    """Block-scope `extern T x;` whose type disagrees with the file's own definition of x (gcc 2.95
    let it pass): give the block-scope name an alias bound to the same symbol by an asm label."""
    mask = code_mask(text)
    depth = brace_depth(mask)
    top = ''.join(ch if depth[i] == 0 else ' ' for i, ch in enumerate(mask))
    # a file-scope declaration's own asm label is the symbol (patches.py may give one)
    labels = {m.group(1): m.group(2) for m in re.finditer(
        r'\b(\w+)\s*(?:\[[^\]]*\]\s*)*(?:__asm__|__asm|asm)\s*\(\s*"([\w.$]+)"\s*\)', text) if depth[m.start()] == 0}
    edits = []
    for m in BLOCK_EXTERN.finditer(mask):
        if depth[m.start()] == 0:
            continue
        end = close_paren(mask, max(i for i in range(m.start()) if mask[i] == '{' and depth[i] == depth[m.start()]))
        body = m.group(0)[:-1]
        for dm in re.finditer(r'(\w+)\s*((?:\[[^\]]*\]\s*)*)(?=,|$)', body):
            name = dm.group(1)
            if not re.search(r'\b%s\b' % name, top):
                continue  # not declared at file scope (possibly through a macro)
            alias = name + '__b'
            for u in re.finditer(r'\b%s\b' % name, mask[m.start():end]):
                edits.append((m.start() + u.start(), m.start() + u.end(), alias))
            edits.append((m.start() + dm.end(), m.start() + dm.end(), ' __asm__("%s")' % labels.get(name, name)))
    for s, e, r in sorted(edits, key=lambda x: (x[0], x[1]), reverse=True):
        text = text[:s] + r + text[e:]
    return text


ASM_LABEL = re.compile(r'\b(__asm__|__asm|asm)(\s*\(\s*")([A-Za-z_.$][\w.$]*)("\s*\))')


def prefix_asm_labels(text, prefix):
    """Win32 C symbols carry a leading underscore, which asm labels bypass: put it in the label
    too, so a labelled declaration still meets the plain C name and the converted data. Runs
    after rewrite_asm, so any asm("name") left is a label."""
    return ASM_LABEL.sub(lambda m: m.group(1) + m.group(2) + prefix + m.group(3) + m.group(4), text)


LABEL_IN = re.compile(r'\b(?:__asm__|__asm|asm)\s*\(\s*"([\w.$]+)"\s*\)')


def _array_count(init, mask):
    """Element count of an array initializer ({a, b, c} or "string"), or None."""
    init = init.strip()
    if init.startswith('"'):
        return len(literal_value(init)) + 1 if re.fullmatch(r'"(?:[^"\\]|\\.)*"', init, re.S) else None
    if not init.startswith('{'):
        return None
    inner = mask[1:len(init) - 1] if init.endswith('}') else None
    if inner is None:
        return None
    parts = [p for p in split_top(inner, inner, 0, len(inner), ',')]
    if parts and not inner[parts[-1][0]:parts[-1][1]].strip():
        parts = parts[:-1]  # trailing comma
    return len(parts)


def extern_image_data(text, image):
    """The PS2 data image (port/tools/data.py) holds every global the retail ELF has, at its PS2
    address. A file-scope definition of one of them (by name or by asm label) becomes an extern
    declaration, so there is one copy, the image's. An initialized `T x[] = {...}` keeps its
    element count. A declaration that also defines other, non-image variables is an error.
    An alias of an image global (a second name for one address) becomes an asm label naming it."""
    text = re.sub(r'__attribute__\s*\(\(\s*alias\s*\(\s*"(\w+)"\s*\)\s*\)\)',
                  lambda m: '__asm__("%s")' % m.group(1) if m.group(1) in image else m.group(0), text)
    mask = code_mask(text)
    # the braces of extern "C" { } and namespace { } blocks don't nest: their items are file scope
    blocks = list(mask)
    for m in re.finditer(r'\b(?:extern\s*"[^"\n]*"|namespace(?:\s+\w+)?)\s*\{', mask):  # "C" is blanked
        o = m.end() - 1
        # Flatten the whole header: leaving `extern "C"` before the first
        # declaration makes that definition look like an existing extern.
        for i in range(m.start(), o + 1):
            if blocks[i] != '\n': blocks[i] = ' '
        blocks[close_paren(mask, o)] = ' '
    blocks = ''.join(blocks)
    depth = brace_depth(blocks)
    nodir = re.sub(r'^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), blocks, flags=re.M)
    edits, start, paren, body = [], 0, 0, False
    for i, ch in enumerate(nodir):
        if ch == '{' and depth[i] == 1:
            # a function body (or namespace/extern "C" block) ends the item at its '}'; the braces
            # of an initializer or a struct definition are inside the declaration
            seg = nodir[start:i]
            body = '=' not in seg and not re.search(r'\b(?:struct|union|enum|class)\b[^;()]*$', seg)
        if depth[i] or ch == '}':
            if ch == '}' and depth[i] == 1 and body:
                start, body = i + 1, False
            continue
        if ch == '(':
            paren += 1
        elif ch == ')':
            paren -= 1
        if ch != ';' or paren:
            continue
        a, b, start = start, i, i + 1
        head = nodir[a:b].lstrip()
        a = b - len(head)
        if not head or re.match(r'(?:typedef|static|template|using|namespace)\b', head) \
                or re.match(r'extern\b', head) and '=' not in head \
                or re.match(r'(?:struct|union|enum|class)\s+\w*\s*\{[^;]*\}\s*$', head) or '{' == head[:1]:
            continue
        found, others = [], False
        for pa, pb in split_top(text, mask, a, b, ','):
            eq = next((j for j in range(pa, pb) if mask[j] == '=' and depth[j] == 0
                       and nodir[pa:j].count('(') == nodir[pa:j].count(')')
                       and mask[j + 1] != '=' and mask[j - 1] not in '!<>='), None)
            decl = text[pa:eq if eq is not None else pb]
            names = [n for n in re.findall(r'\b[A-Za-z_]\w*\b', COMMENT_RE.sub('', decl)) if n in image]
            label = LABEL_IN.search(decl)
            if names or (label and label.group(1) in image):
                found.append((pa, pb, eq))
            elif re.search(r'\w', mask[pa:pb]):
                others = True
        if not found:
            continue
        if others:
            raise ValueError('declaration defines image and non-image variables: %r' % text[a:b][:80])
        for pa, pb, eq in found:
            # weak/section/aligned only mattered for the definition (weak would still define it)
            for at in re.finditer(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)', mask[pa:eq if eq is not None else pb]):
                edits.append((pa + at.start(), pa + at.end(), ''))
            if eq is not None:
                decl = text[pa:eq]
                if re.search(r'\[\s*\]\s*(?:__asm__\s*\("[^"]*"\)\s*)?$', decl.rstrip()):
                    n = _array_count(text[eq + 1:pb], mask[eq + 1:pb].strip())
                    if n is None:
                        raise ValueError('array size of %r unknown' % decl.strip()[:60])
                    k = decl.rindex('[')
                    edits.append((pa + k + 1, pa + decl.index(']', k), str(n)))
                edits.append((eq, pb, ''))
        if not re.match(r'extern\b', head):
            edits.append((a, a, 'extern '))
    for s, e, r in sorted(edits, key=lambda x: (x[0], x[1]), reverse=True):
        text = text[:s] + r + text[e:]
    return text


COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def dma_stores(text):
    """Start DMA at CHCR.STR, before another launch or a reused packet can replace it.
    Match literal MMIO stores and the two lvalue-macro forms used by the game.
    Recompiled SDK stores use the same helper (recomp.py).
    """
    addr = r'0x1000(?:8000|9000|[aA]000|[bB]000|[bB]400|[cC]000|[cC]400|[cC]800|[dD]000|[dD]400)'
    macros = {}
    for m in re.finditer(r'^#define\s+(\w+)\s+([^\n]+)', text, re.M):
        a = re.search(addr, m[2])
        if a and '*' in m[2]: macros[m[1]] = a[0]
    literal = r'\*\s*\(\s*volatile\s+(?:unsigned\s+)?int\s*\*\s*\)\s*(' + addr + ')'
    call = r'\b[A-Z_]+\(\s*(' + addr + r')\s*\)'
    names = '|'.join(re.escape(n) for n in macros) or r'(?!)'
    pattern = r'(?:' + literal + '|' + call + r'|\b(' + names + r')\b)\s*=(?!=)\s*([^;]+);'
    def replace(m):
        a = m[1] or m[2] or macros[m[3]]
        return 'ps2_store32(' + a + ', ' + m[4] + ');'
    return re.sub(pattern, replace, text)


_abi_sites = None


def fix_abi_calls(text, relative, sites=None):
    """Apply the audited EE register-bank permutations before other rewrites.

    Cast the callee too: merely swapping expressions under the old prototype
    would convert them back to the wrong types. Preserve the local pointer
    types and return type. Source guards make stale line numbers a build error.
    """
    import hashlib
    import json
    from pathlib import Path
    if sites is None:
        global _abi_sites
        if _abi_sites is None:
            path = Path(__file__).resolve().parents[1] / 'abi_calls.json'
            _abi_sites = json.loads(path.read_text())['sites']
        sites = _abi_sites
    sites = [s for s in sites if s['file'] == relative]
    if not sites: return text
    data = text.encode('utf-8')
    starts = [0] + [m.end() for m in re.finditer(b'\n', data)]
    edits = []
    for site in sites:
        if site['file'] != relative: continue
        start = starts[site['line'] - 1] + site['column'] - 1
        end = start + site['length']
        old = data[start:end]
        if hashlib.sha256(old).hexdigest() != site['sha256']:
            raise ValueError('stale ABI site %s:%d; run port/tools/abi_check.py' % (relative, site['line']))
        edits.append((start, end, site))
    # Nested calls must be rendered recursively, not overwritten by the outer
    # replacement. The manifest ranges refer to the unchanged original text.
    def render(a, b, exclude=None):
        children = [e for e in edits if e is not exclude and a <= e[0] and e[1] <= b]
        top = [e for e in children if not any(o is not e and o[0] <= e[0] and e[1] <= o[1] for o in children)]
        result, pos = [], a
        for e in sorted(top, key=lambda e: e[0]):
            s, end, site = e
            result.append(data[pos:s])
            parts = [render(s + x, s + y, e) for x, y in site['parts']]
            types = ', '.join(site['types'][i] for i in site['order'])
            cast = ('((' + site['ret'] + ' (*)(' + types + '))').encode()
            replacement = cast + parts[0] + b')(' + b', '.join(parts[i + 1] for i in site['order']) + b')'
            # Keep subsequent source locations stable even for multiline calls.
            replacement += b'\n' * max(0, data[s:end].count(b'\n') - replacement.count(b'\n'))
            result.append(replacement)
            pos = end
        result.append(data[pos:b])
        return b''.join(result)
    return render(0, len(data)).decode('utf-8')


def host_types(text):
    """Spell the PS2 va_list and size_t as the host's types: char * and unsigned int on 32-bit
    Windows, but distinct types on wasm32 (port/tools/pctarget.py), where clang's builtins
    insist on them."""
    text = text.replace('typedef char *va_list;', 'typedef __builtin_va_list va_list;')
    return re.sub(r'(\boperator new(?:\[\])?\()unsigned int\b', r'\1__SIZE_TYPE__', text)


def transform(text, cpp=False, label_prefix='', image=None):
    text = host_types(text)
    text = rewrite_long(sound_signatures(rewrite_minmax(rewrite_asm(fix_strings(text)))))
    text = dma_stores(text)
    text = hoist_asm_labels(split_merged_globals(text))
    if not cpp:
        text = alias_block_externs(declare_before_use(text))
    if cpp:
        text = gcc_virtuals(gcc_member_pointers(name_anonymous_structs(text)))
    if image:
        text = extern_image_data(text, image)
    return prefix_asm_labels(text, label_prefix) if label_prefix else text


if __name__ == '__main__':
    src, out = sys.argv[1:3]
    t = open(src, encoding='utf-8', errors='replace').read()
    open(out, 'w', encoding='utf-8', newline='\n').write(
        '#include "port.h"\n#line 1 "%s"\n' % src.replace('\\', '/') + transform(t))
