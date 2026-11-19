// The GM built-in functions Tower of Heaven calls (see docs/inventory.md).
// Each follows the OpenGMK kernel.rs function of the same name unless noted.
#include <algorithm>
#include <cstring>

#include "game.h"

namespace gml {

namespace {
double R(const Args& a, size_t i) { return i < a.size() ? a[i].real() : 0.0; }
int I(const Args& a, size_t i) { return i < a.size() ? a[i].i32() : 0; }
std::string S(const Args& a, size_t i) { return i < a.size() ? (a[i].is_str ? a[i].s : a[i].repr()) : std::string(); }
bool B(const Args& a, size_t i) { return i < a.size() && truthy(a[i]); }
Game& G(Ctx& C) { return *C.g; }
}  // namespace

// ------------------------------------------------------------------ maths & strings

Value bi_round(Ctx&, const Args& a) { return gm_round(R(a, 0)); }
Value bi_random(Ctx& C, const Args& a) { return G(C).rand_next(R(a, 0)); }
Value bi_choose(Ctx& C, const Args& a) {
    if (a.empty()) return Value();
    return a[G(C).rand_int(static_cast<uint32_t>(a.size() - 1))];
}
Value bi_point_distance(Ctx&, const Args& a) {
    double dx = R(a, 2) - R(a, 0), dy = R(a, 3) - R(a, 1);
    return std::sqrt(dx * dx + dy * dy);
}
Value bi_is_real(Ctx&, const Args& a) { return !a.empty() && !a[0].is_str; }
Value bi_is_string(Ctx&, const Args& a) { return !a.empty() && a[0].is_str; }
Value bi_string(Ctx&, const Args& a) { return a.empty() ? Value("") : Value(a[0].repr()); }
Value bi_real(Ctx&, const Args& a) {
    if (a.empty() || !a[0].is_str) return a.empty() ? Value() : a[0];
    return std::atof(a[0].s.c_str());
}
Value bi_ord(Ctx&, const Args& a) {
    std::string s = S(a, 0);
    return s.empty() ? 0.0 : static_cast<double>(static_cast<uint8_t>(s[0]));
}

// ------------------------------------------------------------------ instances

Value bi_instance_create(Ctx& C, const Args& a) {
    Game& g = G(C);
    int obj = I(a, 2);
    if (obj < 0 || obj >= static_cast<int>(g.objects.size()) || !g.objects[obj]) g.error("instance_create: no such object");
    int id = ++g.last_instance_id;
    g.create_instance(R(a, 0), R(a, 1), obj, id, true);
    return id;
}

Value bi_instance_destroy(Ctx& C, const Args&) {
    G(C).destroy_instance(C.self);
    return Value();
}

Value bi_instance_exists(Ctx& C, const Args& a) {
    int t = I(a, 0);
    if (t < 0) return t == -1 || (t == -2 && C.other);
    if (t < 100000) return G(C).list.count(t) > 0;
    return G(C).list.by_id(t) != nullptr;
}

Value bi_instance_number(Ctx& C, const Args& a) { return G(C).list.count(I(a, 0)); }

template <class F>
Instance* Game::find_instance_with(Ctx& C, int obj, F pred) {
    if (obj == -3) {
        for (Instance* p : list.snapshot_active()) if (pred(p)) return p;
        return nullptr;
    }
    if (obj == -1) return pred(C.self) ? C.self : nullptr;
    if (obj == -2) return C.other && pred(C.other) ? C.other : nullptr;
    if (obj < 0) return nullptr;
    if (obj < 100000) {
        for (Instance* p : list.snapshot_identity(obj)) if (pred(p)) return p;
        return nullptr;
    }
    Instance* p = list.by_id(obj);
    return p && pred(p) ? p : nullptr;
}

// place_meeting / instance_place: temporarily move self to (x,y)
static Instance* place_test(Ctx& C, double x, double y, int obj) {
    Game& g = G(C);
    Instance* self = C.self;
    double ox = self->x, oy = self->y;
    self->x = x; self->y = y; self->bbox_stale = true;
    Instance* hit = obj == -1 ? nullptr : g.find_instance_with(C, obj, [&](Instance* t) { return g.check_collision(self, t); });
    self->x = ox; self->y = oy; self->bbox_stale = true;
    return hit;
}

Value bi_place_meeting(Ctx& C, const Args& a) { return place_test(C, R(a, 0), R(a, 1), I(a, 2)) != nullptr; }
Value bi_instance_place(Ctx& C, const Args& a) {
    Instance* p = place_test(C, R(a, 0), R(a, 1), I(a, 2));
    return p ? p->id : -4;
}

Value bi_instance_deactivate_all(Ctx& C, const Args& a) {
    Game& g = G(C);
    for (Instance* p : g.list.snapshot_active()) p->state = State::Inactive;
    if (B(a, 0) && C.self->state == State::Inactive) C.self->state = State::Active;
    g.list.refresh_maps();
    return Value();
}

Value bi_instance_activate_all(Ctx& C, const Args&) {
    Game& g = G(C);
    for (Instance* p : g.list.draw_order) if (p->state == State::Inactive) p->state = State::Active;
    g.list.refresh_maps();
    return Value();
}

Value bi_instance_activate_object(Ctx& C, const Args& a) {
    Game& g = G(C);
    int obj = I(a, 0);
    for (Instance* p : g.list.draw_order) {
        if (p->state != State::Inactive) continue;
        bool match = obj == -3 || (obj >= 100000 ? p->id == obj : std::find(p->parents.begin(), p->parents.end(), obj) != p->parents.end());
        if (match) p->state = State::Active;
    }
    g.list.refresh_maps();
    return Value();
}

// ------------------------------------------------------------------ movement

Value bi_move_contact_solid(Ctx& C, const Args& a) {
    Game& g = G(C);
    Instance* self = C.self;
    double dir = R(a, 0) * M_PI / 180.0;
    int maxd = R(a, 1) > 0 ? static_cast<int>(gm_round(R(a, 1))) : 1000;
    double sx = std::cos(dir), sy = -std::sin(dir);
    if (!g.collision_solid(self)) {
        for (int i = 0; i < maxd; i++) {
            double ox = self->x, oy = self->y;
            self->x += sx; self->y += sy; self->bbox_stale = true;
            if (g.collision_solid(self)) { self->x = ox; self->y = oy; self->bbox_stale = true; break; }
        }
    }
    return Value();
}

Value bi_move_bounce_solid(Ctx& C, const Args& a) {
    // advanced == true in this game, but GM7's "precise" bounce isn't ported yet; use the basic one.
    (void)a;
    Game& g = G(C);
    Instance* in = C.self;
    if (g.collision_solid(in)) { in->x = in->xprevious; in->y = in->yprevious; }
    double ox = in->x, oy = in->y;
    in->x = ox + in->hspeed; in->bbox_stale = true;
    bool xb = g.collision_solid(in) != nullptr;
    in->x = ox; in->y = oy + in->vspeed; in->bbox_stale = true;
    bool yb = g.collision_solid(in) != nullptr;
    if (xb) in->set_hspeed(-in->hspeed);
    if (yb) in->set_vspeed(-in->vspeed);
    if (!xb && !yb) {
        in->x = ox + in->hspeed; in->bbox_stale = true;
        if (g.collision_solid(in)) in->set_hvspeed(-in->hspeed, -in->vspeed);
    }
    in->x = ox; in->y = oy; in->bbox_stale = true;
    return Value();
}

Value bi_move_towards_point(Ctx& C, const Args& a) {
    Instance* in = C.self;
    double d = std::atan2(in->y - R(a, 1), R(a, 0) - in->x) * 180.0 / M_PI;
    in->set_speed_direction(R(a, 2), d);
    return Value();
}

// ------------------------------------------------------------------ rooms & game

Value bi_room_goto(Ctx& C, const Args& a) { G(C).scene_change = I(a, 0); return Value(); }

Value bi_room_next(Ctx& C, const Args& a) {
    auto& order = G(C).room_order;
    int r = I(a, 0);
    for (size_t i = 0; i + 1 < order.size(); i++) if (order[i] == r) return order[i + 1];
    return -1;
}

Value bi_room_goto_next(Ctx& C, const Args&) {
    Game& g = G(C);
    Value n = bi_room_next(C, {Value(g.room)});
    if (n.i32() < 0) g.error("room_goto_next: no next room");
    g.scene_change = n.i32();
    return Value();
}

Value bi_game_end(Ctx& C, const Args&) { G(C).scene_change = -100; return Value(); }

Value bi_show_error(Ctx& C, const Args& a) {
    G(C).log_error("show_error", S(a, 0));
    if (B(a, 1)) G(C).quit = true;
    return Value();
}

Value bi_variable_global_exists(Ctx& C, const Args& a) {
    std::string name = S(a, 0);
    for (int i = 0; i < V__COUNT; i++)
        if (name == USER_VAR_NAMES[i]) return G(C).globals.count(i) != 0;
    return false;
}

// ------------------------------------------------------------------ input

Value bi_keyboard_check(Ctx& C, const Args& a) { int k = I(a, 0); return k >= 0 && k < 256 && G(C).key_held[k]; }
Value bi_keyboard_check_pressed(Ctx& C, const Args& a) { int k = I(a, 0); return k >= 0 && k < 256 && G(C).key_pressed[k]; }
Value bi_keyboard_check_released(Ctx& C, const Args& a) { int k = I(a, 0); return k >= 0 && k < 256 && G(C).key_released[k]; }

// ------------------------------------------------------------------ drawing

Value bi_draw_set_color(Ctx& C, const Args& a) { G(C).draw_color = I(a, 0); return Value(); }
Value bi_draw_set_alpha(Ctx& C, const Args& a) { G(C).draw_alpha = R(a, 0); return Value(); }
Value bi_draw_set_font(Ctx& C, const Args& a) { G(C).draw_font = I(a, 0); return Value(); }
Value bi_draw_set_halign(Ctx& C, const Args& a) { G(C).halign = I(a, 0); return Value(); }
Value bi_draw_set_valign(Ctx& C, const Args& a) { G(C).valign = I(a, 0); return Value(); }

Value bi_draw_sprite(Ctx& C, const Args& a) {
    G(C).draw_sprite_ext(I(a, 0), R(a, 1), R(a, 2), R(a, 3), 1, 1, 0, 0xFFFFFF, G(C).draw_alpha, C.self);
    return Value();
}

Value bi_draw_sprite_ext(Ctx& C, const Args& a) {
    G(C).draw_sprite_ext(I(a, 0), R(a, 1), R(a, 2), R(a, 3), R(a, 4), R(a, 5), R(a, 6), I(a, 7), R(a, 8), C.self);
    return Value();
}

static Value tiled(Ctx& C, int spr, int sub, double x, double y, double xs, double ys, int col, double alpha) {
    Game& g = G(C);
    if (spr < 0 || spr >= static_cast<int>(g.sprites.size()) || !g.sprites[spr]) g.error("draw_sprite_tiled: no such sprite");
    Sprite& s = *g.sprites[spr];
    if (sub < 0) sub = static_cast<int>(std::floor(C.self->image_index));
    int n = static_cast<int>(s.frames.size());
    if (!n) return Value();
    SDL_Texture* t = g.sprite_texture(s, ((sub % n) + n) % n);
    double w = s.w * xs, h = s.h * ys;
    double sx = std::fmod(x, w); if (sx > 0) sx -= w;
    double sy = std::fmod(y, h); if (sy > 0) sy -= h;
    for (double yy = sy; yy < g.room_state.height; yy += h)
        for (double xx = sx; xx < g.room_state.width; xx += w)
            g.draw_texture(t, 0, 0, s.w, s.h, xx, yy, 0, 0, xs, ys, 0, col, alpha);
    return Value();
}

Value bi_draw_sprite_tiled(Ctx& C, const Args& a) { return tiled(C, I(a, 0), I(a, 1), R(a, 2), R(a, 3), 1, 1, 0xFFFFFF, G(C).draw_alpha); }
Value bi_draw_sprite_tiled_ext(Ctx& C, const Args& a) {
    return tiled(C, I(a, 0), I(a, 1), R(a, 2), R(a, 3), R(a, 4), R(a, 5), I(a, 6), R(a, 7));
}

Value bi_draw_background(Ctx& C, const Args& a) {
    Game& g = G(C);
    int b = I(a, 0);
    if (b < 0 || b >= static_cast<int>(g.backgrounds.size()) || !g.backgrounds[b]) g.error("draw_background: no such background");
    Background& bg = *g.backgrounds[b];
    g.draw_texture(g.bg_texture(bg), 0, 0, bg.w, bg.h, R(a, 1), R(a, 2), 0, 0, 1, 1, 0, 0xFFFFFF, g.draw_alpha);
    return Value();
}

Value bi_draw_rectangle_color(Ctx& C, const Args& a) {
    G(C).draw_rect(R(a, 0), R(a, 1), R(a, 2), R(a, 3), I(a, 4), I(a, 5), I(a, 6), I(a, 7), B(a, 8));
    return Value();
}

Value bi_draw_clear(Ctx& C, const Args& a) {
    Game& g = G(C);
    g.set_target(g.target_texture());
    int c = I(a, 0);
    SDL_SetRenderDrawColor(g.renderer, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255);
    SDL_RenderClear(g.renderer);
    return Value();
}

Value bi_draw_text(Ctx& C, const Args& a) {
    Game& g = G(C);
    g.draw_string(R(a, 0), R(a, 1), S(a, 2), -1, -1, g.draw_color, g.draw_color, g.draw_color, g.draw_color, g.draw_alpha);
    return Value();
}

Value bi_draw_text_color(Ctx& C, const Args& a) {
    G(C).draw_string(R(a, 0), R(a, 1), S(a, 2), -1, -1, I(a, 3), I(a, 4), I(a, 5), I(a, 6), R(a, 7));
    return Value();
}

Value bi_draw_text_ext_color(Ctx& C, const Args& a) {
    int sep = I(a, 3), w = I(a, 4);
    G(C).draw_string(R(a, 0), R(a, 1), S(a, 2), sep < 0 ? -1 : sep, w < 0 ? -1 : w, I(a, 5), I(a, 6), I(a, 7), I(a, 8), R(a, 9));
    return Value();
}

Value bi_string_width_ext(Ctx& C, const Args& a) {
    int sep = I(a, 1), w = I(a, 2);
    return G(C).string_size(S(a, 0), sep < 0 ? -1 : sep, w < 0 ? -1 : w).first;
}

Value bi_string_height_ext(Ctx& C, const Args& a) {
    int sep = I(a, 1), w = I(a, 2);
    return G(C).string_size(S(a, 0), sep < 0 ? -1 : sep, w < 0 ? -1 : w).second;
}

// ------------------------------------------------------------------ surfaces, screen, window

Value bi_surface_create(Ctx& C, const Args& a) {
    Game& g = G(C);
    SDL_Texture* t = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, I(a, 0), I(a, 1));
    if (!t) return -1;
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    g.surfaces.push_back(t);
    return static_cast<int>(g.surfaces.size() - 1);
}

Value bi_surface_set_target(Ctx& C, const Args& a) {
    Game& g = G(C);
    int s = I(a, 0);
    if (s >= 0 && s < static_cast<int>(g.surfaces.size()) && g.surfaces[s]) g.surface_target = s;
    return Value();
}

Value bi_surface_reset_target(Ctx& C, const Args&) {
    Game& g = G(C);
    g.surface_target = -1;
    g.xf = Game::Xform{};
    return Value();
}

Value bi_draw_surface_stretched(Ctx& C, const Args& a) {
    Game& g = G(C);
    int s = I(a, 0);
    if (s < 0 || s >= static_cast<int>(g.surfaces.size()) || !g.surfaces[s]) g.error("draw_surface_stretched: no such surface");
    float w, h;
    SDL_GetTextureSize(g.surfaces[s], &w, &h);
    g.draw_texture(g.surfaces[s], 0, 0, w, h, R(a, 1), R(a, 2), 0, 0, R(a, 3) / w, R(a, 4) / h, 0, 0xFFFFFF, g.draw_alpha);
    return Value();
}

Value bi_screen_refresh(Ctx&, const Args&) { return Value(); }   // frame is presented once, at the end
Value bi_window_set_size(Ctx&, const Args&) { return Value(); }  // window size is ours to manage
Value bi_window_set_region_scale(Ctx&, const Args&) { return Value(); }

// ------------------------------------------------------------------ sprites

Value bi_sprite_exists(Ctx& C, const Args& a) {
    int s = I(a, 0);
    return s >= 0 && s < static_cast<int>(G(C).sprites.size()) && G(C).sprites[s].has_value();
}

Value bi_sprite_delete(Ctx& C, const Args& a) {
    Game& g = G(C);
    int s = I(a, 0);
    if (s >= 0 && s < static_cast<int>(g.sprites.size()) && g.sprites[s]) {
        for (SDL_Texture* t : g.sprites[s]->textures) if (t) SDL_DestroyTexture(t);
        g.sprites[s].reset();
    }
    return Value();
}

// GM7 signature: (x, y, w, h, precise, transparent, smooth, preload, xorig, yorig)
Value bi_sprite_create_from_screen(Ctx& C, const Args& a) {
    Game& g = G(C);
    SDL_Rect r{I(a, 0), I(a, 1), I(a, 2), I(a, 3)};
    SDL_SetRenderTarget(g.renderer, g.screen);
    SDL_Surface* surf = SDL_RenderReadPixels(g.renderer, &r);
    if (!surf) g.error(std::string("sprite_create_from_screen: ") + SDL_GetError());
    SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surf);
    Sprite s;
    s.name = "__screen";
    s.w = conv->w; s.h = conv->h;
    s.origin_x = I(a, 8); s.origin_y = I(a, 9);
    std::vector<uint8_t> px(static_cast<size_t>(s.w) * s.h * 4);
    for (int y = 0; y < s.h; y++)
        std::memcpy(&px[static_cast<size_t>(y) * s.w * 4], static_cast<uint8_t*>(conv->pixels) + y * conv->pitch, s.w * 4);
    SDL_DestroySurface(conv);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    s.frames.push_back(std::move(px));
    s.textures.assign(1, nullptr);
    Mask m;
    m.w = s.w; m.h = s.h; m.left = 0; m.top = 0; m.right = s.w - 1; m.bottom = s.h - 1;
    m.data.assign(static_cast<size_t>(s.w) * s.h, 1);
    s.masks.push_back(m);
    s.bbox_left = 0; s.bbox_top = 0; s.bbox_right = s.w - 1; s.bbox_bottom = s.h - 1;
    g.sprites.push_back(std::move(s));
    return static_cast<int>(g.sprites.size() - 1);
}

// Alpha of each pixel = average intensity of the matching pixel in the other sprite (stretched).
Value bi_sprite_set_alpha_from_sprite(Ctx& C, const Args& a) {
    Game& g = G(C);
    int d = I(a, 0), s = I(a, 1);
    auto ok = [&](int i) { return i >= 0 && i < static_cast<int>(g.sprites.size()) && g.sprites[i]; };
    if (!ok(d) || !ok(s)) g.error("sprite_set_alpha_from_sprite: no such sprite");
    Sprite& dst = *g.sprites[d];
    const Sprite& src = *g.sprites[s];
    for (size_t f = 0; f < dst.frames.size(); f++) {
        const auto& sp = src.frames[std::min(f, src.frames.size() - 1)];
        auto& dp = dst.frames[f];
        for (int y = 0; y < dst.h; y++)
            for (int x = 0; x < dst.w; x++) {
                int sx = x * src.w / dst.w, sy = y * src.h / dst.h;
                const uint8_t* p = &sp[(static_cast<size_t>(sy) * src.w + sx) * 4];
                uint8_t* q = &dp[(static_cast<size_t>(y) * dst.w + x) * 4];
                q[3] = static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3 * q[3] / 255);
            }
        if (dst.textures[f]) { SDL_DestroyTexture(dst.textures[f]); dst.textures[f] = nullptr; }
    }
    return Value();
}

// ------------------------------------------------------------------ sounds (GM built-in, WAV effects)

static SDL_AudioStream* stream_for(Game& g, const PcmSound& pcm, SDL_AudioStream*& slot) {
    if (!g.audio_dev || pcm.pcm.empty()) return nullptr;
    if (!slot) {
        slot = SDL_CreateAudioStream(&pcm.spec, nullptr);
        SDL_BindAudioStream(g.audio_dev, slot);
    }
    return slot;
}

static SoundAsset* sound(Game& g, int id) {
    if (id < 0 || id >= static_cast<int>(g.sounds.size()) || !g.sounds[id]) g.error("no such sound");
    return &*g.sounds[id];
}

Value bi_sound_play(Ctx& C, const Args& a) {
    SoundAsset* s = sound(G(C), I(a, 0));
    if (auto* st = stream_for(G(C), s->pcm, s->stream)) {
        SDL_ClearAudioStream(st);
        SDL_SetAudioStreamGain(st, static_cast<float>(s->volume));
        SDL_PutAudioStreamData(st, s->pcm.pcm.data(), static_cast<int>(s->pcm.pcm.size()));
    }
    s->looping = false;
    return Value();
}

Value bi_sound_loop(Ctx& C, const Args& a) {
    bi_sound_play(C, a);
    sound(G(C), I(a, 0))->looping = true;
    return Value();
}

Value bi_sound_stop(Ctx& C, const Args& a) {
    SoundAsset* s = sound(G(C), I(a, 0));
    if (s->stream) SDL_ClearAudioStream(s->stream);
    s->looping = false;
    return Value();
}

Value bi_sound_stop_all(Ctx& C, const Args&) {
    for (auto& s : G(C).sounds)
        if (s) { if (s->stream) SDL_ClearAudioStream(s->stream); s->looping = false; }
    return Value();
}

Value bi_sound_isplaying(Ctx& C, const Args& a) {
    SoundAsset* s = sound(G(C), I(a, 0));
    return s->stream && SDL_GetAudioStreamQueued(s->stream) > 0;
}

Value bi_sound_volume(Ctx& C, const Args& a) {
    SoundAsset* s = sound(G(C), I(a, 0));
    s->volume = R(a, 1);
    if (s->stream) SDL_SetAudioStreamGain(s->stream, static_cast<float>(s->volume));
    return Value();
}

Value bi_sound_background_tempo(Ctx&, const Args&) { return Value(); }   // MIDI only; unused here

void Game::update_audio() {
    for (auto& s : sounds)
        if (s && s->looping && s->stream && SDL_GetAudioStreamQueued(s->stream) < static_cast<int>(s->pcm.pcm.size() / 4))
            SDL_PutAudioStreamData(s->stream, s->pcm.pcm.data(), static_cast<int>(s->pcm.pcm.size()));
    for (auto& s : ss_sounds) {
        if (!s.looping || !s.stream || s.include < 0) continue;
        const auto& pcm = includes[s.include].second.pcm;
        if (SDL_GetAudioStreamQueued(s.stream) < static_cast<int>(pcm.size() / 4))
            SDL_PutAudioStreamData(s.stream, pcm.data(), static_cast<int>(pcm.size()));
    }
}

// ------------------------------------------------------------------ supersound.dll replacement
// external_define returns an id; external_call dispatches on the function name.
// Handles are returned as strings ("1", "2", ...); results are "1"/"0"/"-1" strings like the DLL.

Value bi_external_define(Ctx& C, const Args& a) {
    G(C).ext_names.push_back(S(a, 1));
    return static_cast<int>(G(C).ext_names.size() - 1);
}

static std::string norm(std::string s) {
    for (char& c : s) c = c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Value bi_external_call(Ctx& C, const Args& a) {
    Game& g = G(C);
    int id = I(a, 0);
    if (id < 0 || id >= static_cast<int>(g.ext_names.size())) g.error("external_call: undefined function");
    const std::string& fn = g.ext_names[id];
    auto handle = [&]() -> Game::SSound* {
        int h = std::atoi(S(a, 1).c_str()) - 1;
        return h >= 0 && h < static_cast<int>(g.ss_sounds.size()) ? &g.ss_sounds[h] : nullptr;
    };
    if (fn == "SS_Init") return "1";
    if (fn == "SS_LoadSound") {
        std::string want = norm(S(a, 1));
        for (size_t i = 0; i < g.includes.size(); i++) {
            if (norm(g.includes[i].first) == want) {
                Game::SSound s;
                s.include = static_cast<int>(i);
                g.ss_sounds.push_back(s);
                return std::to_string(g.ss_sounds.size());
            }
        }
        return "0";
    }
    Game::SSound* s = handle();
    if (!s) return fn == "SS_IsSoundPlaying" ? "-1" : "0";
    const PcmSound& pcm = g.includes[s->include].second;
    if (fn == "SS_PlaySound" || fn == "SS_LoopSound") {
        if (auto* st = stream_for(g, pcm, s->stream)) {
            SDL_ClearAudioStream(st);
            SDL_SetAudioStreamGain(st, static_cast<float>(s->vol));
            SDL_PutAudioStreamData(st, pcm.pcm.data(), static_cast<int>(pcm.pcm.size()));
        }
        s->looping = fn == "SS_LoopSound";
        return "1";
    }
    if (fn == "SS_StopSound") {
        if (s->stream) SDL_ClearAudioStream(s->stream);
        s->looping = false;
        return "1";
    }
    if (fn == "SS_SetSoundVol") {
        s->vol = std::atof(S(a, 2).c_str()) / 10000.0;
        if (s->stream) SDL_SetAudioStreamGain(s->stream, static_cast<float>(s->vol));
        return "1";
    }
    if (fn == "SS_IsSoundPlaying") {
        if (!g.audio_dev) return s->looping ? "1" : "0";
        return s->stream && SDL_GetAudioStreamQueued(s->stream) > 0 ? "1" : "0";
    }
    if (fn == "SS_FreeSound") {
        if (s->stream) { SDL_DestroyAudioStream(s->stream); s->stream = nullptr; }
        s->looping = false;
        return "1";
    }
    g.log_error("external_call", "unimplemented " + fn);
    return "0";
}

// ------------------------------------------------------------------ text files (saves, quality.txt)

static std::string file_path(Game& g, const std::string& name) { return g.save_dir + name; }

Value bi_file_exists(Ctx& C, const Args& a) {
    FILE* f = std::fopen(file_path(G(C), S(a, 0)).c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

static Value open_file(Ctx& C, const std::string& name, bool write) {
    Game& g = G(C);
    FILE* f = std::fopen(file_path(g, name).c_str(), write ? "wb" : "rb");
    if (!f) return -1;
    for (size_t i = 0; i < g.files.size(); i++)
        if (!g.files[i].f) { g.files[i] = {f, write}; return static_cast<int>(i); }
    g.files.push_back({f, write});
    return static_cast<int>(g.files.size() - 1);
}

static TextFile* tfile(Ctx& C, const Args& a) {
    int i = I(a, 0);
    auto& fs = G(C).files;
    return i >= 0 && i < static_cast<int>(fs.size()) && fs[i].f ? &fs[i] : nullptr;
}

Value bi_file_text_open_read(Ctx& C, const Args& a) { return open_file(C, S(a, 0), false); }
Value bi_file_text_open_write(Ctx& C, const Args& a) { return open_file(C, S(a, 0), true); }

Value bi_file_text_close(Ctx& C, const Args& a) {
    if (TextFile* t = tfile(C, a)) { std::fclose(t->f); t->f = nullptr; }
    return Value();
}

Value bi_file_text_write_string(Ctx& C, const Args& a) {
    if (TextFile* t = tfile(C, a)) { std::string s = S(a, 1); std::fwrite(s.data(), 1, s.size(), t->f); }
    return Value();
}

Value bi_file_text_writeln(Ctx& C, const Args& a) {
    if (TextFile* t = tfile(C, a)) std::fwrite("\r\n", 1, 2, t->f);
    return Value();
}

Value bi_file_text_read_string(Ctx& C, const Args& a) {
    TextFile* t = tfile(C, a);
    if (!t) return "";
    std::string s;
    int c;
    while ((c = std::fgetc(t->f)) != EOF && c != '\r' && c != '\n') s += static_cast<char>(c);
    if (c != EOF) std::ungetc(c, t->f);
    return s;
}

Value bi_file_text_read_real(Ctx& C, const Args& a) {
    TextFile* t = tfile(C, a);
    if (!t) return 0.0;
    double v = 0;
    if (std::fscanf(t->f, " %lf", &v) != 1) v = 0;
    return v;
}

Value bi_file_text_readln(Ctx& C, const Args& a) {
    TextFile* t = tfile(C, a);
    if (!t) return Value();
    int c;
    while ((c = std::fgetc(t->f)) != EOF && c != '\n') {}
    return Value();
}

}  // namespace gml
