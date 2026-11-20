// GML interpreter. Lexer, parser and resolver mirror tools/transpiler/gml2cpp.py (itself a port
// of OpenGMK's lexer.rs / ast.rs), and the evaluator does exactly what that transpiler's C++
// did, so GM's quirks carry over:
//   * '=' inside an expression is equality; 'and'/'or'/'xor' share precedence 0
//   * unary operators bind tighter than everything; 'begin'/'end' are braces; 'then' is ignored
//   * no short-circuit and/or; operands, arguments and case labels are evaluated left to right
//   * break/continue outside any loop end the piece; statements before a switch's first case are skipped
#include "interp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <iterator>

namespace gml {

namespace {
#include "gml_names.inc"
}

// ================================================================== nodes

enum class K {
    // expressions as parsed
    Real, Str, Bin, Un, Call, Id, Index, Deref,
    // expressions after resolving
    Const, Ref, CallScript, CallBuiltin,
    // statements
    Group, Var, CallStmt, Assign, If, While, Do, Repeat, For, With, Switch, Case, Default, Break, Continue,
    Exit, Return,
};

enum class RK { Local, Arg, ArgArr, Builtin, Global, Self, Deref };

struct Node {
    K k;
    int line = 0;
    std::string name;            // identifier, call name, operator, string literal
    double real = 0;
    Value cval;                  // Const
    std::vector<Node*> kids;     // call args, index dims, group statements, var names (unused)
    Node* a = nullptr;           // operand / condition / target / ref / start
    Node* b = nullptr;           // second operand / body / value / cond
    Node* c = nullptr;           // else / step
    Node* d = nullptr;           // for body
    // resolved
    RK rk = RK::Self;
    int id = 0;                  // local slot, arg number, BV, var id, script id
    bool field_builtin = false;  // Deref
    Value (*bin)(Pair) = nullptr;
    Value (*un)(const Value&) = nullptr;
    BuiltinFn fn = nullptr;
    void (Ref::*assign)(Idx, Value) const = nullptr;
    // switch
    std::vector<std::pair<Node*, int>> cases;   // label expr -> chunk
    int default_chunk = -1;
    std::vector<std::vector<Node*>> chunks;
};

Code::Code() = default;
Code::~Code() = default;

static GmlError syntax(const std::string& msg) { return GmlError{msg}; }

// ================================================================== lexer

namespace {

enum TK { KW, OP, SEP, ID, REAL, STR };

struct Tok {
    TK kind;
    std::string val;
    double real = 0;
    int line = 0;
};

const char* const KEYWORDS[] = {"var", "globalvar", "if", "else", "with", "repeat", "do", "until", "while", "for",
                                "switch", "case", "default", "break", "continue", "return", "exit"};

bool is_kw(const std::string& w) {
    for (const char* k : KEYWORDS) if (w == k) return true;
    return false;
}

std::vector<Tok> lex(const std::string& src) {
    std::vector<Tok> toks;
    size_t i = 0, n = src.size();
    int line = 1;
    auto at = [&](size_t j) -> unsigned char { return j < n ? static_cast<unsigned char>(src[j]) : 0; };
    auto push = [&](TK k, std::string v) { toks.push_back({k, std::move(v), 0, line}); };
    while (true) {
        while (i < n && at(i) <= ' ') { line += at(i) == '\n'; i++; }
        if (i >= n) break;
        unsigned char c = at(i);
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(at(j)) || at(j) == '_') && at(j) < 128) j++;
            std::string w = src.substr(i, j - i);
            i = j;
            if (is_kw(w)) push(KW, w);
            else if (w == "mod" || w == "div") push(OP, w);
            else if (w == "and") push(OP, "&&");
            else if (w == "or") push(OP, "||");
            else if (w == "xor") push(OP, "^^");
            else if (w == "not") push(OP, "!");
            else if (w == "then") push(SEP, "then");
            else if (w == "begin") push(SEP, "{");
            else if (w == "end") push(SEP, "}");
            else push(ID, w);
        } else if (std::isdigit(c) || c == '.') {
            bool point = false;
            if (c == '.') {
                i++;
                if (std::isdigit(at(i))) point = true;
                else { push(SEP, "."); continue; }
            }
            double result = 0.0, factor = 1.0;
            while (i < n && (std::isdigit(at(i)) || at(i) == '.')) {
                if (at(i) == '.') point = true;
                else {
                    if (point) factor /= 10.0;
                    result = result * 10.0 + (at(i) - 48);
                }
                i++;
            }
            toks.push_back({REAL, "", result * factor, line});
        } else if (c == '"' || c == '\'') {
            size_t j = src.find(static_cast<char>(c), i + 1);
            if (j == std::string::npos) j = n;
            push(STR, src.substr(i + 1, j - i - 1));
            for (size_t k = i; k < j; k++) line += src[k] == '\n';
            i = j + 1;
        } else if (c == '$') {
            size_t j = i + 1;
            while (j < n && std::isxdigit(at(j))) j++;
            double v = j > i + 1 ? static_cast<double>(std::stoull(src.substr(i + 1, j - i - 1), nullptr, 16)) : 0.0;
            toks.push_back({REAL, "", v, line});
            i = j;
        } else if (std::strchr("!&()*+,-/:;<=>[]^{|}~", c)) {
            bool op = std::strchr("!&*+-/<=>^|~", c) != nullptr;
            unsigned char c2 = at(i + 1);
            i++;
            if (op) {
                if (c2 == c && std::strchr("&|^<>=", c)) {
                    push(OP, c == '&' ? "&&" : c == '|' ? "||" : c == '^' ? "^^" : c == '<' ? "<<" : c == '>' ? ">>" : "==");
                    i++;
                } else if (c2 == c && c == '/') {
                    while (i < n && at(i) != '\r' && at(i) != '\n') i++;
                } else if (c2 == '=' && std::strchr("!<>+-*/&|^", c)) {
                    push(OP, std::string(1, static_cast<char>(c)) + "=");
                    i++;
                } else if (c == '/' && c2 == '*') {
                    size_t j = src.find("*/", i + 1);
                    size_t stop = j == std::string::npos ? n : j;
                    for (size_t k = i; k < stop; k++) line += src[k] == '\n';
                    i = j == std::string::npos ? n : j + 2;
                } else if (c == '<' && c2 == '>') {
                    push(OP, "!=");
                    i++;
                } else {
                    push(OP, std::string(1, static_cast<char>(c)));
                }
            } else if (c == ':' && c2 == '=') {
                push(OP, "=");
                i++;
            } else {
                push(SEP, std::string(1, static_cast<char>(c)));
            }
        } else {
            throw syntax("line " + std::to_string(line) + ": invalid char " + std::string(1, static_cast<char>(c)));
        }
    }
    return toks;
}

// ================================================================== parser

int prec(const std::string& op) {
    if (op == "+" || op == "-") return 4;
    if (op == "*" || op == "/" || op == "div" || op == "mod") return 5;
    if (op == "&" || op == "|" || op == "^") return 2;
    if (op == "<" || op == ">" || op == "==" || op == "!=" || op == "<=" || op == ">=") return 1;
    if (op == "&&" || op == "||" || op == "^^") return 0;
    if (op == "<<" || op == ">>") return 3;
    return -1;
}

struct Parser {
    const std::vector<Tok>& t;
    Code& code;
    size_t p = 0;

    Node* mk(K k, int line = 0) {
        code.nodes.push_back(std::make_unique<Node>());
        Node* n = code.nodes.back().get();
        n->k = k;
        n->line = line;
        return n;
    }
    const Tok* peek(size_t k = 0) const { return p + k < t.size() ? &t[p + k] : nullptr; }
    const Tok* next() { const Tok* tok = peek(); p++; return tok; }
    bool is(TK kind, const char* val, size_t k = 0) const {
        const Tok* tok = peek(k);
        return tok && tok->kind == kind && tok->val == val;
    }
    void expect(TK kind, const char* val) {
        const Tok* tok = next();
        if (!tok || tok->kind != kind || tok->val != val) throw syntax(std::string("expected '") + val + "'");
    }
    void skip_semis() { while (is(SEP, ";")) p++; }

    Node* program() {
        Node* g = mk(K::Group);
        while (Node* s = line()) g->kids.push_back(s);
        return g;
    }
    Node* group() {
        if (is(SEP, ";")) { skip_semis(); return mk(K::Group); }
        if (!peek()) return nullptr;
        return line();
    }
    Node* line() {
        skip_semis();
        const Tok* tok = next();
        if (!tok) return nullptr;
        Node* r = line_(*tok);
        skip_semis();
        return r;
    }
    Node* line_(const Tok& tok) {
        if (tok.kind == KW) {
            const std::string& k = tok.val;
            if (k == "var" || k == "globalvar") {
                Node* n = mk(K::Var, tok.line);
                n->name = k;
                if (peek() && peek()->kind == ID) {
                    n->kids.push_back(mk(K::Id));
                    n->kids.back()->name = next()->val;
                    while (true) {
                        const Tok* nx = peek();
                        if (nx && nx->kind == SEP && nx->val == ",") p++;
                        else if (nx && nx->kind == ID) {
                            const Tok* after = peek(1);
                            if (after && after->kind == SEP && (after->val == "(" || after->val == ".")) break;
                        } else break;
                        if (peek() && peek()->kind == ID) {
                            n->kids.push_back(mk(K::Id));
                            n->kids.back()->name = next()->val;
                        } else break;
                    }
                }
                return n;
            }
            if (k == "do") {
                Node* n = mk(K::Do, tok.line);
                n->b = group();
                expect(KW, "until");
                n->a = expr();
                return n;
            }
            if (k == "if") {
                Node* n = mk(K::If, tok.line);
                n->a = expr();
                if (is(SEP, "then")) p++;
                n->b = group();
                if (is(KW, "else")) { p++; n->c = group(); }
                return n;
            }
            if (k == "for") {
                Node* n = mk(K::For, tok.line);
                expect(SEP, "(");
                n->a = line();
                if (is(SEP, ";")) p++;
                n->b = expr();
                if (is(SEP, ";")) p++;
                n->c = line();
                skip_semis();
                expect(SEP, ")");
                n->d = group();
                return n;
            }
            if (k == "repeat") { Node* n = mk(K::Repeat, tok.line); n->a = expr(); n->b = group(); return n; }
            if (k == "switch") { Node* n = mk(K::Switch, tok.line); n->a = expr(); n->b = line(); return n; }
            if (k == "with") {
                Node* n = mk(K::With, tok.line);
                n->a = expr();
                if (is(KW, "do")) p++;
                n->b = group();
                return n;
            }
            if (k == "while") {
                Node* n = mk(K::While, tok.line);
                n->a = expr();
                if (is(KW, "do")) p++;
                n->b = group();
                return n;
            }
            if (k == "case") { Node* n = mk(K::Case, tok.line); n->a = expr(); expect(SEP, ":"); return n; }
            if (k == "default") { expect(SEP, ":"); return mk(K::Default, tok.line); }
            if (k == "break") return mk(K::Break, tok.line);
            if (k == "continue") return mk(K::Continue, tok.line);
            if (k == "exit") return mk(K::Exit, tok.line);
            if (k == "return") { Node* n = mk(K::Return, tok.line); n->a = expr(); return n; }
            throw syntax("line " + std::to_string(tok.line) + ": unexpected keyword " + k);
        }
        if (tok.kind == ID) {
            if (is(SEP, "(")) {
                Node* n = mk(K::CallStmt, tok.line);
                n->a = call(tok.val, tok.line);
                return n;
            }
            return tree(&tok, true);
        }
        if (tok.kind == SEP && tok.val == "{") {
            Node* g = mk(K::Group, tok.line);
            while (true) {
                if (is(SEP, "}")) { p++; return g; }
                Node* s = line();
                if (!s) throw syntax("unclosed brace");
                g->kids.push_back(s);
            }
        }
        if (tok.kind == SEP && tok.val == "(") return tree(&tok, true);
        throw syntax("line " + std::to_string(tok.line) + ": unexpected token " + tok.val);
    }

    Node* call(const std::string& name, int line) {
        Node* n = mk(K::Call, line);
        n->name = name;
        expect(SEP, "(");
        if (is(SEP, ")")) { p++; return n; }
        while (true) {
            n->kids.push_back(expr());
            const Tok* tk = next();
            if (tk && tk->kind == SEP && tk->val == ")") break;
            if (tk && tk->kind == SEP && tk->val == ",") {
                if (is(SEP, ")")) { p++; break; }
                continue;
            }
            throw syntax("bad token in call " + name);
        }
        return n;
    }

    Node* expr() { return tree(nullptr, false); }

    Node* tree(const Tok* first, bool expect_assign) {
        std::string op;
        Node* v = tree_rec(first, expect_assign, 0, op);
        if (!op.empty()) throw syntax("stray operator " + op);
        return v;
    }

    // returns the expression; `out_op` = an operator left over for a caller with lower precedence
    Node* tree_rec(const Tok* first, bool expect_assign, int lowest, std::string& out_op) {
        out_op.clear();
        Node* lhs = operand(first);
        const Tok* nt = peek();
        if (!(nt && nt->kind == OP)) {
            if (expect_assign) throw syntax("line " + std::to_string(nt ? nt->line : 0) + ": expected assignment");
            return lhs;
        }
        std::string op = nt->val;
        if (op == "=" && !expect_assign) op = "==";
        p++;
        while (true) {
            int pr = prec(op);
            if (pr >= 0) {
                if (expect_assign) throw syntax("operator " + op + " where assignment expected");
                if (pr < lowest) { out_op = op; return lhs; }
                std::string nop;
                Node* rhs = tree_rec(nullptr, false, pr + 1, nop);
                Node* bin = mk(K::Bin, lhs->line);
                bin->name = op; bin->a = lhs; bin->b = rhs;
                if (nop.empty()) return bin;
                if (prec(nop) < lowest) { out_op = nop; return bin; }
                lhs = bin;
                op = nop;
            } else {
                if (!expect_assign || op == "!" || op == "~") throw syntax("invalid operator " + op);
                std::string stray;
                Node* rhs = tree_rec(nullptr, false, lowest, stray);
                if (!stray.empty()) throw syntax("stray operator " + stray);
                Node* as = mk(K::Assign, lhs->line);
                as->name = op; as->a = lhs; as->b = rhs;
                return as;
            }
        }
    }

    Node* operand(const Tok* first) {
        const Tok* tk = first ? first : next();
        if (!tk) throw syntax("unexpected end of code in expression");
        Node* lhs;
        if (tk->kind == SEP && tk->val == "(") {
            lhs = expr();
            expect(SEP, ")");
        } else if (tk->kind == OP) {
            if (tk->val == "+" || tk->val == "-" || tk->val == "!" || tk->val == "~") {
                Node* n = mk(K::Un, tk->line);
                n->name = tk->val;
                n->a = operand(nullptr);
                return n;
            }
            throw syntax("invalid unary " + tk->val);
        } else if (tk->kind == ID) {
            if (is(SEP, "(")) lhs = call(tk->val, tk->line);
            else { lhs = mk(K::Id, tk->line); lhs->name = tk->val; }
        } else if (tk->kind == REAL) {
            lhs = mk(K::Real, tk->line);
            lhs->real = tk->real;
        } else if (tk->kind == STR) {
            lhs = mk(K::Str, tk->line);
            lhs->name = tk->val;
        } else {
            throw syntax("line " + std::to_string(tk->line) + ": invalid token " + tk->val + " in expression");
        }
        while (true) {
            if (is(SEP, "[")) {
                p++;
                Node* ix = mk(K::Index, lhs->line);
                ix->a = lhs;
                if (is(SEP, "]")) p++;
                else {
                    while (true) {
                        ix->kids.push_back(expr());
                        const Tok* t2 = next();
                        if (t2 && t2->val == "]") break;
                        if (t2 && t2->val == ",") {
                            if (is(SEP, "]")) { p++; break; }
                            continue;
                        }
                        throw syntax("bad array index token");
                    }
                }
                lhs = ix;
            } else if (is(SEP, ".")) {
                p++;
                const Tok* t2 = next();
                if (!t2 || t2->kind != ID) throw syntax("expected identifier after .");
                Node* dr = mk(K::Deref, lhs->line);
                dr->a = lhs;
                dr->name = t2->val;
                lhs = dr;
            } else {
                return lhs;
            }
        }
    }
};

}  // namespace

// ================================================================== resolver

struct Resolver {
    Interp& I;
    Code& code;
    std::unordered_map<std::string, int> locals;

    static const std::unordered_set<std::string>& gm_builtin_vars() {
        static std::unordered_set<std::string> s(std::begin(GM_BUILTIN_VAR_NAMES), std::end(GM_BUILTIN_VAR_NAMES));
        return s;
    }
    static const std::unordered_map<std::string, double>& gm_constants() {
        static std::unordered_map<std::string, double> m = [] {
            std::unordered_map<std::string, double> r;
            for (auto& c : GM_CONSTANTS) r[c.name] = c.value;
            return r;
        }();
        return m;
    }
    static const std::unordered_map<std::string, BuiltinFn>& functions() {
        static std::unordered_map<std::string, BuiltinFn> m = {
#define GML_FN_ENTRY(n) {#n, bi_##n},
            GML_FUNCTIONS(GML_FN_ENTRY)
#undef GML_FN_ENTRY
        };
        return m;
    }
    static int bv(const std::string& name) {
        static const char* const names[] = {
#define GML_BV_NAME(n) #n,
            GML_BUILTIN_VARS(GML_BV_NAME)
#undef GML_BV_NAME
        };
        for (size_t i = 0; i < std::size(names); i++) if (name == names[i]) return static_cast<int>(i);
        return -1;
    }
    static bool is_argument(const std::string& name, int* n) {
        if (name.rfind("argument", 0) != 0) return false;
        std::string rest = name.substr(8);
        for (char ch : rest) if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        if (n) *n = rest.empty() ? -1 : std::stoi(rest);
        return true;
    }

    GmlError err(const Node* n, const std::string& msg) {
        return GmlError{code.desc + ": line " + std::to_string(n->line) + ": " + msg};
    }
    Node* mk(K k) {
        code.nodes.push_back(std::make_unique<Node>());
        Node* n = code.nodes.back().get();
        n->k = k;
        return n;
    }

    bool constant(const std::string& name, Value& out) {
        static const std::unordered_map<std::string, double> special = {
            {"self", -1}, {"other", -2}, {"all", -3}, {"noone", -4}, {"global", -5}, {"true", 1}, {"false", 0}};
        if (auto it = special.find(name); it != special.end()) { out = Value(it->second); return true; }
        if (name == "pi") { out = Value(3.141592653589793); return true; }
        if (auto it = I.assets.find(name); it != I.assets.end()) { out = Value(static_cast<double>(it->second)); return true; }
        std::string lower = name;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (auto it = gm_constants().find(lower); it != gm_constants().end()) { out = Value(it->second); return true; }
        return false;
    }

    void set_builtin(Node* n, const std::string& name) {
        int b = bv(name);
        if (b < 0) throw err(n, "builtin variable " + name + " is not supported by this runtime");
        n->rk = RK::Builtin;
        n->id = b;
    }

    // turns an Id / Index / Deref node into a Ref node in place (gml2cpp.py Gen.ref)
    void ref(Node* n) {
        std::vector<Node*> dims;
        Node* base = n;
        if (n->k == K::Index) { dims = n->kids; base = n->a; }
        for (Node* d : dims) expr(d);
        Node r;   // the resolved reference, copied over n
        r.k = K::Ref;
        r.line = n->line;
        if (base->k == K::Id) {
            const std::string& name = base->name;
            int an;
            if (auto it = locals.find(name); it != locals.end()) { r.rk = RK::Local; r.id = it->second; }
            else if (is_argument(name, &an)) { r.rk = an >= 0 ? RK::Arg : RK::ArgArr; r.id = an; }
            else if (gm_builtin_vars().count(name)) { set_builtin(&r, name); }
            else if (I.globalvars.count(name)) { r.rk = RK::Global; r.id = I.var_id(name); }
            else { r.rk = RK::Self; r.id = I.var_id(name); }
            r.name = name;
        } else if (base->k == K::Deref) {
            const std::string& name = base->name;
            r.name = name;
            if (base->a->k == K::Id && base->a->name == "global") {
                if (gm_builtin_vars().count(name)) set_builtin(&r, name);
                else { r.rk = RK::Global; r.id = I.var_id(name); }
            } else {
                r.rk = RK::Deref;
                expr(base->a);
                r.a = base->a;
                if (gm_builtin_vars().count(name)) { set_builtin(&r, name); r.rk = RK::Deref; r.field_builtin = true; }
                else { r.id = I.var_id(name); r.field_builtin = false; }
            }
        } else {
            throw err(n, "not assignable");
        }
        if (dims.size() > 2) dims.resize(2);   // only the first two indices are used
        r.kids = dims;
        *n = std::move(r);
    }

    void expr(Node* n) {
        switch (n->k) {
        case K::Real: { double v = n->real; n->k = K::Const; n->cval = Value(v); return; }
        case K::Str: { std::string s = n->name; n->k = K::Const; n->cval = Value(s); return; }
        case K::Bin: {
            expr(n->a); expr(n->b);
            static const std::unordered_map<std::string, Value (*)(Pair)> ops = {
                {"+", op_add}, {"-", op_sub}, {"*", op_mul}, {"/", op_div}, {"div", op_idiv}, {"mod", op_mod},
                {"&", op_band}, {"|", op_bor}, {"^", op_bxor}, {"<<", op_shl}, {">>", op_shr}, {"==", op_eq},
                {"!=", op_ne}, {"<", op_lt}, {"<=", op_le}, {">", op_gt}, {">=", op_ge}, {"&&", op_and},
                {"||", op_or}, {"^^", op_xor}};
            n->bin = ops.at(n->name);
            return;
        }
        case K::Un: {
            expr(n->a);
            n->un = n->name == "-" ? op_neg : n->name == "!" ? op_not : n->name == "~" ? op_compl : op_pos;
            return;
        }
        case K::Call: {
            for (Node* x : n->kids) expr(x);
            if (auto it = I.script_ids.find(n->name); it != I.script_ids.end()) {
                n->k = K::CallScript;
                n->id = it->second;
            } else if (auto f = functions().find(n->name); f != functions().end()) {
                n->k = K::CallBuiltin;
                n->fn = f->second;
            } else {
                throw err(n, "function " + n->name + " is not supported by this runtime");
            }
            return;
        }
        case K::Id: {
            const std::string& name = n->name;
            Value c;
            if (!locals.count(name) && !gm_builtin_vars().count(name) && !I.globalvars.count(name) &&
                !is_argument(name, nullptr) && constant(name, c)) {
                n->k = K::Const;
                n->cval = c;
                return;
            }
            ref(n);
            return;
        }
        case K::Index:
        case K::Deref:
            ref(n);
            return;
        default:
            throw err(n, "bad expression");
        }
    }

    void stmt(Node* n) {
        if (!n) return;
        switch (n->k) {
        case K::Group: for (Node* x : n->kids) stmt(x); return;
        case K::Var: return;
        case K::CallStmt: expr(n->a); return;
        case K::Assign: {
            ref(n->a);
            expr(n->b);
            static const std::unordered_map<std::string, void (Ref::*)(Idx, Value) const> ops = {
                {"=", &Ref::set}, {"+=", &Ref::add}, {"-=", &Ref::sub}, {"*=", &Ref::mul}, {"/=", &Ref::div},
                {"&=", &Ref::band}, {"|=", &Ref::bor}, {"^=", &Ref::bxor}};
            n->assign = ops.at(n->name);
            return;
        }
        case K::If: expr(n->a); stmt(n->b); stmt(n->c); return;
        case K::While: case K::Repeat: case K::With: expr(n->a); stmt(n->b); return;
        case K::Do: stmt(n->b); expr(n->a); return;
        case K::For: stmt(n->a); expr(n->b); stmt(n->c); stmt(n->d); return;
        case K::Switch: {
            expr(n->a);
            std::vector<Node*> body = n->b && n->b->k == K::Group ? n->b->kids : std::vector<Node*>{n->b};
            bool started = false;
            for (Node* st : body) {
                if (!st) continue;
                if (st->k == K::Case || st->k == K::Default) {
                    n->chunks.emplace_back();
                    started = true;
                    int chunk = static_cast<int>(n->chunks.size()) - 1;
                    if (st->k == K::Case) { expr(st->a); n->cases.push_back({st->a, chunk}); }
                    else n->default_chunk = chunk;
                } else if (started) {
                    stmt(st);
                    n->chunks.back().push_back(st);
                }
            }
            return;
        }
        case K::Case: case K::Default: throw err(n, "case/default outside switch");
        case K::Break: case K::Continue: case K::Exit: return;
        case K::Return: expr(n->a); return;
        default: throw err(n, "bad statement");
        }
    }
};

int Interp::var_id(const std::string& name) {
    auto it = var_ids.find(name);
    if (it != var_ids.end()) return it->second;
    int id = static_cast<int>(var_names.size());
    var_names.push_back(name);
    var_ids[name] = id;
    return id;
}

int Interp::find_var(const std::string& name) const {
    auto it = var_ids.find(name);
    return it == var_ids.end() ? -1 : it->second;
}

Code* Interp::parse(const std::string& src, const std::string& desc) {
    auto code = std::make_unique<Code>();
    code->desc = desc;
    try {
        std::vector<Tok> toks = lex(src);
        Parser p{toks, *code};
        code->root = p.program();
    } catch (const GmlError& e) {
        throw GmlError{desc + ": " + e.msg};
    }
    codes.push_back(std::move(code));
    return codes.back().get();
}

void Interp::link() {
    // `var` names are only local to their piece; `globalvar` names are global everywhere
    globalvars.clear();
    for (auto& c : codes) {
        std::unordered_set<std::string> gv;
        std::function<void(const Node*)> walk = [&](const Node* n) {
            if (!n) return;
            if (n->k == K::Var && n->name == "globalvar") for (const Node* v : n->kids) gv.insert(v->name);
            for (const Node* x : n->kids) walk(x);
            walk(n->a); walk(n->b); walk(n->c); walk(n->d);
        };
        walk(c->root);
        globalvars.insert(gv.begin(), gv.end());
    }
    for (auto& c : codes) resolve(*c);
}

void Interp::resolve(Code& code) {
    Resolver r{*this, code, {}};
    std::unordered_set<std::string> locs;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->k == K::Var && n->name == "var") for (const Node* v : n->kids) locs.insert(v->name);
        for (const Node* x : n->kids) walk(x);
        walk(n->a); walk(n->b); walk(n->c); walk(n->d);
    };
    walk(code.root);
    std::vector<std::string> sorted(locs.begin(), locs.end());
    std::sort(sorted.begin(), sorted.end());
    for (auto& name : sorted) r.locals[name] = static_cast<int>(r.locals.size());
    code.nlocals = static_cast<int>(r.locals.size());
    r.stmt(code.root);
}

void Interp::set_script(int id, const Code* code) {
    if (id < 0) return;
    if (static_cast<size_t>(id) >= scripts.size()) scripts.resize(id + 1, nullptr);
    scripts[id] = code;
}

// ================================================================== evaluation

enum class Flow { Normal, Break, Continue, Exit };

struct Exec {
    Interp& I;
    Ctx& C;
    std::vector<Slot> locals;
    Value ret;

    Value eval(const Node* n) {
        switch (n->k) {
        case K::Const: return n->cval;
        case K::Bin: return n->bin(Pair{eval(n->a), eval(n->b)});
        case K::Un: return n->un(eval(n->a));
        case K::Ref: {
            Ref r = make_ref(n);
            return r.get(index(n));
        }
        case K::CallBuiltin: {
            Args args;
            args.reserve(n->kids.size());
            for (const Node* x : n->kids) args.push_back(eval(x));
            return n->fn(C, args);
        }
        case K::CallScript: {
            Args args;
            args.reserve(n->kids.size());
            for (const Node* x : n->kids) args.push_back(eval(x));
            const Code* sc = static_cast<size_t>(n->id) < I.scripts.size() ? I.scripts[n->id] : nullptr;
            if (!sc) throw GmlError{"script " + n->name + " has no code"};
            Ctx sub(C, args);
            return I.run(sc, sub);
        }
        default: throw GmlError{"bad expression node"};
        }
    }

    Ref make_ref(const Node* n) {
        switch (n->rk) {
        case RK::Local: return Ref::local(locals[n->id]);
        case RK::Arg: return Ref::arg(C, n->id);
        case RK::ArgArr: return Ref::argarr(C);
        case RK::Builtin: return Ref::builtin(C, static_cast<BV>(n->id));
        case RK::Global: return Ref::global(C, n->id);
        case RK::Self: return Ref::self(C, n->id);
        case RK::Deref: {
            Value t = eval(n->a);
            return Ref::deref(C, t, n->field_builtin ? Field::builtin(static_cast<BV>(n->id)) : Field::user(n->id));
        }
        }
        throw GmlError{"bad reference"};
    }

    Idx index(const Node* n) {
        if (n->kids.empty()) return Idx{};
        if (n->kids.size() == 1) return Idx{Value(0.0), eval(n->kids[0])};
        Value i = eval(n->kids[0]);
        Value j = eval(n->kids[1]);
        return Idx{i, j};
    }

    Flow body(const Node* n) { return n ? exec(n) : Flow::Normal; }

    Flow exec(const Node* n) {
        switch (n->k) {
        case K::Group:
            for (const Node* x : n->kids) {
                Flow f = exec(x);
                if (f != Flow::Normal) return f;
            }
            return Flow::Normal;
        case K::Var: return Flow::Normal;
        case K::CallStmt: eval(n->a); return Flow::Normal;
        case K::Assign: {
            Ref r = make_ref(n->a);
            Idx idx = index(n->a);
            Value v = eval(n->b);
            (r.*(n->assign))(idx, v);
            return Flow::Normal;
        }
        case K::If:
            if (truthy(eval(n->a))) return body(n->b);
            return body(n->c);
        case K::While:
            while (truthy(eval(n->a))) {
                Flow f = body(n->b);
                if (f == Flow::Break) break;
                if (f == Flow::Exit) return f;
            }
            return Flow::Normal;
        case K::Do:
            do {
                Flow f = body(n->b);
                if (f == Flow::Break) break;
                if (f == Flow::Exit) return f;
            } while (!truthy(eval(n->a)));
            return Flow::Normal;
        case K::Repeat:
            for (int r = repeat_count(eval(n->a)); r > 0; --r) {
                Flow f = body(n->b);
                if (f == Flow::Break) break;
                if (f == Flow::Exit) return f;
            }
            return Flow::Normal;
        case K::For: {
            Flow f = body(n->a);
            if (f != Flow::Normal) return f;
            for (bool first = true;; first = false) {
                if (!first) {
                    Flow s = body(n->c);
                    if (s == Flow::Break) break;
                    if (s == Flow::Exit) return s;
                }
                if (!truthy(eval(n->b))) break;
                Flow b = body(n->d);
                if (b == Flow::Break) break;
                if (b == Flow::Exit) return b;
            }
            return Flow::Normal;
        }
        case K::With: {
            for (Instance* w : with_targets(C, eval(n->a))) {
                if (!w->alive()) continue;
                WithScope ws(C, w);
                Flow f = body(n->b);
                if (f == Flow::Break) break;
                if (f == Flow::Exit) return f;
            }
            return Flow::Normal;
        }
        case K::Switch: {
            Value sv = eval(n->a);
            int start = -1;
            for (auto& [label, chunk] : n->cases) {
                if (truthy(op_eq(Pair{sv, eval(label)}))) { start = chunk; break; }
            }
            if (start < 0) start = n->default_chunk;
            if (start < 0) return Flow::Normal;
            for (size_t c = static_cast<size_t>(start); c < n->chunks.size(); c++) {
                for (const Node* st : n->chunks[c]) {
                    Flow f = exec(st);
                    if (f == Flow::Break) return Flow::Normal;
                    if (f != Flow::Normal) return f;   // continue reaches the enclosing loop
                }
            }
            return Flow::Normal;
        }
        case K::Break: return Flow::Break;
        case K::Continue: return Flow::Continue;
        case K::Exit: return Flow::Exit;
        case K::Return: ret = eval(n->a); return Flow::Exit;
        default: throw GmlError{"bad statement node"};
        }
    }
};

Value Interp::run(const Code* code, Ctx& C) {
    Exec e{*this, C, std::vector<Slot>(code->nlocals), Value()};
    e.exec(code->root);   // break/continue that reach the top end the piece, as exit does
    return e.ret;
}

}  // namespace gml
