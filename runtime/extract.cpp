// Builds toh.pak straight from the original "Tower of Heaven.exe" (Game Maker 7.0), so players
// only need their own copy of the game. The output is byte-for-byte what the development
// pipeline writes (tools/extract.sh -> gm_reader -> dump.py -> tools/pack/pack.py); see that
// pipeline for the format notes.
//
// The GM7 reading is a port of gm_reader (https://github.com/elipsitz/gm_reader, MIT/Apache-2.0,
// src/decoder/mod.rs and decrypt.rs) and tools/gm7extract/gm7decrypt.py.
#include "extract.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <tuple>

namespace gml {

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::vector<uint8_t> inflate_all(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw Fail("zlib init failed");
    z.next_in = const_cast<Bytef*>(p);
    z.avail_in = static_cast<uInt>(n);
    uint8_t buf[65536];
    int rc;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); throw Fail("corrupt compressed data"); }
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (rc != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
    inflateEnd(&z);
    return out;
}

// GM strings are Windows-1252; the pipeline decodes them and writes Latin-1 with '?' for what
// Latin-1 can't hold (the cp1252-only characters in 0x80-0x9F)
std::string latin1(std::string s) {
    for (char& c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80 && u <= 0x9F && u != 0x81 && u != 0x8D && u != 0x8F && u != 0x90 && u != 0x9D) c = '?';
    }
    return s;
}

// Python's text-mode read: \r\n and lone \r become \n
std::string universal_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;
        } else out += s[i];
    }
    return out;
}

// Python's str.strip() leaves nothing (whitespace as decoded from cp1252)
bool blank(const std::string& s) {
    for (unsigned char c : s)
        if (!((c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x20) || c == 0xA0)) return false;
    return true;
}

struct In {
    std::vector<uint8_t> d;
    size_t p = 0;
    void need(size_t n) const { if (p + n > d.size()) throw Fail("unexpected end of game data"); }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    bool b() { return i32() > 0; }
    double f64() { need(8); double v; std::memcpy(&v, &d[p], 8); p += 8; return v; }
    std::vector<uint8_t> blob() { uint32_t n = u32(); need(n); std::vector<uint8_t> v(d.begin() + p, d.begin() + p + n); p += n; return v; }
    std::string str() { auto v = blob(); return latin1(std::string(v.begin(), v.end())); }
    void skip(size_t n) { need(n); p += n; }
    In compressed() { uint32_t n = u32(); need(n); In s; s.d = inflate_all(&d[p], n); p += n; return s; }
};

struct Out {
    std::vector<uint8_t>& b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f64(double v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); b.insert(b.end(), s.begin(), s.end()); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    void blob(const std::vector<uint8_t>& v) { u32(static_cast<uint32_t>(v.size())); bytes(v); }
};

// ------------------------------------------------------------------ GMKrypt

std::array<uint8_t, 256> swap_table(uint32_t seed) {
    uint32_t a = 6 + seed % 250, b = seed / 250;
    std::array<uint8_t, 256> t0{}, t1{};
    for (int i = 0; i < 256; i++) t0[i] = static_cast<uint8_t>(i);
    for (uint32_t i = 1; i < 10001; i++) {
        uint32_t j = 1 + (i * a + b) % 254;
        std::swap(t0[j], t0[j + 1]);
    }
    for (int i = 1; i < 256; i++) t1[t0[i]] = static_cast<uint8_t>(i);
    return t1;
}

// gm_reader gmkrypt_decrypt(input, 0, has_garbage, use_offset)
std::vector<uint8_t> gmkrypt(In& in, bool has_garbage, bool use_offset) {
    size_t start = in.p;
    uint32_t seed;
    if (has_garbage) {
        uint32_t s1 = in.u32(), s2 = in.u32();
        in.skip(4 * static_cast<size_t>(s1));
        seed = in.u32();
        in.skip(4 * static_cast<size_t>(s2));
    } else {
        seed = in.u32();
    }
    std::vector<uint8_t> out;
    in.need(1);
    out.push_back(in.d[in.p++]);
    size_t offset = in.p - start;
    auto t = swap_table(seed);
    for (size_t i = 0; in.p < in.d.size(); i++, in.p++) {
        uint8_t v = in.d[in.p];
        out.push_back(use_offset ? static_cast<uint8_t>(static_cast<int64_t>(t[v]) - static_cast<int64_t>(offset + i))
                                 : t[v]);
    }
    return out;
}

// ------------------------------------------------------------------ the game, as read

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA
};
Image bgra(uint32_t w, uint32_t h, std::vector<uint8_t> data) {
    if (data.size() != static_cast<size_t>(w) * h * 4) throw Fail("bad image size");
    for (size_t i = 0; i < data.size(); i += 4) std::swap(data[i], data[i + 2]);
    return {w, h, std::move(data)};
}

struct Mask { uint32_t w, h; int32_t left, right, bottom, top; std::vector<uint8_t> bits; };
struct Sprite { std::string name; int32_t ox = 0, oy = 0; std::vector<Image> frames; std::vector<Mask> masks; };
struct Background { std::string name; Image image; };
struct Sound { std::string name; uint32_t kind = 0; std::string filetype; std::vector<uint8_t> data; double volume = 0, pan = 0; bool preload = false; };
struct Script { std::string name; std::string source; };
struct Glyph { uint32_t x, y, w, h; int32_t advance, kerning; };
struct Font { std::string name; uint32_t size = 0; bool bold = false, italic = false; uint32_t start = 0, end = 0; std::vector<Glyph> glyphs; uint32_t w = 0, h = 0; std::vector<uint8_t> atlas; };
struct ActionData { uint32_t lib = 0, act = 0; int32_t target = 0; bool relative = false; std::vector<std::string> args; };
struct Event { uint32_t type; int32_t number; std::vector<ActionData> actions; };
struct Object { std::string name; int32_t sprite, depth, parent, mask; bool solid, visible, persistent; std::vector<Event> events; };
struct RoomBg { bool visible, foreground; int32_t bg, x, y; bool tile_h, tile_v; int32_t hspeed, vspeed; bool stretch; };
struct RoomView { bool visible; int32_t v[13]; };
struct RoomInst { int32_t x, y, object, id; std::string code; };
struct Tile { int32_t v[9]; };
struct Room { std::string name, caption, code; uint32_t width, height, speed, clear_color; bool persistent, clear, views_enabled; std::vector<RoomBg> bgs; std::vector<RoomView> views; std::vector<RoomInst> instances; std::vector<Tile> tiles; };
struct Include { std::string name; std::vector<uint8_t> data; };

struct GameData {
    std::map<int, Sprite> sprites;
    std::map<int, Background> backgrounds;
    std::map<int, Sound> sounds;
    std::map<int, Script> scripts;
    std::map<int, Font> fonts;
    std::map<int, Object> objects;
    std::map<int, Room> rooms;
    std::vector<Include> includes;
    std::vector<uint32_t> room_order;
};

std::vector<ActionData> read_actions(In& s) {
    if (s.u32() != 400) throw Fail("unsupported action list version");
    std::vector<ActionData> out(s.u32());
    for (auto& a : out) {
        if (s.u32() != 440) throw Fail("unsupported action version");
        a.lib = s.u32(); a.act = s.u32();
        s.u32(); s.b(); s.b(); s.b(); s.u32();   // kind, has_relative, is_question, has_target, type
        s.str(); s.str();                        // name, code
        s.u32();                                 // parameters used
        uint32_t np = s.u32();
        for (uint32_t i = 0; i < np; i++) s.u32();
        a.target = s.i32();
        a.relative = s.b();
        a.args.resize(s.u32());
        for (auto& x : a.args) x = s.str();
        s.b();                                   // negate
    }
    return out;
}

GameData read_game(const std::vector<uint8_t>& exe) {
    // header: GM runners scan in steps of 10000 bytes for (1234321, 700)
    size_t h = std::string::npos;
    for (size_t off = 0; off + 8 <= exe.size(); off += 10000) {
        uint32_t m, v;
        std::memcpy(&m, &exe[off], 4);
        std::memcpy(&v, &exe[off + 4], 4);
        if (m == 1234321 && v == 700) { h = off; break; }
    }
    if (h == std::string::npos) throw Fail("this is not a Game Maker 7 game");
    static const char dll[] = "D3DX8.dll";
    auto it = std::search(exe.begin() + h, exe.end(), dll, dll + 9);
    if (it == exe.end()) throw Fail("game data not found (no D3DX8.dll)");
    In outer;
    outer.d = exe;
    outer.p = static_cast<size_t>(it - exe.begin()) + 9;
    outer.skip(outer.u32());                     // the DLL itself
    In blob = outer.compressed();
    In s;
    s.d = gmkrypt(blob, true, true);

    GameData g;
    s.b(); s.u32();                              // pro, game id
    for (int i = 0; i < 4; i++) s.u32();         // guid

    // extensions
    if (s.u32() != 700) throw Fail("unsupported extensions version");
    for (uint32_t e = s.u32(); e > 0; e--) {
        s.u32(); s.str(); s.str();
        uint32_t nfiles = s.u32();
        for (uint32_t f = 0; f < nfiles; f++) {
            s.u32(); s.str(); s.u32(); s.str(); s.str();
            for (uint32_t fn = s.u32(); fn > 0; fn--) {
                s.u32(); s.str(); s.str(); s.u32(); s.u32(); s.i32();
                for (int i = 0; i < 17; i++) s.u32();
                s.u32();
            }
            for (uint32_t c = s.u32(); c > 0; c--) { s.u32(); s.str(); s.str(); }
        }
        In enc;
        enc.d = s.blob();
        In dec;
        dec.d = gmkrypt(enc, false, false);
        for (uint32_t f = 0; f < nfiles; f++) dec.compressed();
    }

    // sounds
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Sound so;
        so.name = s.str();
        uint32_t v = s.u32();
        if (v != 600 && v != 800) throw Fail("unsupported sound version");
        so.kind = s.u32(); so.filetype = s.str(); s.str();
        if (s.b()) so.data = s.blob();
        s.u32(); so.volume = s.f64(); so.pan = s.f64(); so.preload = s.b();
        if (so.data.size() >= 2 && so.data[0] == 0x78 && so.data[1] == 0x9c) so.data = inflate_all(so.data.data(), so.data.size());
        g.sounds[static_cast<int>(i)] = std::move(so);
    }

    // sprites
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Sprite sp;
        sp.name = s.str();
        if (s.u32() != 542) throw Fail("unsupported sprite version");
        Mask base{};
        base.w = s.u32(); base.h = s.u32();
        base.left = s.i32(); base.right = s.i32(); base.bottom = s.i32(); base.top = s.i32();
        s.b(); s.b(); s.b(); s.u32();
        bool precise = s.b();
        sp.ox = s.i32(); sp.oy = s.i32();
        for (uint32_t f = s.u32(); f > 0; f--) {
            s.u32(); s.u32();
            uint32_t w = s.u32(), hh = s.u32();
            sp.frames.push_back(bgra(w, hh, s.compressed().d));
        }
        if (precise) {
            for (auto& fr : sp.frames) {
                Mask m = base;
                m.w = fr.w; m.h = fr.h;
                m.bits.resize(static_cast<size_t>(fr.w) * fr.h);
                for (size_t k = 0; k < m.bits.size(); k++) m.bits[k] = fr.px[k * 4 + 3] == 255;
                sp.masks.push_back(std::move(m));
            }
        } else {
            Mask m = base;
            for (uint32_t y = 0; y < m.h; y++)
                for (uint32_t x = 0; x < m.w; x++) {
                    int xi = static_cast<int>(x), yi = static_cast<int>(y);
                    m.bits.push_back(xi >= m.left && xi <= m.right && yi >= m.top && yi <= m.bottom);
                }
            sp.masks.push_back(std::move(m));
        }
        g.sprites[static_cast<int>(i)] = std::move(sp);
    }

    // backgrounds
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Background bg;
        bg.name = s.str();
        if (s.u32() != 543) throw Fail("unsupported background version");
        s.u32(); s.u32(); s.b(); s.b(); s.b();
        if (s.b()) {
            s.u32(); s.u32();
            uint32_t w = s.u32(), hh = s.u32();
            bg.image = bgra(w, hh, s.compressed().d);
        }
        g.backgrounds[static_cast<int>(i)] = std::move(bg);
    }

    // paths (unused by the runtime)
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        s.str();
        if (s.u32() != 530) throw Fail("unsupported path version");
        s.u32(); s.b(); s.u32();
        for (uint32_t k = s.u32(); k > 0; k--) { s.f64(); s.f64(); s.f64(); }
    }

    // scripts
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Script sc;
        sc.name = s.str();
        if (s.u32() != 400) throw Fail("unsupported script version");
        In c = s.compressed();
        auto t = swap_table(12345);
        for (auto& byte : c.d) byte = t[byte];
        sc.source = c.str();
        g.scripts[static_cast<int>(i)] = std::move(sc);
    }

    // fonts
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Font f;
        f.name = s.str();
        uint32_t v = s.u32();
        if (v < 540) throw Fail("unsupported font version");
        s.str();
        f.size = s.u32(); f.bold = s.b(); f.italic = s.b();
        f.start = s.u32() & 0xFFFF; f.end = s.u32();
        f.glyphs.resize(256);
        for (auto& gl : f.glyphs) {
            gl.x = s.u32(); gl.y = s.u32(); gl.w = s.u32(); gl.h = s.u32();
            gl.advance = s.i32(); gl.kerning = s.i32();
        }
        f.w = s.u32(); f.h = s.u32();
        f.atlas = v == 540 ? s.compressed().d : s.blob();
        g.fonts[static_cast<int>(i)] = std::move(f);
    }

    // timelines (unused)
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        s.str();
        if (s.u32() != 500) throw Fail("unsupported timeline version");
        for (uint32_t m = s.u32(); m > 0; m--) { s.u32(); read_actions(s); }
    }

    // objects
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Object o;
        o.name = s.str();
        if (s.u32() != 430) throw Fail("unsupported object version");
        o.sprite = s.i32(); o.solid = s.b(); o.visible = s.b(); o.depth = s.i32(); o.persistent = s.b();
        o.parent = s.i32(); o.mask = s.i32();
        uint32_t ntypes = s.u32() + 1;
        for (uint32_t type = 0; type < ntypes; type++) {
            while (true) {
                int32_t number = s.i32();
                if (number == -1) break;
                o.events.push_back({type, number, read_actions(s)});
            }
        }
        g.objects[static_cast<int>(i)] = std::move(o);
    }

    // rooms
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        if (!s.b()) continue;
        Room r;
        r.name = s.str();
        if (s.u32() != 541) throw Fail("unsupported room version");
        r.caption = s.str(); r.width = s.u32(); r.height = s.u32(); r.speed = s.u32(); r.persistent = s.b();
        r.clear_color = s.u32(); r.clear = s.b(); r.code = s.str();
        r.bgs.resize(s.u32());
        for (auto& b : r.bgs) {
            b.visible = s.b(); b.foreground = s.b(); b.bg = s.i32(); b.x = s.i32(); b.y = s.i32();
            b.tile_h = s.b(); b.tile_v = s.b(); b.hspeed = s.i32(); b.vspeed = s.i32(); b.stretch = s.b();
        }
        r.views_enabled = s.b();
        r.views.resize(s.u32());
        for (auto& v : r.views) { v.visible = s.b(); for (auto& x : v.v) x = s.i32(); }
        r.instances.resize(s.u32());
        for (auto& in : r.instances) { in.x = s.i32(); in.y = s.i32(); in.object = s.i32(); in.id = s.i32(); in.code = s.str(); }
        r.tiles.resize(s.u32());
        for (auto& t : r.tiles) for (auto& x : t.v) x = s.i32();
        g.rooms[static_cast<int>(i)] = std::move(r);
    }
    s.u32(); s.u32();   // last instance id, last tile id

    // includes
    s.u32();
    for (uint32_t i = 0, n = s.u32(); i < n; i++) {
        uint32_t v = s.u32();
        if (v != 620 && v != 800) throw Fail("unsupported include version");
        Include inc;
        inc.name = s.str();
        s.str();
        bool chosen = s.b();
        s.u32();
        bool stored = s.b();
        if (chosen && stored) inc.data = v == 620 ? s.compressed().d : s.blob();
        s.u32(); s.str(); s.b(); s.b(); s.b();
        g.includes.push_back(std::move(inc));
    }

    // help
    uint32_t hv = s.u32();
    if (hv < 600) throw Fail("unsupported help version");
    s.u32(); s.b(); s.str(); s.i32(); s.i32(); s.i32(); s.i32(); s.b(); s.b(); s.b(); s.b();
    if (hv == 800) s.str(); else s.compressed();

    // library init scripts
    if (s.u32() != 500) throw Fail("unsupported library init version");
    for (uint32_t k = s.u32(); k > 0; k--) s.str();

    // room order
    uint32_t ov = s.u32();
    if (ov != 540 && ov != 700) throw Fail("unsupported room order version");
    g.room_order.resize(s.u32());
    for (auto& r : g.room_order) r = s.u32();
    return g;
}

// dump.py safe(): file names on disk
std::string safe(const std::string& name) {
    std::string out = name;
    for (char& c : out)
        if (!(std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) && c != '_' &&
            c != '.' && c != '-')
            c = '_';
    return out;
}

template <class M, class F>
void slots(Out& w, const M& items, F fn) {
    uint32_t n = items.empty() ? 0 : static_cast<uint32_t>(items.rbegin()->first + 1);
    w.u32(n);
    for (uint32_t i = 0; i < n; i++) {
        auto it = items.find(static_cast<int>(i));
        if (it == items.end()) { w.u8(0); continue; }
        w.u8(1);
        fn(it->second);
    }
}

std::string event_label(const Event& ev, const GameData& g) {
    static const char* const names[] = {"create", "destroy", "alarm", "step", "collision", "keyboard",
                                        "mouse", "other", "draw", "keypress", "keyrelease", "trigger"};
    std::string base = ev.type < 12 ? names[ev.type] : "ev" + std::to_string(ev.type);
    if (ev.type == 3) {
        if (ev.number == 0) return "step";
        if (ev.number == 1) return "begin_step";
        if (ev.number == 2) return "end_step";
        return "step" + std::to_string(ev.number);
    }
    if (ev.type == 4) {
        auto it = g.objects.find(ev.number);
        return "collision_" + (it != g.objects.end() ? it->second.name : std::to_string(ev.number));
    }
    return base + "_" + std::to_string(ev.number);
}

std::vector<uint8_t> write_pak(const GameData& g) {
    std::vector<uint8_t> buf;
    Out w{buf};
    const char magic[] = "TOHPAK02";
    buf.insert(buf.end(), magic, magic + 8);

    slots(w, g.sprites, [&](const Sprite& s) {
        w.str(s.name);
        w.u32(s.frames.empty() ? 0 : s.frames[0].w);
        w.u32(s.frames.empty() ? 0 : s.frames[0].h);
        w.i32(s.ox); w.i32(s.oy);
        w.u32(static_cast<uint32_t>(s.frames.size()));
        for (auto& f : s.frames) w.bytes(f.px);
        w.u32(static_cast<uint32_t>(s.masks.size()));
        for (auto& m : s.masks) {
            w.u32(m.w); w.u32(m.h); w.i32(m.left); w.i32(m.right); w.i32(m.top); w.i32(m.bottom);
            w.bytes(m.bits);
        }
    });
    slots(w, g.backgrounds, [&](const Background& b) {
        w.str(b.name); w.u32(b.image.w); w.u32(b.image.h); w.bytes(b.image.px);
    });
    slots(w, g.fonts, [&](const Font& f) {
        w.str(f.name); w.u32(f.size); w.boolean(f.bold); w.boolean(f.italic); w.u32(f.start); w.u32(f.end);
        w.u32(f.w); w.u32(f.h); w.bytes(f.atlas);
        w.u32(static_cast<uint32_t>(f.glyphs.size()));
        for (auto& gl : f.glyphs) {
            w.u32(gl.x); w.u32(gl.y); w.u32(gl.w); w.u32(gl.h); w.i32(gl.advance); w.i32(gl.kerning);
        }
    });
    slots(w, g.sounds, [&](const Sound& s) {
        w.str(s.name); w.u32(s.kind); w.str(s.filetype);
        w.f64(s.volume); w.f64(s.pan); w.boolean(s.preload);
        // the pipeline finds the data by name + filetype; with no filetype it was written as .bin
        w.blob(s.filetype.empty() ? std::vector<uint8_t>{} : s.data);
    });
    slots(w, g.objects, [&](const Object& o) {
        w.str(o.name); w.i32(o.sprite); w.boolean(o.solid); w.boolean(o.visible);
        w.i32(o.depth); w.boolean(o.persistent); w.i32(o.parent); w.i32(o.mask);
    });
    slots(w, g.rooms, [&](const Room& r) {
        w.str(r.name); w.str(r.caption); w.u32(r.width); w.u32(r.height); w.u32(r.speed);
        w.boolean(r.persistent); w.u32(r.clear_color); w.boolean(r.clear); w.boolean(r.views_enabled);
        w.u32(static_cast<uint32_t>(r.bgs.size()));
        for (auto& b : r.bgs) {
            w.boolean(b.visible); w.boolean(b.foreground); w.i32(b.bg); w.i32(b.x); w.i32(b.y);
            w.boolean(b.tile_h); w.boolean(b.tile_v); w.i32(b.hspeed); w.i32(b.vspeed); w.boolean(b.stretch);
        }
        w.u32(static_cast<uint32_t>(r.views.size()));
        for (auto& v : r.views) { w.boolean(v.visible); for (int32_t x : v.v) w.i32(x); }
        w.u32(static_cast<uint32_t>(r.instances.size()));
        for (auto& in : r.instances) { w.i32(in.x); w.i32(in.y); w.i32(in.object); w.i32(in.id); }
        w.u32(static_cast<uint32_t>(r.tiles.size()));
        for (auto& t : r.tiles) for (int32_t x : t.v) w.i32(x);
    });

    // includes: one per file name (a later one overwrites), sorted, DLLs left out
    std::map<std::string, const Include*> incs;
    for (auto& inc : g.includes) incs[safe(inc.name)] = &inc;
    std::vector<std::pair<std::string, const Include*>> kept;
    for (auto& [name, inc] : incs) {
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".dll") == 0) continue;
        kept.push_back({name, inc});
    }
    w.u32(static_cast<uint32_t>(kept.size()));
    for (auto& [name, inc] : kept) { w.str(name); w.blob(inc->data); }

    w.u32(static_cast<uint32_t>(g.room_order.size()));
    for (uint32_t r : g.room_order) w.i32(static_cast<int32_t>(r));

    // code
    w.u32(static_cast<uint32_t>(g.scripts.size()));
    for (auto& [id, sc] : g.scripts) { w.str(sc.name); w.i32(id); w.str(universal_newlines(sc.source)); }
    uint32_t nevents = 0;
    for (auto& [id, o] : g.objects) nevents += static_cast<uint32_t>(o.events.size());
    w.u32(nevents);
    for (auto& [id, o] : g.objects) {
        for (auto& ev : o.events) {
            w.i32(id); w.i32(static_cast<int32_t>(ev.type)); w.i32(ev.number); w.str(event_label(ev, g));
            w.u32(static_cast<uint32_t>(ev.actions.size()));
            for (auto& a : ev.actions) {
                if (a.lib == 1 && a.act == 603) {   // execute code
                    w.u8(1); w.i32(a.target);
                    w.str(universal_newlines(a.args.empty() ? std::string() : a.args[0]));
                } else {
                    w.u8(0); w.i32(static_cast<int32_t>(a.lib)); w.i32(static_cast<int32_t>(a.act)); w.i32(a.target);
                    w.boolean(a.relative);
                    w.u32(static_cast<uint32_t>(a.args.size()));
                    for (auto& x : a.args) w.str(x);
                }
            }
        }
    }
    std::vector<std::tuple<int32_t, std::string, std::string>> rc, ic;
    for (auto& [id, r] : g.rooms) {
        if (!blank(r.code)) rc.push_back({id, r.name, r.code});
        for (auto& in : r.instances)
            if (!blank(in.code)) ic.push_back({in.id, r.name, in.code});
    }
    for (auto* list : {&rc, &ic}) {
        w.u32(static_cast<uint32_t>(list->size()));
        for (auto& [id, name, src] : *list) { w.i32(id); w.str(name); w.str(src); }
    }
    return buf;
}

}  // namespace

bool extract_pak(const std::string& exe_path, std::vector<uint8_t>& pak, std::string& error) {
    try {
        std::ifstream f(exe_path, std::ios::binary);
        if (!f) throw Fail("cannot open " + exe_path);
        std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), {});
        pak = write_pak(read_game(exe));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace gml
