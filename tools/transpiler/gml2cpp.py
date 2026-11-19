#!/usr/bin/env python3
"""GML (Game Maker 7) -> C++ transpiler for the Tower of Heaven port.

Lexing and parsing mirror OpenGMK's gml-parser (lexer.rs / ast.rs) so that
GM's quirks are reproduced exactly:
  * '=' inside an expression is equality; 'and'/'or'/'xor' share precedence 0
  * unary operators bind tighter than everything (not a = b  ->  (not a) = b)
  * 'begin'/'end' are braces, 'then' is ignored, ':=' is assignment
Semantics follow OpenGMK's runtime: no short-circuit and/or, operands evaluated
left to right (we use braced initialisers, which C++ evaluates in order).

usage: gml2cpp.py <extracted/project> <out_dir>
writes out_dir/gml_code.cpp and out_dir/gml_ids.h
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- lexer

KEYWORDS = {'var', 'globalvar', 'if', 'else', 'with', 'repeat', 'do', 'until', 'while', 'for',
            'switch', 'case', 'default', 'break', 'continue', 'return', 'exit'}
WORD_OPS = {'mod': 'mod', 'div': 'div', 'and': '&&', 'or': '||', 'xor': '^^', 'not': '!'}
SINGLE = {'!': 'op', '&': 'op', '(': 'sep', ')': 'sep', '*': 'op', '+': 'op', ',': 'sep',
          '-': 'op', '/': 'op', ':': 'sep', ';': 'sep', '<': 'op', '=': 'op', '>': 'op',
          '[': 'sep', ']': 'sep', '^': 'op', '{': 'sep', '|': 'op', '}': 'sep', '~': 'op'}
DOUBLED = {'&': '&&', '|': '||', '^': '^^', '<': '<<', '>': '>>', '=': '=='}
EQ_COMBO = {'!': '!=', '<': '<=', '>': '>=', '+': '+=', '-': '-=', '*': '*=', '/': '/=',
            '&': '&=', '|': '|=', '^': '^='}


class Tok:
    __slots__ = ('kind', 'val', 'line')

    def __init__(self, kind, val, line):
        self.kind, self.val, self.line = kind, val, line

    def __eq__(self, o):
        return isinstance(o, Tok) and self.kind == o.kind and self.val == o.val

    def __repr__(self):
        return f'{self.kind}:{self.val!r}'


def lex(src):
    toks, i, n, line = [], 0, len(src), 1
    while True:
        while i < n and src[i] <= ' ':
            line += src[i] == '\n'
            i += 1
        if i >= n:
            break
        c = src[i]
        if c.isalpha() or c == '_':
            j = i
            while j < n and (src[j].isalnum() or src[j] == '_') and src[j].isascii():
                j += 1
            w = src[i:j]
            i = j
            if w in KEYWORDS:
                toks.append(Tok('kw', w, line))
            elif w in WORD_OPS:
                toks.append(Tok('op', WORD_OPS[w], line))
            elif w == 'then':
                toks.append(Tok('sep', 'then', line))
            elif w == 'begin':
                toks.append(Tok('sep', '{', line))
            elif w == 'end':
                toks.append(Tok('sep', '}', line))
            else:
                toks.append(Tok('id', w, line))
        elif c.isdigit() or c == '.':
            point = False
            if c == '.':
                i += 1
                if i < n and src[i].isdigit():
                    point = True
                else:
                    toks.append(Tok('sep', '.', line))
                    continue
            result, factor = 0.0, 1.0
            while i < n and (src[i].isdigit() or src[i] == '.'):
                if src[i] == '.':
                    point = True
                else:
                    if point:
                        factor /= 10.0
                    result = result * 10.0 + (ord(src[i]) - 48)
                i += 1
            toks.append(Tok('real', result * factor, line))
        elif c in '"\'':
            j = src.find(c, i + 1)
            j = n if j < 0 else j
            toks.append(Tok('str', src[i + 1:j], line))
            line += src.count('\n', i, j)
            i = j + 1
        elif c == '$':
            j = i + 1
            while j < n and src[j] in '0123456789abcdefABCDEF':
                j += 1
            toks.append(Tok('real', float(int(src[i + 1:j], 16)) if j > i + 1 else 0.0, line))
            i = j
        elif c in SINGLE:
            kind = SINGLE[c]
            c2 = src[i + 1] if i + 1 < n else ''
            i += 1
            if kind == 'op':
                if c2 == c and c in DOUBLED:
                    toks.append(Tok('op', DOUBLED[c], line))
                    i += 1
                elif c2 == c == '/':
                    while i < n and src[i] not in '\r\n':
                        i += 1
                elif c2 == '=' and c in EQ_COMBO:
                    toks.append(Tok('op', EQ_COMBO[c], line))
                    i += 1
                elif c == '/' and c2 == '*':
                    j = src.find('*/', i + 1)
                    line += src.count('\n', i, n if j < 0 else j)
                    i = n if j < 0 else j + 2
                elif c == '<' and c2 == '>':
                    toks.append(Tok('op', '!=', line))
                    i += 1
                else:
                    toks.append(Tok('op', c, line))
            elif c == ':' and c2 == '=':
                toks.append(Tok('op', '=', line))
                i += 1
            else:
                toks.append(Tok('sep', c, line))
        else:
            raise SyntaxError(f'line {line}: invalid char {c!r}')
    return toks

# ---------------------------------------------------------------- parser
# AST nodes are tuples: (kind, ...)

PREC = {'+': 4, '-': 4, '*': 5, '/': 5, 'div': 5, 'mod': 5, '&': 2, '|': 2, '^': 2,
        '<': 1, '>': 1, '==': 1, '!=': 1, '<=': 1, '>=': 1, '&&': 0, '||': 0, '^^': 0,
        '<<': 3, '>>': 3}
ASSIGN_OPS = {'=', '+=', '-=', '*=', '/=', '&=', '|=', '^='}


class Parser:
    def __init__(self, toks):
        self.t, self.p = toks, 0

    def peek(self, k=0):
        return self.t[self.p + k] if self.p + k < len(self.t) else None

    def next(self):
        tok = self.peek()
        self.p += 1
        return tok

    def is_(self, kind, val, k=0):
        tok = self.peek(k)
        return tok is not None and tok.kind == kind and tok.val == val

    def expect(self, kind, val):
        tok = self.next()
        if tok is None or tok.kind != kind or tok.val != val:
            raise SyntaxError(f'expected {val!r}, got {tok}')

    def skip_semis(self):
        while self.is_('sep', ';'):
            self.p += 1

    def program(self):
        out = []
        while True:
            s = self.line()
            if s is None:
                return ('group', out)
            out.append(s)

    def group(self):
        if self.is_('sep', ';'):
            self.skip_semis()
            return ('group', [])
        if self.peek() is None:
            return None
        return self.line()

    def line(self):
        self.skip_semis()
        tok = self.next()
        if tok is None:
            return None
        r = self._line(tok)
        self.skip_semis()
        return r

    def _line(self, tok):
        if tok.kind == 'kw':
            k = tok.val
            if k in ('var', 'globalvar'):
                names = []
                if self.peek() and self.peek().kind == 'id':
                    names.append(self.next().val)
                    while True:
                        nx = self.peek()
                        if nx and nx.kind == 'sep' and nx.val == ',':
                            self.p += 1
                        elif nx and nx.kind == 'id':
                            after = self.peek(1)
                            if after and after.kind == 'sep' and after.val in ('(', '.'):
                                break
                        else:
                            break
                        if self.peek() and self.peek().kind == 'id':
                            names.append(self.next().val)
                        else:
                            break
                return (k, names)
            if k == 'do':
                body = self.group()
                self.expect('kw', 'until')
                return ('do', body, self.expr())
            if k == 'if':
                cond = self.expr()
                if self.is_('sep', 'then'):
                    self.p += 1
                body = self.group()
                els = None
                if self.is_('kw', 'else'):
                    self.p += 1
                    els = self.group()
                return ('if', cond, body, els)
            if k == 'for':
                self.expect('sep', '(')
                start = self.line()
                if self.is_('sep', ';'):
                    self.p += 1
                cond = self.expr()
                if self.is_('sep', ';'):
                    self.p += 1
                step = self.line()
                self.skip_semis()
                self.expect('sep', ')')
                return ('for', start, cond, step, self.group())
            if k == 'repeat':
                return ('repeat', self.expr(), self.group())
            if k == 'switch':
                return ('switch', self.expr(), self.line())
            if k == 'with':
                tgt = self.expr()
                if self.is_('kw', 'do'):
                    self.p += 1
                return ('with', tgt, self.group())
            if k == 'while':
                cond = self.expr()
                if self.is_('kw', 'do'):
                    self.p += 1
                return ('while', cond, self.group())
            if k == 'case':
                e = self.expr()
                self.expect('sep', ':')
                return ('case', e)
            if k == 'default':
                self.expect('sep', ':')
                return ('default',)
            if k in ('break', 'continue', 'exit'):
                return (k,)
            if k == 'return':
                return ('return', self.expr())
            raise SyntaxError(f'line {tok.line}: unexpected keyword {k}')
        if tok.kind == 'id':
            if self.is_('sep', '('):
                return ('callstmt', self.call(tok.val))
            return self.tree(tok, True)
        if tok.kind == 'sep' and tok.val == '{':
            body = []
            while True:
                if self.is_('sep', '}'):
                    self.p += 1
                    return ('group', body)
                s = self.line()
                if s is None:
                    raise SyntaxError('unclosed brace')
                body.append(s)
        if tok.kind == 'sep' and tok.val == '(':
            return self.tree(tok, True)
        raise SyntaxError(f'line {tok.line}: unexpected token {tok}')

    def call(self, name):
        self.expect('sep', '(')
        args = []
        if self.is_('sep', ')'):
            self.p += 1
        else:
            while True:
                args.append(self.expr())
                t = self.next()
                if t and t.kind == 'sep' and t.val == ')':
                    break
                if t and t.kind == 'sep' and t.val == ',':
                    if self.is_('sep', ')'):
                        self.p += 1
                        break
                    continue
                raise SyntaxError(f'bad token in call {name}: {t}')
        return ('call', name, args)

    def expr(self):
        return self.tree(None, False)

    def tree(self, first, expect_assign):
        val, op = self.tree_rec(first, expect_assign, 0)
        if op is not None:
            raise SyntaxError(f'stray operator {op}')
        return val

    def tree_rec(self, first, expect_assign, lowest):
        lhs = self.operand(first)
        nt = self.peek()
        if not (nt and nt.kind == 'op'):
            if expect_assign:
                raise SyntaxError(f'line {nt.line if nt else "?"}: expected assignment, got {nt}')
            return lhs, None
        op = nt.val
        if op == '=' and not expect_assign:
            op = '=='
        self.p += 1
        while True:
            if op in PREC:
                if expect_assign:
                    raise SyntaxError(f'operator {op} where assignment expected')
                prec = PREC[op]
                if prec < lowest:
                    return lhs, op
                rhs, nop = self.tree_rec(None, False, prec + 1)
                if nop is None:
                    return ('bin', op, lhs, rhs), None
                if PREC[nop] < lowest:
                    return ('bin', op, lhs, rhs), nop
                lhs, op = ('bin', op, lhs, rhs), nop
            else:
                if not expect_assign or op in ('!', '~'):
                    raise SyntaxError(f'invalid operator {op}')
                rhs, stray = self.tree_rec(None, False, lowest)
                if stray:
                    raise SyntaxError(f'stray operator {stray}')
                return ('assign', op, lhs, rhs), None

    def operand(self, first):
        t = first if first is not None else self.next()
        if t is None:
            raise SyntaxError('unexpected EOF in expression')
        if t.kind == 'sep' and t.val == '(':
            e = self.expr()
            self.expect('sep', ')')
            lhs = e
        elif t.kind == 'op':
            if t.val in ('+', '-', '!', '~'):
                return ('un', t.val, self.operand(None))
            raise SyntaxError(f'invalid unary {t.val}')
        elif t.kind == 'id':
            lhs = self.call(t.val) if self.is_('sep', '(') else ('id', t.val)
        elif t.kind == 'real':
            lhs = ('real', t.val)
        elif t.kind == 'str':
            lhs = ('str', t.val)
        else:
            raise SyntaxError(f'line {t.line}: invalid token {t} in expression')
        while True:
            if self.is_('sep', '['):
                self.p += 1
                dims = []
                if self.is_('sep', ']'):
                    self.p += 1
                else:
                    while True:
                        dims.append(self.expr())
                        t2 = self.next()
                        if t2 and t2.val == ']':
                            break
                        if t2 and t2.val == ',':
                            if self.is_('sep', ']'):
                                self.p += 1
                                break
                            continue
                        raise SyntaxError(f'bad array index token {t2}')
                lhs = ('index', lhs, dims)
            elif self.is_('sep', '.'):
                self.p += 1
                t2 = self.next()
                if not t2 or t2.kind != 'id':
                    raise SyntaxError('expected identifier after .')
                lhs = ('deref', lhs, t2.val)
            else:
                return lhs

# ---------------------------------------------------------------- codegen

BINOPS = {'+': 'add', '-': 'sub', '*': 'mul', '/': 'div', 'div': 'idiv', 'mod': 'mod',
          '&': 'band', '|': 'bor', '^': 'bxor', '<<': 'shl', '>>': 'shr', '==': 'eq',
          '!=': 'ne', '<': 'lt', '<=': 'le', '>': 'gt', '>=': 'ge', '&&': 'and', '||': 'or',
          '^^': 'xor'}
UNOPS = {'-': 'neg', '!': 'not', '~': 'compl', '+': 'pos'}
ASSIGN = {'=': 'set', '+=': 'add', '-=': 'sub', '*=': 'mul', '/=': 'div', '&=': 'band',
          '|=': 'bor', '^=': 'bxor'}
SPECIAL_IDS = {'self': -1.0, 'other': -2.0, 'all': -3.0, 'noone': -4.0, 'global': -5.0,
               'true': 1.0, 'false': 0.0}


def cstr(s):
    out = []
    for ch in s.encode('latin-1', 'replace'):
        if ch in (34, 92):
            out.append('\\' + chr(ch))
        elif 32 <= ch < 127 and ch != 63:  # avoid trigraph '?'
            out.append(chr(ch))
        else:
            out.append(f'\\{ch:03o}')
    return '"' + ''.join(out) + '"'


def creal(v):
    if v == int(v) and abs(v) < 1e15:
        return f'{int(v)}.0'
    return repr(float(v))


class Gen:
    def __init__(self, names):
        self.n = names               # dict with consts, scripts, builtins, globalvars
        self.user_vars = {}          # name -> id
        self.locals = set()
        self.tmp = 0
        self.loop_stack = []         # 'loop' | ('switch', endlabel) | 'with'

    def var_id(self, name):
        if name not in self.user_vars:
            self.user_vars[name] = len(self.user_vars)
        return f'V_{name}'

    def fresh(self, p='t'):
        self.tmp += 1
        return f'_{p}{self.tmp}'

    # --- variable references -------------------------------------------------
    def ref(self, node):
        """Return a C++ expression of type VarRef for an lvalue-ish node and its index expr."""
        idx = 'Idx{}'
        if node[0] == 'index':
            dims = node[2]
            if len(dims) == 1:
                idx = f'Idx{{0.0, {self.e(dims[0])}}}'
            elif len(dims) >= 2:
                idx = f'Idx{{{self.e(dims[0])}, {self.e(dims[1])}}}'
            node = node[1]
        if node[0] == 'id':
            name = node[1]
            if name in self.locals:
                return f'Ref::local(l_{name})', idx
            m = re.fullmatch(r'argument(\d+)', name)
            if m:
                return f'Ref::arg(C, {m.group(1)})', idx
            if name == 'argument':
                return 'Ref::argarr(C)', idx
            if name in self.n['builtins']:
                return f'Ref::builtin(C, BV::{name})', idx
            if name in self.n['globalvars']:
                return f'Ref::global(C, {self.var_id(name)})', idx
            return f'Ref::self(C, {self.var_id(name)})', idx
        if node[0] == 'deref':
            tgt, name = node[1], node[2]
            if tgt == ('id', 'global'):
                if name in self.n['builtins']:
                    return f'Ref::builtin(C, BV::{name})', idx
                return f'Ref::global(C, {self.var_id(name)})', idx
            field = (f'Field::builtin(BV::{name})' if name in self.n['builtins']
                     else f'Field::user({self.var_id(name)})')
            return f'Ref::deref(C, {self.e(tgt)}, {field})', idx
        raise SyntaxError(f'not assignable: {node}')

    # --- expressions ---------------------------------------------------------
    def e(self, node):
        k = node[0]
        if k == 'real':
            return f'Value({creal(node[1])})'
        if k == 'str':
            return f'Value({cstr(node[1])})'
        if k == 'bin':
            return f'op_{BINOPS[node[1]]}({{{self.e(node[2])}, {self.e(node[3])}}})'
        if k == 'un':
            return f'op_{UNOPS[node[1]]}({self.e(node[2])})'
        if k == 'call':
            return self.call(node)
        if k == 'id':
            name = node[1]
            if name not in self.locals and name not in self.n['builtins'] \
                    and name not in self.n['globalvars'] and not re.fullmatch(r'argument\d*', name):
                c = self.const(name)
                if c is not None:
                    return c
        if k in ('id', 'index', 'deref'):
            r, idx = self.ref(node)
            return f'{r}.get({idx})'
        raise SyntaxError(f'bad expression {node}')

    def const(self, name):
        if name in SPECIAL_IDS:
            return f'Value({creal(SPECIAL_IDS[name])})'
        if name == 'pi':
            return 'Value(3.141592653589793)'
        if name in self.n['assets']:
            return f'Value({creal(self.n["assets"][name])}) /*{name}*/'
        if name.lower() in self.n['consts']:
            return f'Value({creal(self.n["consts"][name.lower()])}) /*{name}*/'
        return None

    def call(self, node):
        _, name, args = node
        a = ', '.join(self.e(x) for x in args)
        if name in self.n['scripts']:
            return f's_{name}(C, Args{{{a}}})'
        return f'bi_{name}(C, Args{{{a}}})'

    # --- statements ----------------------------------------------------------
    def s(self, node, ind):
        p = '    ' * ind
        k = node[0]
        if k == 'group':
            out = [p + '{']
            for x in node[1]:
                out.append(self.s(x, ind + 1))
            out.append(p + '}')
            return '\n'.join(out)
        if k in ('var', 'globalvar'):
            return p + f'/* {k} {", ".join(node[1])} */;'
        if k == 'callstmt':
            return p + self.e(node[1]) + ';'
        if k == 'assign':
            r, idx = self.ref(node[2])
            return p + f'{r}.{ASSIGN[node[1]]}({idx}, {self.e(node[3])});'
        if k == 'if':
            out = p + f'if (truthy({self.e(node[1])}))\n' + self.s(self.blk(node[2]), ind)
            if node[3] is not None:
                out += '\n' + p + 'else\n' + self.s(self.blk(node[3]), ind)
            return out
        if k == 'while':
            self.loop_stack.append('loop')
            body = self.s(self.blk(node[2]), ind)
            self.loop_stack.pop()
            return p + f'while (truthy({self.e(node[1])}))\n' + body
        if k == 'do':
            self.loop_stack.append('loop')
            body = self.s(self.blk(node[1]), ind)
            self.loop_stack.pop()
            return p + 'do\n' + body + '\n' + p + f'while (!truthy({self.e(node[2])}));'
        if k == 'repeat':
            v = self.fresh('r')
            self.loop_stack.append('loop')
            body = self.s(self.blk(node[2]), ind)
            self.loop_stack.pop()
            return p + f'for (int {v} = repeat_count({self.e(node[1])}); {v} > 0; --{v})\n' + body
        if k == 'for':
            f = self.fresh('f')
            self.loop_stack.append('loop')
            body = self.s(self.blk(node[4]), ind + 1)
            self.loop_stack.pop()
            return '\n'.join([
                p + '{',
                self.s(node[1], ind + 1),
                p + f'    for (bool {f} = true;; {f} = false) {{',
                p + f'        if (!{f})',
                self.s(self.blk(node[3]), ind + 2),
                p + f'        if (!truthy({self.e(node[2])})) break;',
                body,
                p + '    }',
                p + '}'])
        if k == 'with':
            w = self.fresh('w')
            self.loop_stack.append('loop')
            body = self.s(self.blk(node[2]), ind + 1)
            self.loop_stack.pop()
            return '\n'.join([
                p + f'for (Instance* {w} : with_targets(C, {self.e(node[1])})) {{',
                p + f'    if (!{w}->alive()) continue;',
                p + f'    WithScope _ws(C, {w});',
                body,
                p + '}'])
        if k == 'switch':
            return self.switch(node, ind)
        if k == 'break':
            top = self.loop_stack[-1] if self.loop_stack else None
            if top is None:
                return p + 'return Value();'
            if isinstance(top, tuple):
                return p + f'goto {top[1]};'
            return p + 'break;'
        if k == 'continue':
            loops = [x for x in self.loop_stack if not isinstance(x, tuple)]
            return p + ('continue;' if loops else 'return Value();')
        if k == 'exit':
            return p + 'return Value();'
        if k == 'return':
            return p + f'return {self.e(node[1])};'
        if k in ('case', 'default'):
            raise SyntaxError('case/default outside switch')
        raise SyntaxError(f'bad statement {node}')

    def blk(self, node):
        return node if node[0] == 'group' else ('group', [node])

    def switch(self, node, ind):
        p = '    ' * ind
        sv, end = self.fresh('sw'), self.fresh('send')
        body = node[2] if node[2][0] == 'group' else ('group', [node[2]])
        labels, chunks, default = [], [], None
        cur = []
        for st in body[1]:
            if st[0] in ('case', 'default'):
                lab = self.fresh('case')
                cur = []
                chunks.append((lab, cur))
                if st[0] == 'case':
                    labels.append((lab, st[1]))
                else:
                    default = lab
            else:
                cur.append(st)
        out = [p + '{', p + f'    Value {sv} = {self.e(node[1])};']
        for lab, ce in labels:
            out.append(p + f'    if (truthy(op_eq({{{sv}, {self.e(ce)}}}))) goto {lab};')
        out.append(p + f'    goto {default or end};')
        self.loop_stack.append(('switch', end))
        for lab, sts in chunks:
            out.append(p + f'    {lab}:;')
            for st in sts:
                out.append(self.s(self.blk(st), ind + 1))
        self.loop_stack.pop()
        out.append(p + f'    {end}:;')
        out.append(p + '}')
        return '\n'.join(out)

    def piece(self, fname, ast, local_names):
        self.locals = set(local_names)
        self.loop_stack = []
        lines = [f'static Value {fname}(Ctx& C) {{']
        for name in sorted(self.locals):
            lines.append(f'    Slot l_{name};')
        lines.append(self.s(ast, 1))
        lines.append('    return Value();')
        lines.append('}')
        return '\n'.join(lines)


def collect(ast, kind, out):
    if isinstance(ast, tuple):
        if ast and ast[0] == kind:
            out.update(ast[1])
        for x in ast:
            collect(x, kind, out)
    elif isinstance(ast, list):
        for x in ast:
            collect(x, kind, out)


def ident(s):
    return re.sub(r'\W', '_', s)


def main():
    proj, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    gm = json.load(open(os.path.join(HERE, 'gm_builtins.json')))

    assets = {}
    for kind in ('sprites', 'backgrounds', 'sounds', 'fonts'):
        for f in glob.glob(f'{proj}/{kind}/*.json') + glob.glob(f'{proj}/{kind}/*/sprite.json'):
            d = json.load(open(f))
            assets[d['name']] = d['id']
    objects = {}
    for f in glob.glob(f'{proj}/objects/*/object.json'):
        d = json.load(open(f))
        objects[d['name']] = d
        assets[d['name']] = d['id']
    rooms = {}
    for f in glob.glob(f'{proj}/rooms/*.json'):
        d = json.load(open(f))
        rooms[d['name']] = d
        assets[d['name']] = d['id']
    script_ids = json.load(open(f'{proj}/scripts/index.json'))
    assets.update(script_ids)

    # gather all pieces: (cpp function name, source, description)
    pieces = []
    for name in sorted(script_ids, key=script_ids.get):
        pieces.append((f'p_script_{ident(name)}', open(f'{proj}/scripts/{name}.gml').read(), f'script {name}'))
    events = []  # (object id, type, number, [(fn or None, action json)])
    for oname, o in sorted(objects.items(), key=lambda kv: kv[1]['id']):
        for ev in o['events']:
            acts = []
            for j, a in enumerate(ev['actions']):
                if 'code_file' in a:
                    fn = f'p_{ident(oname)}_{ident(ev["label"])}_{j}'
                    pieces.append((fn, open(f'{proj}/objects/{oname}/{a["code_file"]}').read(),
                                   f'{oname} {ev["label"]} action {j}'))
                    acts.append((fn, a))
                else:
                    acts.append((None, a))
            events.append((o['id'], ev['type'], ev['number'], acts))
    room_code, inst_code = {}, {}
    for rname, r in sorted(rooms.items(), key=lambda kv: kv[1]['id']):
        if r['creation_code'].strip():
            fn = f'p_room_{ident(rname)}'
            pieces.append((fn, r['creation_code'], f'room {rname} creation code'))
            room_code[r['id']] = fn
        for inst in r['instances']:
            if inst['creation_code'].strip():
                fn = f'p_inst_{inst["id"]}'
                pieces.append((fn, inst['creation_code'], f'instance {inst["id"]} in {rname}'))
                inst_code[inst['id']] = fn

    asts, errors, globalvars = [], [], set()
    for fn, src, desc in pieces:
        try:
            ast = Parser(lex(src)).program()
        except SyntaxError as ex:
            errors.append(f'{desc}: {ex}')
            ast = None
        asts.append(ast)
        if ast:
            collect(ast, 'globalvar', globalvars)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        sys.exit(1)

    names = {'consts': gm['constants'], 'builtins': set(gm['builtin_vars']), 'assets': assets,
             'scripts': set(script_ids), 'globalvars': globalvars}
    g = Gen(names)
    bodies = []
    for (fn, src, desc), ast in zip(pieces, asts):
        locs = set()
        collect(ast, 'var', locs)
        bodies.append(f'// {desc}\n' + g.piece(fn, ast, locs))

    used_bv = sorted(set(re.findall(r'BV::(\w+)', '\n'.join(bodies))))
    used_bi = sorted(set(re.findall(r'\bbi_(\w+)\(', '\n'.join(bodies))))

    with open(f'{out}/gml_ids.h', 'w') as f:
        f.write('// generated by tools/transpiler/gml2cpp.py -- do not edit\n#pragma once\n\n')
        f.write('enum UserVar : int {\n')
        for name, i in sorted(g.user_vars.items(), key=lambda kv: kv[1]):
            f.write(f'    V_{name} = {i},\n')
        f.write(f'    V__COUNT = {len(g.user_vars)}\n}};\n\n')
        f.write('inline const char* const USER_VAR_NAMES[] = {\n')
        f.write(''.join(f'    "{n}",\n' for n, _ in sorted(g.user_vars.items(), key=lambda kv: kv[1])))
        f.write('};\n\n// builtin vars referenced by game code\n#define GML_USED_BUILTIN_VARS(X) \\\n')
        f.write(' \\\n'.join(f'    X({b})' for b in used_bv) + '\n\n')
        f.write('// builtin functions referenced by game code\n#define GML_USED_FUNCTIONS(X) \\\n')
        f.write(' \\\n'.join(f'    X({b})' for b in used_bi) + '\n')

    with open(f'{out}/gml_code.cpp', 'w') as f:
        f.write('// generated by tools/transpiler/gml2cpp.py from the game\'s GML -- do not edit\n')
        f.write('#include "gml_runtime.h"\n\nnamespace gml {\n\n')
        for name in sorted(script_ids, key=script_ids.get):
            f.write(f'static Value p_script_{ident(name)}(Ctx& C);\n')
        f.write('\n')
        for name in sorted(script_ids, key=script_ids.get):
            f.write(f'Value s_{name}(Ctx& P, const Args& A) {{ Ctx C(P, A); return p_script_{ident(name)}(C); }}\n')
        f.write('\n' + '\n\n'.join(bodies) + '\n\n')
        f.write('void register_game_code(CodeTable& T) {\n')
        for oid, et, en, acts in events:
            items = []
            for fn, a in acts:
                if fn:
                    items.append(f'Action::code({fn}, {a["target"]})')
                else:
                    args = ', '.join(cstr(x) for x in a['arguments'])
                    items.append(f'Action::builtin({a["library_id"]}, {a["action_id"]}, {a["target"]}, '
                                 f'{"true" if a["relative"] else "false"}, {{{args}}})')
            f.write(f'    T.add_event({oid}, {et}, {en}, {{{", ".join(items)}}});\n')
        for rid, fn in room_code.items():
            f.write(f'    T.room_code[{rid}] = {fn};\n')
        for iid, fn in inst_code.items():
            f.write(f'    T.instance_code[{iid}] = {fn};\n')
        f.write('}\n\n} // namespace gml\n')

    with open(f'{out}/gml_scripts.h', 'w') as f:
        f.write('// generated -- do not edit\n#pragma once\nnamespace gml {\n')
        for name in sorted(script_ids, key=script_ids.get):
            f.write(f'Value s_{name}(Ctx& P, const Args& A);\n')
        f.write('}\n')

    print(f'{len(pieces)} pieces, {len(g.user_vars)} user vars, {len(used_bv)} builtin vars, '
          f'{len(used_bi)} builtin functions, {len(globalvars)} globalvars')


if __name__ == '__main__':
    main()
