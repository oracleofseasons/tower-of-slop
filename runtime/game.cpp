// Frame loop, events, rooms, instances, collision, variables.
// Each part mirrors the OpenGMK function named in its comment.
#include "game.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace gml {

Game* g_game = nullptr;

enum Ev { CREATE = 0, DESTROY = 1, ALARM = 2, STEP = 3, COLLISION = 4, KEYBOARD = 5, MOUSE = 6, OTHER = 7,
          DRAW = 8, KEYPRESS = 9, KEYRELEASE = 10 };
static constexpr int ROOM_END_GAME = -100;

// ================================================================== errors

void Game::error(const std::string& msg) { throw GmlError{msg}; }

void Game::log_error(const std::string& where, const std::string& msg) {
    std::fprintf(stderr, "[frame %llu] GML error in %s: %s\n", static_cast<unsigned long long>(frame_count),
                 where.c_str(), msg.c_str());
}

// ================================================================== pak loading

namespace {
struct Reader {
    std::vector<uint8_t> d;
    size_t p = 0;
    uint8_t u8() { return d.at(p++); }
    uint32_t u32() { uint32_t v; std::memcpy(&v, &d.at(p), 4); p += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    double f64() { double v; std::memcpy(&v, &d.at(p), 8); p += 8; return v; }
    bool b() { return u8() != 0; }
    std::string str() { uint32_t n = u32(); std::string s(reinterpret_cast<char*>(&d.at(p)), n); p += n; return s; }
    std::vector<uint8_t> bytes(size_t n) { std::vector<uint8_t> v(d.begin() + p, d.begin() + p + n); p += n; return v; }
    std::vector<uint8_t> blob() { return bytes(u32()); }
};

bool decode_wav(const std::vector<uint8_t>& data, PcmSound& out) {
    // only WAV is supported; the game's single MIDI (sound_start, 164 bytes) is left silent
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0) return false;
    SDL_IOStream* io = SDL_IOFromConstMem(data.data(), data.size());
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(io, true, &out.spec, &buf, &len)) {
        std::fprintf(stderr, "wav decode failed: %s\n", SDL_GetError());
        return false;
    }
    out.pcm.assign(buf, buf + len);
    SDL_free(buf);
    return true;
}
}  // namespace

bool Game::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s (run tools/build.sh first)\n", path.c_str());
        return false;
    }
    Reader r;
    r.d.assign(std::istreambuf_iterator<char>(f), {});
    if (r.d.size() < 8 || std::memcmp(r.d.data(), "TOHPAK01", 8) != 0) {
        std::fprintf(stderr, "%s is not a TOHPAK01 file\n", path.c_str());
        return false;
    }
    r.p = 8;

    sprites.resize(r.u32());
    for (auto& slot : sprites) {
        if (!r.b()) continue;
        Sprite s;
        s.name = r.str();
        s.w = r.u32(); s.h = r.u32();
        s.origin_x = r.i32(); s.origin_y = r.i32();
        uint32_t nf = r.u32();
        for (uint32_t i = 0; i < nf; i++) s.frames.push_back(r.bytes(static_cast<size_t>(s.w) * s.h * 4));
        uint32_t nm = r.u32();
        for (uint32_t i = 0; i < nm; i++) {
            Mask m;
            m.w = r.u32(); m.h = r.u32();
            m.left = r.i32(); m.right = r.i32(); m.top = r.i32(); m.bottom = r.i32();
            m.data = r.bytes(static_cast<size_t>(m.w) * m.h);
            s.masks.push_back(std::move(m));
        }
        if (!s.masks.empty()) {
            s.bbox_left = s.masks[0].left; s.bbox_right = s.masks[0].right;
            s.bbox_top = s.masks[0].top; s.bbox_bottom = s.masks[0].bottom;
        }
        s.textures.assign(s.frames.size(), nullptr);
        slot = std::move(s);
    }

    backgrounds.resize(r.u32());
    for (auto& slot : backgrounds) {
        if (!r.b()) continue;
        Background b;
        b.name = r.str(); b.w = r.u32(); b.h = r.u32();
        b.rgba = r.bytes(static_cast<size_t>(b.w) * b.h * 4);
        slot = std::move(b);
    }

    fonts.resize(r.u32());
    for (auto& slot : fonts) {
        if (!r.b()) continue;
        Font ft;
        ft.name = r.str();
        r.u32(); r.b(); r.b();   // size, bold, italic
        ft.first = r.u32(); ft.last = r.u32();
        ft.atlas_w = r.u32(); ft.atlas_h = r.u32();
        ft.atlas = r.bytes(static_cast<size_t>(ft.atlas_w) * ft.atlas_h);
        uint32_t ng = r.u32();
        std::vector<Glyph> all(ng);
        for (auto& g : all) { g.x = r.i32(); g.y = r.i32(); g.w = r.i32(); g.h = r.i32(); g.advance = r.i32(); g.distance = r.i32(); }
        for (int c = ft.first; c <= ft.last && c < static_cast<int>(ng); c++) {
            ft.glyphs.push_back(all[c]);
            ft.tallest = std::max(ft.tallest, all[c].h);
        }
        slot = std::move(ft);
    }

    sounds.resize(r.u32());
    for (auto& slot : sounds) {
        if (!r.b()) continue;
        SoundAsset s;
        s.name = r.str(); r.u32(); r.str();   // kind, ext
        s.volume = r.f64(); r.f64(); r.b();   // pan, preload
        decode_wav(r.blob(), s.pcm);
        slot = std::move(s);
    }

    objects.resize(r.u32());
    for (auto& slot : objects) {
        if (!r.b()) continue;
        ObjectAsset o;
        o.name = r.str(); o.sprite = r.i32(); o.solid = r.b(); o.visible = r.b();
        o.depth = r.i32(); o.persistent = r.b(); o.parent = r.i32(); o.mask = r.i32();
        slot = std::move(o);
    }
    for (size_t i = 0; i < objects.size(); i++) {
        if (!objects[i]) continue;
        for (int o = static_cast<int>(i); o >= 0 && o < static_cast<int>(objects.size()) && objects[o];
             o = objects[o]->parent)
            objects[i]->ancestors.push_back(o);
    }

    rooms.resize(r.u32());
    for (auto& slot : rooms) {
        if (!r.b()) continue;
        RoomAsset rm;
        rm.name = r.str(); rm.caption = r.str(); rm.width = r.u32(); rm.height = r.u32();
        rm.speed = r.u32(); rm.persistent = r.b(); rm.clear_color = r.u32(); rm.clear = r.b();
        rm.views_enabled = r.b();
        rm.bgs.resize(r.u32());
        for (auto& b : rm.bgs) {
            b.visible = r.b(); b.foreground = r.b(); b.bg = r.i32(); b.x = r.i32(); b.y = r.i32();
            b.tile_h = r.b(); b.tile_v = r.b(); b.hspeed = r.i32(); b.vspeed = r.i32(); b.stretch = r.b();
        }
        rm.views.resize(r.u32());
        for (auto& v : rm.views) {
            v.visible = r.b(); v.x = r.i32(); v.y = r.i32(); v.w = r.i32(); v.h = r.i32();
            v.port_x = r.i32(); v.port_y = r.i32(); v.port_w = r.i32(); v.port_h = r.i32();
            v.hborder = r.i32(); v.vborder = r.i32(); v.hspeed = r.i32(); v.vspeed = r.i32(); v.target = r.i32();
        }
        rm.instances.resize(r.u32());
        for (auto& in : rm.instances) { in.x = r.i32(); in.y = r.i32(); in.object = r.i32(); in.id = r.i32(); }
        rm.tiles.resize(r.u32());
        for (auto& t : rm.tiles) {
            t.x = r.i32(); t.y = r.i32(); t.bg = r.i32(); t.tile_x = r.i32(); t.tile_y = r.i32();
            t.w = r.i32(); t.h = r.i32(); t.depth = r.i32(); t.id = r.i32();
        }
        for (auto& in : rm.instances) last_instance_id = std::max(last_instance_id, in.id);
        for (auto& t : rm.tiles) last_tile_id = std::max(last_tile_id, t.id);
        slot = std::move(rm);
    }

    uint32_t ninc = r.u32();
    for (uint32_t i = 0; i < ninc; i++) {
        std::string name = r.str();
        PcmSound pcm;
        decode_wav(r.blob(), pcm);
        includes.emplace_back(name, std::move(pcm));
    }
    room_order.resize(r.u32());
    for (auto& id : room_order) id = r.i32();

    register_game_code(code);
    fill_holders();
    return true;
}

// ================================================================== instance list (instancelist.rs)

Instance* InstanceList::insert(std::unique_ptr<Instance> inst) {
    Instance* p = inst.get();
    storage.push_back(std::move(inst));
    draw_order.push_back(p);
    by_object[p->object_index].push_back(p);
    for (int parent : p->parents) by_identity[parent].push_back(p);
    return p;
}

void InstanceList::draw_sort() {
    std::stable_sort(draw_order.begin(), draw_order.end(),
                     [](const Instance* a, const Instance* b) { return a->depth > b->depth; });
}

void InstanceList::refresh_maps() {
    by_object.clear();
    by_identity.clear();
    for (Instance* p : draw_order) {
        if (!p->alive()) continue;
        by_object[p->object_index].push_back(p);
        for (int parent : p->parents) by_identity[parent].push_back(p);
    }
}

void InstanceList::remove_deleted() {
    bool any = false;
    for (auto* p : draw_order) any |= p->state == State::Deleted;
    if (!any) return;
    draw_order.erase(std::remove_if(draw_order.begin(), draw_order.end(),
                                    [](Instance* p) { return p->state == State::Deleted; }),
                     draw_order.end());
    storage.erase(std::remove_if(storage.begin(), storage.end(),
                                 [](const std::unique_ptr<Instance>& p) { return p->state == State::Deleted; }),
                  storage.end());
    refresh_maps();
}

static std::vector<Instance*> active_of(const std::vector<Instance*>* v) {
    std::vector<Instance*> out;
    if (v)
        for (Instance* p : *v)
            if (p->alive()) out.push_back(p);
    return out;
}

std::vector<Instance*> InstanceList::snapshot_object(int obj) const {
    auto it = by_object.find(obj);
    return active_of(it == by_object.end() ? nullptr : &it->second);
}

std::vector<Instance*> InstanceList::snapshot_identity(int obj) const {
    auto it = by_identity.find(obj);
    return active_of(it == by_identity.end() ? nullptr : &it->second);
}

std::vector<Instance*> InstanceList::snapshot_active() const { return active_of(&draw_order); }

Instance* InstanceList::by_id(int id) const {
    for (Instance* p : draw_order)
        if (p->id == id) return p->alive() ? p : nullptr;
    return nullptr;
}

int InstanceList::count(int obj) const { return static_cast<int>(snapshot_identity(obj).size()); }

// ================================================================== events (game/events.rs)

// fill_event_holders: for each (type, number), the objects whose instances run that event
// (objects that define it or inherit it), in object-id order.
void Game::fill_holders() {
    std::map<std::pair<int, int>, std::vector<int>> out;
    for (auto& [key, acts] : code.events) {
        for (size_t o = 0; o < objects.size(); o++) {
            if (!objects[o]) continue;
            auto& anc = objects[o]->ancestors;
            if (std::find(anc.begin(), anc.end(), key.object) != anc.end()) {
                auto& v = out[{key.type, key.number}];
                if (std::find(v.begin(), v.end(), static_cast<int>(o)) == v.end()) v.push_back(static_cast<int>(o));
            }
        }
    }
    // collision events: an instance of A colliding with a child of B also fires A's collision_B
    std::map<std::pair<int, int>, std::vector<int>> coll;
    for (auto& [key, acts] : code.events) {
        if (key.type != COLLISION) continue;
        for (size_t o = 0; o < objects.size(); o++) {
            if (!objects[o]) continue;
            auto& anc = objects[o]->ancestors;
            if (std::find(anc.begin(), anc.end(), key.number) != anc.end()) {
                auto& v = coll[{key.object, static_cast<int>(o)}];
                (void)v;
            }
        }
    }
    for (auto& [k, v] : out) std::sort(v.begin(), v.end());
    event_holders = std::move(out);

    custom_draw.assign(objects.size(), false);
    if (auto* h = holders_for(DRAW, 0))
        for (int o : *h) custom_draw[o] = true;
}

const std::vector<int>* Game::holders_for(int type, int number) const {
    auto it = event_holders.find({type, number});
    return it == event_holders.end() ? nullptr : &it->second;
}

void Game::run_object_event(int type, int number, Instance* other) {
    const auto* h = holders_for(type, number);
    if (!h) return;
    std::vector<int> objs = *h;
    for (int obj : objs)
        for (Instance* inst : list.snapshot_object(obj))
            if (inst->alive()) run_instance_event(type, number, inst, other ? other : inst);
}

// run_instance_event: walk the parent chain to find the event; collision events also
// try the target's parents.
void Game::run_instance_event(int type, int number, Instance* inst, Instance* other) {
    if (scene_change) return;
    int obj = inst->object_index;
    int original = obj;
    for (;;) {
        if (obj < 0) {
            if (type != COLLISION) return;
            if (number < 0 || number >= static_cast<int>(objects.size()) || !objects[number]) return;
            int p = objects[number]->parent;
            if (p < 0) return;
            obj = original;
            number = p;
            continue;
        }
        auto it = code.events.find({obj, type, number});
        if (it != code.events.end()) {
            run_actions(it->second, inst, other);
            return;
        }
        if (obj >= static_cast<int>(objects.size()) || !objects[obj]) return;
        obj = objects[obj]->parent;
    }
}

void Game::run_code(CodeFn fn, Instance* self, Instance* other) {
    Ctx C(this, self, other);
    try {
        fn(C);
    } catch (const GmlError& e) {
        log_error(self && self->object_index >= 0 && objects[self->object_index] ? objects[self->object_index]->name
                                                                                   : std::string("?"),
                  e.msg);
    }
}

void Game::run_actions(const std::vector<Action>& acts, Instance* inst, Instance* other) {
    for (const Action& a : acts) {
        if (!inst->alive() && a.target == -1) break;
        if (a.fn) {
            run_code(a.fn, inst, other);
        } else if (a.library == 1 && a.action == 117) {
            // action_snap(hsnap, vsnap): move_snap
            double hs = std::atof(a.args.size() > 0 ? a.args[0].c_str() : "0");
            double vs = std::atof(a.args.size() > 1 ? a.args[1].c_str() : "0");
            if (hs > 0) inst->x = gm_round(inst->x / hs) * hs;
            if (vs > 0) inst->y = gm_round(inst->y / vs) * vs;
            inst->bbox_stale = true;
        } else {
            log_error("action", "unimplemented action " + std::to_string(a.library) + ":" + std::to_string(a.action));
        }
        if (scene_change) break;
    }
}

void Game::run_other_event(int number) {
    for (Instance* inst : list.snapshot_active()) run_instance_event(OTHER, number, inst, inst);
}

void Game::run_alarms() {
    for (int alarm = 0; alarm <= 11; alarm++) {
        const auto* h = holders_for(ALARM, alarm);
        if (!h) continue;
        std::vector<int> objs = *h;
        for (int obj : objs) {
            for (Instance* inst : list.snapshot_object(obj)) {
                if (!inst->alive()) continue;
                auto it = inst->alarms.find(alarm);
                if (it != inst->alarms.end() && it->second >= 0) {
                    it->second -= 1;
                    if (it->second == 0) run_instance_event(ALARM, alarm, inst, inst);
                }
            }
        }
    }
}

void Game::run_key_press_events() {
    // run_key_press_events: "any key" (1) first if anything was pressed, then each key
    for (int key = 0; key < 256; key++) {
        if (!key_pressed[key]) continue;
        run_object_event(KEYPRESS, key);
        if (scene_change) return;
    }
}

// run_collisions
void Game::run_collisions() {
    for (auto& [key, acts] : code.events) {
        (void)acts;
        if (key.type != COLLISION) continue;
    }
    // For every object A with an inherited collision event against B: test instances of A
    // against instances of B and B's children.
    std::vector<std::pair<int, int>> pairs;   // (holder object, target object)
    for (auto& [ev, objs] : event_holders) {
        if (ev.first != COLLISION) continue;
        for (int a : objs)
            for (size_t b = 0; b < objects.size(); b++) {
                if (!objects[b]) continue;
                auto& anc = objects[b]->ancestors;
                if (std::find(anc.begin(), anc.end(), ev.second) != anc.end()) pairs.push_back({a, static_cast<int>(b)});
            }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](auto& l, auto& r) { return l.first < r.first; });
    for (auto [a, b] : pairs) {
        for (Instance* i1 : list.snapshot_object(a)) {
            if (!i1->alive()) continue;
            for (Instance* i2 : list.snapshot_object(b)) {
                if (!i1->alive()) break;
                if (!i2->alive() || !check_collision(i1, i2)) continue;
                bool solid = i1->solid || i2->solid;
                if (solid) {
                    for (Instance* p : {i1, i2}) { p->x = p->xprevious; p->y = p->yprevious; p->bbox_stale = true; }
                }
                run_instance_event(COLLISION, b, i1, i2);
                run_instance_event(COLLISION, a, i2, i1);
                if (solid) {
                    apply_speeds(i1);
                    apply_speeds(i2);
                    if (i1->alive() && i2->alive() && check_collision(i1, i2)) {
                        for (Instance* p : {i1, i2}) { p->x = p->xprevious; p->y = p->yprevious; p->bbox_stale = true; }
                    }
                }
            }
        }
    }
}

// ================================================================== frame (game.rs Game::frame)

void Game::frame() {
    frame_count++;
    for (Instance* inst : list.snapshot_active()) { inst->xprevious = inst->x; inst->yprevious = inst->y; }

    auto stop = [&] { return scene_change.has_value(); };
    run_object_event(STEP, 1);
    if (!stop()) run_alarms();
    // keyboard (held) events are not used by this game
    if (!stop()) run_key_press_events();
    if (!stop()) run_object_event(STEP, 0);
    if (!stop()) {
        process_speeds();
        for (Instance* inst : list.snapshot_active()) apply_speeds(inst);
        run_collisions();
    }
    if (!stop()) run_object_event(STEP, 2);

    list.remove_deleted();

    if (!stop()) {
        draw();
        for (auto& b : room_state.bgs) { b.x += b.hspeed; b.y += b.vspeed; }
        for (Instance* inst : list.snapshot_active()) {
            inst->image_index += inst->image_speed;
            double frames = 0;
            if (inst->sprite_index >= 0 && inst->sprite_index < static_cast<int>(sprites.size()) &&
                sprites[inst->sprite_index])
                frames = static_cast<double>(sprites[inst->sprite_index]->frames.size());
            if (inst->image_index >= frames) {
                inst->image_index -= frames;
                run_instance_event(OTHER, 7, inst, inst);   // animation end
            }
        }
    }

    std::memset(key_pressed, 0, sizeof key_pressed);
    std::memset(key_released, 0, sizeof key_released);

    if (scene_change) {
        int target = *scene_change;
        if (target == ROOM_END_GAME) {
            quit = true;
        } else {
            load_room(target);
        }
    }
}

// ================================================================== rooms (game.rs load_room)

static Instance make_instance(Game& g, double x, double y, int obj, int id) {
    Instance in;
    in.id = id;
    in.object_index = obj;
    in.x = in.xstart = in.xprevious = x;
    in.y = in.ystart = in.yprevious = y;
    if (obj >= 0 && obj < static_cast<int>(g.objects.size()) && g.objects[obj]) {
        const ObjectAsset& o = *g.objects[obj];
        in.sprite_index = o.sprite;
        in.mask_index = o.mask;
        in.depth = o.depth;
        in.visible = o.visible;
        in.solid = o.solid;
        in.persistent = o.persistent;
        in.parents = o.ancestors;
    }
    return in;
}

Instance* Game::create_instance(double x, double y, int obj, int id, bool run_create) {
    Instance* p = list.insert(std::make_unique<Instance>(make_instance(*this, x, y, obj, id)));
    if (run_create) run_instance_event(CREATE, 0, p, p);
    return p;
}

void Game::destroy_instance(Instance* inst) {
    run_instance_event(DESTROY, 0, inst, inst);
    inst->state = State::Deleted;
}

void Game::load_room(int id) {
    if (id < 0 || id >= static_cast<int>(rooms.size()) || !rooms[id]) error("room_goto: no such room");
    if (warp_room >= 0 && rooms[id]->name == "level_1") {
        id = warp_room;
        warp_room = -1;
    }
    scene_change.reset();

    run_other_event(5);   // room end
    scene_change.reset();

    // persistent instances survive the change
    std::vector<std::unique_ptr<Instance>> keep;
    for (auto& p : list.storage)
        if (p->persistent && p->state != State::Deleted) keep.push_back(std::move(p));
    list = InstanceList();

    room = id;
    room_state = *rooms[id];
    for (auto& b : room_state.bgs) {
        if (b.stretch && b.bg >= 0 && b.bg < static_cast<int>(backgrounds.size()) && backgrounds[b.bg] &&
            backgrounds[b.bg]->w > 0) {
            b.xscale = static_cast<double>(room_state.width) / backgrounds[b.bg]->w;
            b.yscale = static_cast<double>(room_state.height) / backgrounds[b.bg]->h;
        }
    }
    tiles = room_state.tiles;
    std::memset(key_pressed, 0, sizeof key_pressed);
    std::memset(key_released, 0, sizeof key_released);

    std::vector<std::pair<Instance*, int>> fresh;
    for (auto& ri : room_state.instances) {
        bool exists = false;
        for (auto& k : keep) exists |= k->id == ri.id;
        if (exists) continue;
        fresh.push_back({list.insert(std::make_unique<Instance>(make_instance(*this, ri.x, ri.y, ri.object, ri.id))),
                         ri.id});
    }
    for (auto& k : keep) list.insert(std::move(k));

    for (auto [inst, rid] : fresh) {
        if (!inst->alive()) continue;
        auto it = code.instance_code.find(rid);
        if (it != code.instance_code.end()) run_code(it->second, inst, inst);
        run_instance_event(CREATE, 0, inst, inst);
    }
    if (game_start) {
        run_other_event(2);
        game_start = false;
    }
    auto rc = code.room_code.find(id);
    if (rc != code.room_code.end()) {
        dummy = make_instance(*this, 0, 0, 0, 0);
        run_code(rc->second, &dummy, &dummy);
    }
    run_other_event(4);   // room start
}

void Game::start() {
    if (!room_order.empty()) load_room(room_order[0]);
}

std::vector<Instance*> Game::resolve(Ctx& C, const Value& target) {
    int t = target.i32();
    if (t == -1) return {C.self};
    if (t == -2) return {C.other};
    if (t == -3) return list.snapshot_active();
    if (t < 0) return {};
    if (t < 100000) return list.snapshot_identity(t);
    Instance* p = list.by_id(t);
    if (p) return {p};
    return {};
}

std::vector<Instance*> with_targets(Ctx& C, const Value& target) {
    auto v = C.g->resolve(C, target);
    v.erase(std::remove(v.begin(), v.end(), nullptr), v.end());
    return v;
}

// ================================================================== movement (instance.rs, movement.rs)

void Instance::update_hvspeed() {
    double rad = direction * M_PI / 180.0;
    double h = std::cos(rad) * speed;
    double v = -std::sin(rad) * speed;
    double rh = gm_round(h), rv = gm_round(v);
    hspeed = std::fabs(rh - h) < 0.0001 ? rh : h;
    vspeed = std::fabs(rv - v) < 0.0001 ? rv : v;
}

void Instance::update_speed_direction() {
    double d = std::atan2(-vspeed, hspeed) * 180.0 / M_PI;
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    direction = std::fabs(d - gm_round(d)) < 0.0001 ? gm_round(d) : d;
    speed = std::sqrt(hspeed * hspeed + vspeed * vspeed);
}

static double rem_euclid(double a, double b) {
    double r = std::fmod(a, b);
    return r < 0 ? r + std::fabs(b) : r;
}

void Instance::set_direction(double d) { direction = rem_euclid(d, 360.0); update_hvspeed(); }
void Instance::set_speed(double s) { speed = s; update_hvspeed(); }
void Instance::set_speed_direction(double s, double d) { speed = s; direction = rem_euclid(d, 360.0); update_hvspeed(); }
void Instance::set_hspeed(double h) { if (hspeed != h) { hspeed = h; update_speed_direction(); } }
void Instance::set_vspeed(double v) { if (vspeed != v) { vspeed = v; update_speed_direction(); } }
void Instance::set_hvspeed(double h, double v) {
    if (hspeed != h || vspeed != v) { hspeed = h; vspeed = v; update_speed_direction(); }
}

void Game::process_speeds() {
    for (Instance* inst : list.snapshot_active()) {
        if (inst->friction != 0) {
            double s = inst->speed;
            if (s >= 0) {
                if (inst->friction > s) inst->set_speed(0);
                else if (s != 0) inst->set_speed(s - inst->friction);
            } else {
                if (inst->friction > -s) inst->set_speed(0);
                else inst->set_speed(s + inst->friction);
            }
        }
        if (inst->gravity != 0) {
            double gd = inst->gravity_direction * M_PI / 180.0;
            inst->set_hvspeed(inst->hspeed + std::cos(gd) * inst->gravity, inst->vspeed - std::sin(gd) * inst->gravity);
        }
    }
}

void Game::apply_speeds(Instance* inst) {
    if (inst->hspeed != 0 || inst->vspeed != 0) {
        inst->x += inst->hspeed;
        inst->y += inst->vspeed;
        inst->bbox_stale = true;
    }
}

// ================================================================== collision (game.rs check_collision)

const Sprite* Game::collision_sprite(const Instance* inst) const {
    int s = inst->mask_index < 0 ? inst->sprite_index : inst->mask_index;
    if (s < 0 || s >= static_cast<int>(sprites.size()) || !sprites[s]) return nullptr;
    return &*sprites[s];
}

static void rotate_around(double& x, double& y, double cx, double cy, double sn, double cs) {
    x -= cx; y -= cy;
    double nx = x * cs - y * sn, ny = x * sn + y * cs;
    x = nx + cx; y = ny + cy;
}

void Game::update_bbox(Instance* inst) {
    if (!inst->bbox_stale) return;
    const Sprite* s = collision_sprite(inst);
    if (s) {
        double x = inst->x, y = inst->y, xs = inst->image_xscale, ys = inst->image_yscale;
        double tlx = (x - s->origin_x * xs) + s->bbox_left * xs;
        double tly = (y - s->origin_y * ys) + s->bbox_top * ys;
        double brx = tlx + (s->bbox_right + 1 - s->bbox_left) * xs - 1.0;
        double bry = tly + (s->bbox_bottom + 1 - s->bbox_top) * ys - 1.0;
        if (xs <= 0) std::swap(tlx, brx);
        if (ys <= 0) std::swap(tly, bry);
        double trx = brx, try_ = tly, blx = tlx, bly = bry;
        double a = -inst->image_angle * M_PI / 180.0, sn = std::sin(a), cs = std::cos(a);
        rotate_around(tlx, tly, x, y, sn, cs);
        rotate_around(trx, try_, x, y, sn, cs);
        rotate_around(blx, bly, x, y, sn, cs);
        rotate_around(brx, bry, x, y, sn, cs);
        inst->bbox_left = static_cast<int>(gm_round(std::min({tlx, trx, blx, brx})));
        inst->bbox_right = static_cast<int>(gm_round(std::max({tlx, trx, blx, brx})));
        inst->bbox_top = static_cast<int>(gm_round(std::min({tly, try_, bly, bry})));
        inst->bbox_bottom = static_cast<int>(gm_round(std::max({tly, try_, bly, bry})));
    } else {
        inst->bbox_left = inst->bbox_right = inst->bbox_top = inst->bbox_bottom = -100000;
    }
    inst->bbox_stale = false;
}

static bool mask_point(const Mask& m, int px, int py, int ix, int iy, int ox, int oy, double xs, double ys,
                       double sn, double cs) {
    double x = px - ix, y = py - iy;
    double nx = x * cs - y * sn, ny = x * sn + y * cs;
    int mx = static_cast<int>(std::floor(ox + nx / xs));
    int my = static_cast<int>(std::floor(oy + ny / ys));
    return m.point(mx, my);
}

bool Game::check_collision(Instance* a, Instance* b) {
    if (a == b) return false;
    const Sprite* s1 = collision_sprite(a);
    const Sprite* s2 = collision_sprite(b);
    update_bbox(a);
    update_bbox(b);
    if (a->bbox_right < b->bbox_left || b->bbox_right < a->bbox_left || a->bbox_bottom < b->bbox_top ||
        b->bbox_bottom < a->bbox_top)
        return false;
    if (!s1 || !s2 || s1->masks.empty() || s2->masks.empty()) return false;
    auto pick = [](const Sprite* s, const Instance* i) -> const Mask& {
        if (s->masks.size() == 1) return s->masks[0];
        int n = static_cast<int>(s->masks.size());
        int f = static_cast<int>(std::floor(i->image_index)) % n;
        if (f < 0) f += n;
        return s->masks[f];
    };
    const Mask& m1 = pick(s1, a);
    const Mask& m2 = pick(s2, b);
    int x1 = static_cast<int>(gm_round(a->x)), y1 = static_cast<int>(gm_round(a->y));
    int x2 = static_cast<int>(gm_round(b->x)), y2 = static_cast<int>(gm_round(b->y));
    double r1 = a->image_angle * M_PI / 180.0, r2 = b->image_angle * M_PI / 180.0;
    double sn1 = std::sin(r1), cs1 = std::cos(r1), sn2 = std::sin(r2), cs2 = std::cos(r2);
    int top = std::max(a->bbox_top, b->bbox_top), bottom = std::min(a->bbox_bottom, b->bbox_bottom);
    int left = std::max(a->bbox_left, b->bbox_left), right = std::min(a->bbox_right, b->bbox_right);
    for (int y = top; y <= bottom; y++)
        for (int x = left; x <= right; x++)
            if (mask_point(m1, x, y, x1, y1, s1->origin_x, s1->origin_y, a->image_xscale, a->image_yscale, sn1, cs1) &&
                mask_point(m2, x, y, x2, y2, s2->origin_x, s2->origin_y, b->image_xscale, b->image_yscale, sn2, cs2))
                return true;
    return false;
}

Instance* Game::collision_solid(Instance* inst) {
    for (Instance* t : list.snapshot_active())
        if (t->solid && check_collision(inst, t)) return t;
    return nullptr;
}

// ================================================================== rng (rand.rs)

double Game::rand_next(double bound) {
    rng = rng * 0x08088405u + 1u;
    return static_cast<double>(rng) * (1.0 / 4294967296.0) * bound;
}

int Game::rand_int(uint32_t bound) {
    rng = rng * 0x08088405u + 1u;
    return static_cast<int>((static_cast<uint64_t>(rng) * (static_cast<uint64_t>(bound) + 1)) >> 32);
}

// ================================================================== variables

const char* Game::var_name(int id) const {
    return id >= 0 && id < V__COUNT ? USER_VAR_NAMES[id] : "?";
}

static Value read_slot(const Slot& s, int key, const char* name) {
    auto it = s.v.find(key);
    if (it == s.v.end()) throw GmlError{std::string("unknown variable ") + name};
    return it->second;
}

Value Ref::get(Idx idx) const {
    int key = idx_key(idx);
    switch (kind) {
    case Local: return read_slot(*slot, key, "(local)");
    case Arg: return id < static_cast<int>(C->args->size()) ? (*C->args)[id] : Value();
    case ArgArr: {
        int n = static_cast<int>(gm_round(idx.j.real()));
        return n >= 0 && n < static_cast<int>(C->args->size()) ? (*C->args)[n] : Value();
    }
    case Builtin: return C->g->get_builtin(*C, C->self, static_cast<BV>(id), idx);
    case Global: {
        auto it = C->g->globals.find(id);
        if (it == C->g->globals.end()) throw GmlError{std::string("unknown variable global.") + C->g->var_name(id)};
        return read_slot(it->second, key, C->g->var_name(id));
    }
    case Self: {
        auto it = C->self->vars.find(id);
        if (it == C->self->vars.end()) throw GmlError{std::string("unknown variable ") + C->g->var_name(id)};
        return read_slot(it->second, key, C->g->var_name(id));
    }
    case Deref: {
        int t = target.i32();
        if (t == -5) {
            Ref r = field.is_builtin ? Ref::builtin(*C, static_cast<BV>(field.id)) : Ref::global(*C, field.id);
            return r.get(idx);
        }
        auto insts = C->g->resolve(*C, target);
        if (insts.empty() || !insts[0]) throw GmlError{"deref of nonexistent instance"};
        Instance* inst = insts[0];
        if (field.is_builtin) return C->g->get_builtin(*C, inst, static_cast<BV>(field.id), idx);
        auto it = inst->vars.find(field.id);
        if (it == inst->vars.end()) throw GmlError{std::string("unknown variable ") + C->g->var_name(field.id)};
        return read_slot(it->second, key, C->g->var_name(field.id));
    }
    }
    return Value();
}

void Ref::set(Idx idx, Value v) const {
    int key = idx_key(idx);
    switch (kind) {
    case Local: slot->v[key] = std::move(v); return;
    case Arg:
    case ArgArr: throw GmlError{"cannot assign to argument"};
    case Builtin: C->g->set_builtin(*C, C->self, static_cast<BV>(id), idx, v); return;
    case Global: C->g->globals[id].v[key] = std::move(v); return;
    case Self: C->self->vars[id].v[key] = std::move(v); return;
    case Deref: {
        int t = target.i32();
        if (t == -5) {
            Ref r = field.is_builtin ? Ref::builtin(*C, static_cast<BV>(field.id)) : Ref::global(*C, field.id);
            r.set(idx, std::move(v));
            return;
        }
        for (Instance* inst : C->g->resolve(*C, target)) {
            if (!inst) continue;
            if (field.is_builtin) C->g->set_builtin(*C, inst, static_cast<BV>(field.id), idx, v);
            else inst->vars[field.id].v[key] = v;
        }
        return;
    }
    }
}

void Ref::modify(Idx idx, const Value& v, Value (*op)(Pair), bool type_unsafe) const {
    Value cur = get(idx);
    Value out;
    try {
        out = op({cur, v});
    } catch (const GmlError&) {
        if (!type_unsafe) throw;
        out = cur;
    }
    set(idx, out);
}

Value Game::get_builtin(Ctx& C, Instance* in, BV b, const Idx& idx) {
    int n = static_cast<int>(gm_round(idx.j.real()));
    auto view = [&]() -> View& {
        if (n < 0 || n >= static_cast<int>(room_state.views.size())) error("view index out of range");
        return room_state.views[n];
    };
    switch (b) {
    case BV::x: return in->x;
    case BV::y: return in->y;
    case BV::xprevious: return in->xprevious;
    case BV::yprevious: return in->yprevious;
    case BV::xstart: return in->xstart;
    case BV::ystart: return in->ystart;
    case BV::hspeed: return in->hspeed;
    case BV::vspeed: return in->vspeed;
    case BV::speed: return in->speed;
    case BV::direction: return in->direction;
    case BV::friction: return in->friction;
    case BV::gravity: return in->gravity;
    case BV::gravity_direction: return in->gravity_direction;
    case BV::sprite_index: return in->sprite_index;
    case BV::image_index: return in->image_index;
    case BV::image_speed: return in->image_speed;
    case BV::image_xscale: return in->image_xscale;
    case BV::image_yscale: return in->image_yscale;
    case BV::image_angle: return in->image_angle;
    case BV::image_alpha: return in->image_alpha;
    case BV::image_blend: return in->image_blend;
    case BV::image_number: {
        const Sprite* s = in->sprite_index >= 0 && in->sprite_index < static_cast<int>(sprites.size()) && sprites[in->sprite_index]
                              ? &*sprites[in->sprite_index] : nullptr;
        return s ? static_cast<double>(s->frames.size()) : 0.0;
    }
    case BV::mask_index: return in->mask_index;
    case BV::depth: return in->depth;
    case BV::visible: return in->visible;
    case BV::solid: return in->solid;
    case BV::persistent: return in->persistent;
    case BV::alarm: {
        auto it = in->alarms.find(n);
        return it == in->alarms.end() ? -1 : it->second;
    }
    case BV::id: return in->id;
    case BV::object_index: return in->object_index;
    case BV::room: return room;
    case BV::room_width: return room_state.width;
    case BV::room_height: return room_state.height;
    case BV::room_speed: return room_state.speed;
    case BV::view_xview: return view().x;
    case BV::view_yview: return view().y;
    case BV::view_wview: return view().w;
    case BV::view_hview: return view().h;
    case BV::background_hspeed:
        if (n < 0 || n >= static_cast<int>(room_state.bgs.size())) error("background index out of range");
        return room_state.bgs[n].hspeed;
    }
    (void)C;
    return Value();
}

void Game::set_builtin(Ctx& C, Instance* in, BV b, const Idx& idx, const Value& v) {
    int n = static_cast<int>(gm_round(idx.j.real()));
    double r = v.real();
    auto view = [&]() -> View& {
        if (n < 0 || n >= static_cast<int>(room_state.views.size())) error("view index out of range");
        return room_state.views[n];
    };
    switch (b) {
    case BV::x: if (r != in->x) { in->x = r; in->bbox_stale = true; } return;
    case BV::y: if (r != in->y) { in->y = r; in->bbox_stale = true; } return;
    case BV::xprevious: in->xprevious = r; return;
    case BV::yprevious: in->yprevious = r; return;
    case BV::xstart: in->xstart = r; return;
    case BV::ystart: in->ystart = r; return;
    case BV::hspeed: in->set_hspeed(r); return;
    case BV::vspeed: in->set_vspeed(r); return;
    case BV::speed: in->set_speed(r); return;
    case BV::direction: in->set_direction(r); return;
    case BV::friction: in->friction = r; return;
    case BV::gravity: in->gravity = r; return;
    case BV::gravity_direction: in->gravity_direction = r; return;
    case BV::sprite_index: {
        int s = v.i32();
        if (s != in->sprite_index) { in->sprite_index = s; in->bbox_stale = true; }
        size_t frames = s >= 0 && s < static_cast<int>(sprites.size()) && sprites[s] ? sprites[s]->frames.size() : 0;
        if (static_cast<double>(frames) <= std::floor(in->image_index)) in->image_index = 0;
        return;
    }
    case BV::image_index: in->image_index = r; return;
    case BV::image_speed: in->image_speed = r; return;
    case BV::image_xscale: if (r != in->image_xscale) { in->image_xscale = r; in->bbox_stale = true; } return;
    case BV::image_yscale: if (r != in->image_yscale) { in->image_yscale = r; in->bbox_stale = true; } return;
    case BV::image_angle: if (r != in->image_angle) { in->image_angle = r; in->bbox_stale = true; } return;
    case BV::image_alpha: in->image_alpha = r; return;
    case BV::image_blend: in->image_blend = v.i32(); return;
    case BV::mask_index: { int m = v.i32(); if (m != in->mask_index) { in->mask_index = m; in->bbox_stale = true; } return; }
    case BV::depth: in->depth = r; return;
    case BV::visible: in->visible = truthy(v); return;
    case BV::solid: in->solid = truthy(v); return;
    case BV::persistent: in->persistent = truthy(v); return;
    case BV::alarm: in->alarms[n] = v.i32(); return;
    case BV::room: scene_change = v.i32(); return;
    case BV::room_speed: room_state.speed = v.i32(); return;
    case BV::view_xview: view().x = v.i32(); return;
    case BV::view_yview: view().y = v.i32(); return;
    case BV::view_wview: view().w = v.i32(); return;
    case BV::view_hview: view().h = v.i32(); return;
    case BV::background_hspeed:
        if (n < 0 || n >= static_cast<int>(room_state.bgs.size())) error("background index out of range");
        room_state.bgs[n].hspeed = r;
        return;
    case BV::image_number: case BV::id: case BV::object_index: case BV::room_width: case BV::room_height:
        error("cannot assign to read-only variable");
    }
    (void)C;
}

}  // namespace gml
