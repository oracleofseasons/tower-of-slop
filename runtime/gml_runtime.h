// Core GML runtime types used by generated game code (build/gen/gml_code.cpp).
// Semantics follow OpenGMK (tools/OpenGMK/gm8emulator/src/gml/value.rs etc.).
#pragma once
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gml_ids.h"

namespace gml {

// ------------------------------------------------------------------ values

struct Value {
    bool is_str = false;
    double r = 0.0;
    std::string s;

    Value() = default;
    Value(double d) : r(d) {}
    Value(int i) : r(i) {}
    Value(bool b) : r(b ? 1.0 : 0.0) {}
    Value(const char* c) : is_str(true), s(c) {}
    Value(std::string str) : is_str(true), s(std::move(str)) {}

    double real() const { return is_str ? 0.0 : r; }      // From<Value> for Real
    int i32() const;                                       // round-half-even, 0 for strings
    std::string repr() const;                              // string() semantics
};

using Args = std::vector<Value>;
struct Pair { Value a, b; };   // braced init => left-to-right evaluation

struct GmlError {
    std::string msg;
};

double gm_round(double v);                 // banker's rounding (Delphi Math.Round)
bool truthy(const Value& v);
int repeat_count(const Value& v);

Value op_add(Pair p);  Value op_sub(Pair p);  Value op_mul(Pair p);  Value op_div(Pair p);
Value op_idiv(Pair p); Value op_mod(Pair p);  Value op_band(Pair p); Value op_bor(Pair p);
Value op_bxor(Pair p); Value op_shl(Pair p);  Value op_shr(Pair p);  Value op_eq(Pair p);
Value op_ne(Pair p);   Value op_lt(Pair p);   Value op_le(Pair p);   Value op_gt(Pair p);
Value op_ge(Pair p);   Value op_and(Pair p);  Value op_or(Pair p);   Value op_xor(Pair p);
Value op_neg(const Value& v); Value op_not(const Value& v); Value op_compl(const Value& v);
Value op_pos(const Value& v);

// ------------------------------------------------------------------ variables

struct Idx {
    Value i = Value(0.0), j = Value(0.0);
};
int idx_key(const Idx& idx);   // GM8 array key: i*32000 + j

struct Slot {
    std::map<int, Value> v;
    bool has(int key) const { return v.count(key) != 0; }
};

enum class BV {
    x, y, xprevious, yprevious, xstart, ystart, hspeed, vspeed, speed, direction, friction,
    gravity, gravity_direction, sprite_index, image_index, image_speed, image_xscale, image_yscale,
    image_angle, image_alpha, image_blend, image_number, mask_index, depth, visible, solid,
    persistent, alarm, id, object_index, room, room_width, room_height, room_speed,
    view_xview, view_yview, view_wview, view_hview, background_hspeed,
};

enum class State : uint8_t { Active, Inactive, Deleted };

struct Instance {
    int id = 0;
    int object_index = 0;
    State state = State::Active;
    double x = 0, y = 0, xprevious = 0, yprevious = 0, xstart = 0, ystart = 0;
    double hspeed = 0, vspeed = 0, speed = 0, direction = 0, friction = 0;
    double gravity = 0, gravity_direction = 270;
    int sprite_index = -1, mask_index = -1;
    double image_index = 0, image_speed = 1, image_xscale = 1, image_yscale = 1, image_angle = 0;
    double image_alpha = 1;
    int image_blend = 0xFFFFFF;
    double depth = 0;
    bool visible = true, solid = false, persistent = false;
    std::map<int, int> alarms;
    std::vector<int> parents;   // self + ancestors (object ids)

    bool bbox_stale = true;
    int bbox_left = 0, bbox_right = 0, bbox_top = 0, bbox_bottom = 0;

    std::unordered_map<int, Slot> vars;

    bool alive() const { return state == State::Active; }
    void set_direction(double d);
    void set_speed(double s);
    void set_speed_direction(double s, double d);
    void set_hspeed(double h);
    void set_vspeed(double v);
    void set_hvspeed(double h, double v);
    void update_hvspeed();
    void update_speed_direction();
};

class Game;

struct Ctx {
    Game* g;
    Instance* self;
    Instance* other;
    const Args* args;
    Ctx(Game* game, Instance* s, Instance* o) : g(game), self(s), other(o), args(&no_args()) {}
    Ctx(Ctx& parent, const Args& a) : g(parent.g), self(parent.self), other(parent.other), args(&a) {}
    static const Args& no_args() { static Args empty; return empty; }
};

struct Field {
    bool is_builtin;
    int id;
    static Field builtin(BV b) { return {true, static_cast<int>(b)}; }
    static Field user(int v) { return {false, v}; }
};

// A resolved variable reference: generated code does Ref::xxx(...).get(idx) / .set(idx, v)
struct Ref {
    enum Kind { Local, Arg, ArgArr, Builtin, Global, Self, Deref } kind;
    Ctx* C = nullptr;
    Slot* slot = nullptr;
    int id = 0;
    Value target;
    Field field{false, 0};

    static Ref local(Slot& s) { Ref r{Local}; r.slot = &s; return r; }
    static Ref arg(Ctx& c, int n) { Ref r{Arg}; r.C = &c; r.id = n; return r; }
    static Ref argarr(Ctx& c) { Ref r{ArgArr}; r.C = &c; return r; }
    static Ref builtin(Ctx& c, BV b) { Ref r{Builtin}; r.C = &c; r.id = static_cast<int>(b); return r; }
    static Ref global(Ctx& c, int v) { Ref r{Global}; r.C = &c; r.id = v; return r; }
    static Ref self(Ctx& c, int v) { Ref r{Self}; r.C = &c; r.id = v; return r; }
    static Ref deref(Ctx& c, Value t, Field f) { Ref r{Deref}; r.C = &c; r.target = std::move(t); r.field = f; return r; }

    Value get(Idx idx) const;
    void set(Idx idx, Value v) const;
    void add(Idx idx, Value v) const { modify(idx, v, op_add, true); }
    void sub(Idx idx, Value v) const { modify(idx, v, op_sub, false); }
    void mul(Idx idx, Value v) const { modify(idx, v, op_mul, false); }
    void div(Idx idx, Value v) const { modify(idx, v, op_div, false); }
    void band(Idx idx, Value v) const { modify(idx, v, op_band, false); }
    void bor(Idx idx, Value v) const { modify(idx, v, op_bor, false); }
    void bxor(Idx idx, Value v) const { modify(idx, v, op_bxor, false); }

  private:
    void modify(Idx idx, const Value& v, Value (*op)(Pair), bool type_unsafe) const;
};

std::vector<Instance*> with_targets(Ctx& C, const Value& target);

struct WithScope {
    Ctx& C;
    Instance* old_self;
    Instance* old_other;
    WithScope(Ctx& c, Instance* inst) : C(c), old_self(c.self), old_other(c.other) {
        C.other = C.self;
        C.self = inst;
    }
    ~WithScope() { C.self = old_self; C.other = old_other; }
};

// ------------------------------------------------------------------ code table

using CodeFn = Value (*)(Ctx&);

struct Action {
    CodeFn fn = nullptr;     // execute-code action
    int library = 0, action = 0, target = -1;
    bool relative = false;
    std::vector<std::string> args;
    static Action code(CodeFn f, int target) { Action a; a.fn = f; a.target = target; return a; }
    static Action builtin(int lib, int act, int target, bool rel, std::vector<std::string> args) {
        Action a; a.library = lib; a.action = act; a.target = target; a.relative = rel; a.args = std::move(args);
        return a;
    }
};

struct EventKey {
    int object, type, number;
    bool operator<(const EventKey& o) const {
        if (object != o.object) return object < o.object;
        if (type != o.type) return type < o.type;
        return number < o.number;
    }
};

struct CodeTable {
    std::map<EventKey, std::vector<Action>> events;
    std::map<int, CodeFn> room_code, instance_code;
    void add_event(int obj, int type, int number, std::vector<Action> acts) {
        events[{obj, type, number}] = std::move(acts);
    }
};

void register_game_code(CodeTable& table);

// ------------------------------------------------------------------ builtins
#define GML_DECLARE_FN(name) Value bi_##name(Ctx& C, const Args& a);
GML_USED_FUNCTIONS(GML_DECLARE_FN)
#undef GML_DECLARE_FN

}  // namespace gml

#include "gml_scripts.h"
