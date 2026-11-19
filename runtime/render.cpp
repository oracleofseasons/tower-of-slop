// Drawing: views, instances, tiles, backgrounds, sprites, text (OpenGMK game/draw.rs).
//
// Coordinate model: the GM "screen" is a 640x480 render target. During the automatic draw
// each view maps its source rect onto its port; if a surface is the render target the view
// source maps onto the whole surface instead (what GM7's D3D8 path does on most hardware,
// and what ChevyRay's surface scaling relies on). Outside the draw, coordinates are 1:1.
#include <algorithm>
#include <cstring>

#include "game.h"

namespace gml {

static SDL_Texture* make_texture(SDL_Renderer* r, int w, int h, const uint8_t* rgba) {
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return t;
}

SDL_Texture* Game::sprite_texture(Sprite& s, int frame) {
    if (frame < 0 || frame >= static_cast<int>(s.frames.size())) return nullptr;
    if (!s.textures[frame]) s.textures[frame] = make_texture(renderer, s.w, s.h, s.frames[frame].data());
    return s.textures[frame];
}

SDL_Texture* Game::bg_texture(Background& b) {
    if (!b.tex && b.w > 0) b.tex = make_texture(renderer, b.w, b.h, b.rgba.data());
    return b.tex;
}

SDL_Texture* Game::font_texture(Font& f) {
    if (!f.tex) {
        std::vector<uint8_t> rgba(static_cast<size_t>(f.atlas_w) * f.atlas_h * 4, 255);
        for (size_t i = 0; i < f.atlas.size(); i++) rgba[i * 4 + 3] = f.atlas[i];
        f.tex = make_texture(renderer, f.atlas_w, f.atlas_h, rgba.data());
    }
    return f.tex;
}

SDL_Texture* Game::target_texture() {
    if (surface_target >= 0 && surface_target < static_cast<int>(surfaces.size()) && surfaces[surface_target])
        return surfaces[surface_target];
    return screen;
}

void Game::set_target(SDL_Texture* t) { SDL_SetRenderTarget(renderer, t); }

void Game::tx(double x, double y, float& ox, float& oy) const {
    ox = static_cast<float>((x - xf.vx) * xf.sx + xf.px);
    oy = static_cast<float>((y - xf.vy) * xf.sy + xf.py);
}

static void gm_color(int c, Uint8& r, Uint8& g, Uint8& b) {
    r = c & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF;
}

// Draws (sx,sy,sw,sh) of tex with its origin (ox,oy in texture pixels) at room position (x,y).
void Game::draw_texture(SDL_Texture* tex, double sx, double sy, double sw, double sh, double x, double y,
                        double ox, double oy, double xscale, double yscale, double angle, int col, double alpha) {
    if (!tex || sw <= 0 || sh <= 0) return;
    set_target(target_texture());
    float px, py;
    tx(x, y, px, py);
    double kx = xscale * xf.sx, ky = yscale * xf.sy;
    double l = px - ox * kx, r = px + (sw - ox) * kx;
    double t = py - oy * ky, b = py + (sh - oy) * ky;
    SDL_FRect src{static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sw), static_cast<float>(sh)};
    SDL_FRect dst{static_cast<float>(std::min(l, r)), static_cast<float>(std::min(t, b)),
                  static_cast<float>(std::fabs(r - l)), static_cast<float>(std::fabs(b - t))};
    SDL_FPoint centre{static_cast<float>(px - dst.x), static_cast<float>(py - dst.y)};
    int flip = SDL_FLIP_NONE;
    if (kx < 0) flip |= SDL_FLIP_HORIZONTAL;
    if (ky < 0) flip |= SDL_FLIP_VERTICAL;
    Uint8 cr, cg, cb;
    gm_color(col, cr, cg, cb);
    SDL_SetTextureColorMod(tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(tex, static_cast<Uint8>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5));
    SDL_RenderTextureRotated(renderer, tex, &src, &dst, -angle, &centre, static_cast<SDL_FlipMode>(flip));
}

void Game::draw_sprite_ext(int spr, double sub, double x, double y, double xs, double ys, double rot, int col,
                           double alpha, Instance* self) {
    if (spr < 0 || spr >= static_cast<int>(sprites.size()) || !sprites[spr]) error("draw_sprite: no such sprite");
    Sprite& s = *sprites[spr];
    if (s.frames.empty()) return;
    if (sub < 0 && self) sub = self->image_index;
    int n = static_cast<int>(s.frames.size());
    int f = static_cast<int>(std::floor(sub)) % n;
    if (f < 0) f += n;
    draw_texture(sprite_texture(s, f), 0, 0, s.w, s.h, x, y, s.origin_x, s.origin_y, xs, ys, rot, col, alpha);
}

void Game::draw_rect(double x1, double y1, double x2, double y2, int c1, int c2, int c3, int c4, bool outline) {
    set_target(target_texture());
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    auto vert = [&](double x, double y, int c) {
        SDL_Vertex v{};
        tx(x, y, v.position.x, v.position.y);
        Uint8 r, g, b;
        gm_color(c, r, g, b);
        v.color = {r / 255.0f, g / 255.0f, b / 255.0f, static_cast<float>(draw_alpha)};
        return v;
    };
    if (outline) {
        draw_rect(x1, y1, x2, y1, c1, c2, c2, c1, false);
        draw_rect(x1, y2, x2, y2, c4, c3, c3, c4, false);
        draw_rect(x1, y1, x1, y2, c1, c1, c4, c4, false);
        draw_rect(x2, y1, x2, y2, c2, c2, c3, c3, false);
        return;
    }
    // GM fills the pixels x1..x2 inclusive
    SDL_Vertex v[4] = {vert(x1, y1, c1), vert(x2 + 1, y1, c2), vert(x2 + 1, y2 + 1, c3), vert(x1, y2 + 1, c4)};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
}

void Game::draw_background_tiled(int bgi, double x, double y, double xs, double ys, bool th, bool tv, int col,
                                 double alpha, double end_x, double end_y) {
    if (bgi < 0 || bgi >= static_cast<int>(backgrounds.size()) || !backgrounds[bgi]) return;
    Background& b = *backgrounds[bgi];
    SDL_Texture* t = bg_texture(b);
    if (!t) return;
    double w = b.w * xs, h = b.h * ys;
    double sx = x, sy = y;
    if (th) { sx = std::fmod(x, w); if (sx > 0) sx -= w; }
    if (tv) { sy = std::fmod(y, h); if (sy > 0) sy -= h; }
    for (double yy = sy; tv ? yy < end_y : yy == sy; yy += h)
        for (double xx = sx; th ? xx < end_x : xx == sx; xx += w)
            draw_texture(t, 0, 0, b.w, b.h, xx, yy, 0, 0, xs, ys, 0, col, alpha);
}

// ------------------------------------------------------------------ text (LineIterator)

namespace {
struct LineIter {
    const std::string& text;
    const Font& font;
    int max_width;   // < 0: none
    size_t pos = 0;
    std::string word;
    int word_w = 0;

    bool next(std::string& line, int& line_w) {
        if (pos >= text.size()) return false;
        line.clear();
        line_w = 0;
        size_t i = pos;
        bool ended = true;
        while (i < text.size()) {
            uint8_t c = static_cast<uint8_t>(text[i++]);
            if (c == '#' || c == '\r' || c == '\n') {
                if (c == '\r' && i < text.size() && text[i] == '\n') i++;
                c = '\n';
            } else if (c == '\\' && i < text.size() && text[i] == '#') {
                i++;
                c = '#';
            } else if (!font.get(c)) {
                c = ' ';
            }
            if (c == '\n') {
                line += word; line_w += word_w; word.clear(); word_w = 0;
                ended = i >= text.size();
                break;
            }
            if (const Glyph* g = font.get(c)) { word += static_cast<char>(c); word_w += g->advance; }
            else {
                word += ' ';
                if (const Glyph* g0 = font.get(static_cast<uint8_t>(font.first))) word_w += g0->advance;
            }
            if (max_width >= 0 && line_w + word_w > max_width && line_w != 0) { ended = i >= text.size(); break; }
            if (c == ' ') { line += word; line_w += word_w; word.clear(); word_w = 0; }
        }
        if (i < text.size() || !ended) {
            pos = i;
        } else {
            line += word; line_w += word_w; word.clear(); word_w = 0;
            pos = text.size();
        }
        return true;
    }
};
}  // namespace

std::pair<int, int> Game::string_size(const std::string& s, int line_height, int max_width) {
    if (draw_font < 0 || draw_font >= static_cast<int>(fonts.size()) || !fonts[draw_font]) return {0, 0};
    const Font& f = *fonts[draw_font];
    if (line_height < 0) line_height = f.tallest;
    LineIter it{s, f, max_width};
    std::string line;
    int w = 0, lw = 0, count = 0;
    while (it.next(line, lw)) { w = std::max(w, lw); count++; }
    return {w, count * line_height};
}

static int lerp_col(int c1, int c2, double t) {
    auto ch = [&](int mask) {
        return static_cast<int>((c1 & mask) * (1.0 - t) + (c2 & mask) * t) & mask;
    };
    return ch(0xFF) + ch(0xFF00) + ch(0xFF0000);
}

void Game::draw_string(double x, double y, const std::string& s, int line_height, int max_width, int c1, int c2,
                       int c3, int c4, double alpha) {
    if (draw_font < 0 || draw_font >= static_cast<int>(fonts.size()) || !fonts[draw_font]) return;
    Font& f = *fonts[draw_font];
    SDL_Texture* tex = font_texture(f);
    if (line_height < 0) line_height = f.tallest;
    int cy = 0;
    if (valign == 1) cy = -(string_size(s, line_height, max_width).second / 2);
    else if (valign == 2) cy = -string_size(s, line_height, max_width).second;
    LineIter it{s, f, max_width};
    std::string line;
    int width = 0;
    while (it.next(line, width)) {
        int left = halign == 1 ? -(width / 2) : halign == 2 ? -width : 0;
        int cx = left;
        for (char ch : line) {
            const Glyph* g = f.get(static_cast<uint8_t>(ch));
            if (!g) {
                if (const Glyph* g0 = f.get(static_cast<uint8_t>(f.first))) cx += g0->advance;
                continue;
            }
            double t = width ? static_cast<double>(cx - left) / width : 0.0;
            int col = c1 == c2 ? c1 : lerp_col(c1, c2, t);
            (void)c3; (void)c4;   // vertical gradients unused by this game
            draw_texture(tex, g->x, g->y, g->w, g->h, x + g->distance + cx, y + cy, 0, 0, 1, 1, 0, col, alpha);
            cx += g->advance;
        }
        cy += line_height;
    }
}

// ------------------------------------------------------------------ the automatic draw (draw.rs draw)

void Game::draw_instance_default(Instance* inst) {
    if (inst->sprite_index < 0) return;
    try {
        draw_sprite_ext(inst->sprite_index, inst->image_index, inst->x, inst->y, inst->image_xscale,
                        inst->image_yscale, inst->image_angle, inst->image_blend, inst->image_alpha, inst);
    } catch (const GmlError&) {
    }
}

void Game::draw_view(const View& v) {
    SDL_Texture* target = target_texture();
    float tw = static_cast<float>(screen_w), th = static_cast<float>(screen_h);
    if (target != screen) SDL_GetTextureSize(target, &tw, &th);
    if (target == screen) {
        xf = {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.port_w) / v.w,
              static_cast<double>(v.port_h) / v.h, static_cast<double>(v.port_x), static_cast<double>(v.port_y)};
    } else {
        xf = {static_cast<double>(v.x), static_cast<double>(v.y), tw / v.w, th / v.h, 0, 0};
    }
    set_target(target);
    if (room_state.clear) {
        Uint8 r, g, b;
        gm_color(static_cast<int>(room_state.clear_color), r, g, b);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_FRect rc{static_cast<float>(v.port_x), static_cast<float>(v.port_y), static_cast<float>(v.port_w),
                     static_cast<float>(v.port_h)};
        if (target != screen) rc = {0, 0, tw, th};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_RenderFillRect(renderer, &rc);
    }

    double end_x = v.x + v.w + v.h / 2.0, end_y = v.y + v.h + v.w / 2.0;
    for (auto& b : room_state.bgs)
        if (b.visible && !b.foreground)
            draw_background_tiled(b.bg, b.x, b.y, b.xscale, b.yscale, b.tile_h, b.tile_v, 0xFFFFFF, 1.0, end_x, end_y);

    list.draw_sort();
    std::vector<Instance*> insts = list.snapshot_active();
    std::vector<Tile*> ts;
    for (auto& t : tiles) ts.push_back(&t);
    std::stable_sort(ts.begin(), ts.end(), [](Tile* a, Tile* b) { return a->depth > b->depth; });
    auto draw_inst = [&](Instance* inst) {
        if (!inst->alive() || !inst->visible) return;
        if (custom_draw[inst->object_index]) run_instance_event(8, 0, inst, inst);
        else draw_instance_default(inst);
    };
    auto draw_tile = [&](Tile* t) {
        if (t->bg < 0 || t->bg >= static_cast<int>(backgrounds.size()) || !backgrounds[t->bg]) return;
        draw_texture(bg_texture(*backgrounds[t->bg]), t->tile_x, t->tile_y, t->w, t->h, t->x, t->y, 0, 0, 1, 1, 0,
                     0xFFFFFF, 1.0);
    };
    size_t ii = 0, ti = 0;
    while (ii < insts.size() || ti < ts.size()) {
        if (ti >= ts.size()) draw_inst(insts[ii++]);
        else if (ii >= insts.size()) draw_tile(ts[ti++]);
        else if (insts[ii]->depth < ts[ti]->depth) draw_tile(ts[ti++]);
        else draw_inst(insts[ii++]);
    }

    for (auto& b : room_state.bgs)
        if (b.visible && b.foreground)
            draw_background_tiled(b.bg, b.x, b.y, b.xscale, b.yscale, b.tile_h, b.tile_v, 0xFFFFFF, 1.0, end_x, end_y);
}

void Game::draw() {
    in_draw = true;
    if (room_state.views_enabled) {
        for (auto& v : room_state.views) {
            if (!v.visible) continue;
            Instance* t = nullptr;
            if (v.target >= 0 && v.target < 100000) {
                auto l = list.snapshot_identity(v.target);
                if (!l.empty()) t = l[0];
            } else if (v.target >= 100000) {
                t = list.by_id(v.target);
            }
            if (!t) continue;
            int x = static_cast<int>(gm_round(t->x)), y = static_cast<int>(gm_round(t->y));
            if (v.hborder < v.w / 2) {
                int bl = x - v.hborder, br = x + v.hborder;
                if (bl < v.x) v.x = v.hspeed < 0 ? bl : v.x - std::min(v.x - bl, v.hspeed);
                else if (br > v.x + v.w) v.x = v.hspeed < 0 ? br - v.w : v.x + std::min(br - (v.x + v.w), v.hspeed);
            } else {
                v.x = x - v.w / 2;
            }
            v.x = std::min(std::max(v.x, 0), room_state.width - v.w);
            if (v.vborder < v.h / 2) {
                int bt = y - v.vborder, bb = y + v.vborder;
                if (bt < v.y) v.y = v.vspeed < 0 ? bt : v.y - std::min(v.y - bt, v.vspeed);
                else if (bb > v.y + v.h) v.y = v.vspeed < 0 ? bb - v.h : v.y + std::min(bb - (v.y + v.h), v.vspeed);
            } else {
                v.y = y - v.h / 2;
            }
            v.y = std::min(std::max(v.y, 0), room_state.height - v.h);
        }
        for (size_t i = 0; i < room_state.views.size(); i++)
            if (room_state.views[i].visible) draw_view(room_state.views[i]);
    } else {
        View whole;
        whole.w = room_state.width; whole.h = room_state.height;
        whole.port_w = room_state.width; whole.port_h = room_state.height;
        draw_view(whole);
    }
    xf = Xform{};
    in_draw = false;
}

void Game::present() {
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &ww, &wh);
    double scale = std::min(static_cast<double>(ww) / screen_w, static_cast<double>(wh) / screen_h);
    SDL_FRect dst{static_cast<float>((ww - screen_w * scale) / 2), static_cast<float>((wh - screen_h * scale) / 2),
                  static_cast<float>(screen_w * scale), static_cast<float>(screen_h * scale)};
    SDL_SetTextureBlendMode(screen, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer, screen, nullptr, &dst);
    SDL_RenderPresent(renderer);
    SDL_SetTextureBlendMode(screen, SDL_BLENDMODE_BLEND);
}

bool Game::init_video(bool headless) {
    if (headless) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO | (headless ? 0 : SDL_INIT_AUDIO))) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("Tower of Heaven", screen_w, screen_h, SDL_WINDOW_RESIZABLE);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    renderer = SDL_CreateRenderer(window, headless ? "software" : nullptr);
    if (!renderer) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
    if (!headless) SDL_SetRenderVSync(renderer, 1);
    screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, screen_w, screen_h);
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderTarget(renderer, screen);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (!headless) {
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
        audio_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
        if (!audio_dev) std::fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
    }
    return true;
}

}  // namespace gml
