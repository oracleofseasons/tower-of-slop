// The Game Maker 7 runner reimplementation: assets, rooms, instances, the frame loop.
// Behaviour follows OpenGMK (tools/OpenGMK/gm8emulator/src/game.rs and friends).
#pragma once
#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gml_runtime.h"

namespace gml {

// ------------------------------------------------------------------ assets

struct Mask {
    int w = 0, h = 0, left = 0, right = 0, top = 0, bottom = 0;
    std::vector<uint8_t> data;
    bool point(int x, int y) const {
        return x >= left && y >= top && x <= right && y <= bottom && x < w && y < h && data[y * w + x];
    }
};

struct Sprite {
    std::string name;
    int w = 0, h = 0, origin_x = 0, origin_y = 0;
    int bbox_left = 0, bbox_right = 0, bbox_top = 0, bbox_bottom = 0;
    std::vector<std::vector<uint8_t>> frames;   // RGBA8
    std::vector<Mask> masks;                    // 1 (shared) or one per frame
    std::vector<SDL_Texture*> textures;         // lazily created
};

struct Background {
    std::string name;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    SDL_Texture* tex = nullptr;
};

struct Glyph {
    int x, y, w, h, advance, distance;
};

struct Font {
    std::string name;
    int first = 0, last = 0, tallest = 0;
    int atlas_w = 0, atlas_h = 0;
    std::vector<uint8_t> atlas;   // 8-bit coverage
    std::vector<Glyph> glyphs;    // indexed by char - first
    SDL_Texture* tex = nullptr;
    const Glyph* get(uint8_t c) const {
        if (c < first || c - first >= static_cast<int>(glyphs.size())) return nullptr;
        return &glyphs[c - first];
    }
};

struct PcmSound {
    SDL_AudioSpec spec{};
    std::vector<uint8_t> pcm;
};

struct SoundAsset {
    std::string name;
    double volume = 1.0;
    PcmSound pcm;
    SDL_AudioStream* stream = nullptr;
    bool looping = false;
};

struct ObjectAsset {
    std::string name;
    int sprite = -1, mask = -1, parent = -1;
    double depth = 0;
    bool solid = false, visible = true, persistent = false;
    std::vector<int> ancestors;   // self first, then parent chain
};

struct RoomBg {
    bool visible = false, foreground = false, tile_h = true, tile_v = true, stretch = false;
    int bg = -1;
    double x = 0, y = 0, hspeed = 0, vspeed = 0, xscale = 1, yscale = 1;
};

struct View {
    bool visible = false;
    int x = 0, y = 0, w = 0, h = 0, port_x = 0, port_y = 0, port_w = 0, port_h = 0;
    int hborder = 0, vborder = 0, hspeed = -1, vspeed = -1, target = -1;
};

struct RoomInst {
    int x, y, object, id;
};

struct Tile {
    int x, y, bg, tile_x, tile_y, w, h, depth, id;
};

struct RoomAsset {
    std::string name, caption;
    int width = 0, height = 0, speed = 30;
    bool persistent = false, clear = true, views_enabled = false;
    uint32_t clear_color = 0;
    std::vector<RoomBg> bgs;
    std::vector<View> views;
    std::vector<RoomInst> instances;
    std::vector<Tile> tiles;
};

// ------------------------------------------------------------------ instance list

class InstanceList {
  public:
    std::vector<std::unique_ptr<Instance>> storage;
    std::vector<Instance*> draw_order;
    std::unordered_map<int, std::vector<Instance*>> by_object, by_identity;

    Instance* insert(std::unique_ptr<Instance> inst);
    void draw_sort();
    void refresh_maps();
    void remove_deleted();
    std::vector<Instance*> snapshot_object(int obj) const;      // exact object, active, insertion order
    std::vector<Instance*> snapshot_identity(int obj) const;    // object or children
    std::vector<Instance*> snapshot_active() const;             // draw order
    Instance* by_id(int id) const;
    int count(int obj) const;
};

// ------------------------------------------------------------------ the game

struct TextFile {
    FILE* f = nullptr;
    bool write = false;
};

class Game {
  public:
    // assets
    std::vector<std::optional<Sprite>> sprites;
    std::vector<std::optional<Background>> backgrounds;
    std::vector<std::optional<Font>> fonts;
    std::vector<std::optional<SoundAsset>> sounds;
    std::vector<std::optional<ObjectAsset>> objects;
    std::vector<std::optional<RoomAsset>> rooms;
    std::vector<std::pair<std::string, PcmSound>> includes;
    std::vector<int> room_order;
    CodeTable code;
    std::map<int, std::vector<std::vector<int>>> holders;   // event type -> number -> object list (see fill_holders)
    std::map<std::pair<int, int>, std::vector<int>> event_holders;
    std::vector<bool> custom_draw;

    // state
    InstanceList list;
    std::vector<Tile> tiles;
    RoomAsset room_state;
    int room = -1;
    int last_instance_id = 0;
    int last_tile_id = 0;
    std::optional<int> scene_change;   // room id, or -100 for game end
    bool game_start = true;
    bool quit = false;
    std::unordered_map<int, Slot> globals;
    Instance dummy;
    uint32_t rng = 0;
    uint64_t frame_count = 0;

    // input (GM virtual key codes)
    bool key_held[256] = {}, key_pressed[256] = {}, key_released[256] = {};

    // drawing state
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* screen = nullptr;           // 640x480 GM "screen"
    std::vector<SDL_Texture*> surfaces;
    int surface_target = -1;
    bool in_draw = false;
    struct Xform { double vx = 0, vy = 0, sx = 1, sy = 1, px = 0, py = 0; } xf;
    int draw_color = 0;
    double draw_alpha = 1.0;
    int draw_font = -1, halign = 0, valign = 0;
    int screen_w = 640, screen_h = 480;
    SDL_AudioDeviceID audio_dev = 0;

    // files
    std::string save_dir;
    std::vector<TextFile> files;

    // supersound replacement
    std::vector<std::string> ext_names;
    struct SSound { int include = -1; SDL_AudioStream* stream = nullptr; bool looping = false; double vol = 1.0; };
    std::vector<SSound> ss_sounds;

    bool load(const std::string& pak_path);
    bool init_video(bool headless);
    void start();
    void frame();
    void present();
    void update_audio();

    // events
    void run_object_event(int type, int number, Instance* other = nullptr);
    void run_instance_event(int type, int number, Instance* inst, Instance* other);
    void run_actions(const std::vector<Action>& acts, Instance* inst, Instance* other);
    void run_code(CodeFn fn, Instance* self, Instance* other);
    void run_alarms();
    void run_key_press_events();
    void run_collisions();
    void run_other_event(int number);
    void fill_holders();
    const std::vector<int>* holders_for(int type, int number) const;

    // rooms & instances
    void load_room(int id);
    Instance* create_instance(double x, double y, int obj, int id, bool run_create);
    void destroy_instance(Instance* inst);
    std::vector<Instance*> resolve(Ctx& C, const Value& target);

    // movement & collision
    void process_speeds();
    void apply_speeds(Instance* inst);
    void update_bbox(Instance* inst);
    const Sprite* collision_sprite(const Instance* inst) const;
    bool check_collision(Instance* a, Instance* b);
    Instance* collision_solid(Instance* inst);
    template <class F> Instance* find_instance_with(Ctx& C, int obj, F pred);

    // variables
    Value get_builtin(Ctx& C, Instance* inst, BV b, const Idx& idx);
    void set_builtin(Ctx& C, Instance* inst, BV b, const Idx& idx, const Value& v);
    const char* var_name(int id) const;

    // drawing
    void draw();
    void draw_view(const View& v);
    void draw_instance_default(Instance* inst);
    SDL_Texture* sprite_texture(Sprite& s, int frame);
    SDL_Texture* bg_texture(Background& b);
    SDL_Texture* font_texture(Font& f);
    SDL_Texture* target_texture();
    void set_target(SDL_Texture* t);
    void tx(double x, double y, float& ox, float& oy) const;
    void draw_texture(SDL_Texture* tex, double sx, double sy, double sw, double sh, double x, double y,
                      double ox, double oy, double xscale, double yscale, double angle, int col, double alpha);
    void draw_sprite_ext(int spr, double sub, double x, double y, double xs, double ys, double rot, int col,
                         double alpha, Instance* self);
    void draw_rect(double x1, double y1, double x2, double y2, int c1, int c2, int c3, int c4, bool outline);
    void draw_background_tiled(int bg, double x, double y, double xs, double ys, bool th, bool tv, int col,
                               double alpha, double end_x, double end_y);
    void draw_string(double x, double y, const std::string& s, int line_height, int max_width, int c1, int c2,
                     int c3, int c4, double alpha);
    std::pair<int, int> string_size(const std::string& s, int line_height, int max_width);

    // rng (GM8 Delphi LCG)
    double rand_next(double bound);
    int rand_int(uint32_t bound);

    [[noreturn]] void error(const std::string& msg);
    void log_error(const std::string& where, const std::string& msg);
};

extern Game* g_game;

}  // namespace gml
