// Entry point: window, input, frame timing.
//   toh [--pak FILE] [--save-dir DIR] [--headless FRAMES] [--shot FILE.bmp] [--shot-every N DIR]
//       [--room ID|NAME]   (start in that room instead of the first; for testing)
//       [--record FILE]    (instrumented builds: save your key presses for tools/trace/compare.sh)
//       [--warp ID|NAME]   (boot normally; the first entry into level_1 goes to this room instead)
//       [--keys "frame:KEY[:FRAMES],..."]   (headless input, KEY = GM vk code, held FRAMES, default 1)
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <map>
#include <string>

#include "game.h"

using namespace gml;

static int vk_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_LEFT: return 37;
    case SDL_SCANCODE_UP: return 38;
    case SDL_SCANCODE_RIGHT: return 39;
    case SDL_SCANCODE_DOWN: return 40;
    case SDL_SCANCODE_ESCAPE: return 27;
    case SDL_SCANCODE_RETURN: return 13;
    case SDL_SCANCODE_SPACE: return 32;
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 16;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 17;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 18;
    default: break;
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 112 + (sc - SDL_SCANCODE_F1);
    return -1;
}

static void save_screen(Game& g, const std::string& path) {
    SDL_SetRenderTarget(g.renderer, g.screen);
    SDL_Surface* s = SDL_RenderReadPixels(g.renderer, nullptr);
    if (s) {
        SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
    }
}

int main(int argc, char** argv) {
    std::string pak = "toh.pak", save_dir, shot, shot_dir, start_room, warp, record;
    long headless = -1, shot_every = 0;
    std::multimap<long, std::pair<int, long>> scripted_keys;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--pak") pak = next();
        else if (a == "--save-dir") save_dir = next();
        else if (a == "--headless") headless = std::atol(next().c_str());
        else if (a == "--shot") shot = next();
        else if (a == "--room") start_room = next();
        else if (a == "--warp") warp = next();
        else if (a == "--record") record = next();
        else if (a == "--shot-every") { shot_every = std::atol(next().c_str()); shot_dir = next(); }
        else if (a == "--keys") {
            std::string spec = next();
            size_t p = 0;
            while (p < spec.size()) {
                size_t comma = spec.find(',', p);
                std::string item = spec.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    size_t colon2 = item.find(':', colon + 1);
                    long dur = colon2 == std::string::npos ? 1 : std::atol(item.substr(colon2 + 1).c_str());
                    scripted_keys.insert({std::atol(item.substr(0, colon).c_str()),
                                          {std::atoi(item.substr(colon + 1).c_str()), dur}});
                }
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    static Game game;
    g_game = &game;
    if (!game.init_video(headless >= 0)) return 1;
    if (!game.load(pak)) return 1;
    if (save_dir.empty()) {
        char* pref = SDL_GetPrefPath("Askiisoft", "Tower of Heaven");
        save_dir = pref ? pref : "./";
        SDL_free(pref);
    } else if (save_dir.back() != '/') {
        save_dir += '/';
    }
    game.save_dir = save_dir;
    game.rng = static_cast<uint32_t>(SDL_GetTicksNS());
    if (headless >= 0) game.rng = 0;   // deterministic runs

    auto find_room = [&](const std::string& name) {
        for (size_t i = 0; i < game.rooms.size(); i++)
            if (game.rooms[i] && (game.rooms[i]->name == name || std::to_string(i) == name)) return static_cast<int>(i);
        std::fprintf(stderr, "no room %s\n", name.c_str());
        std::exit(2);
    };
    if (!warp.empty()) game.warp_room = find_room(warp);
    if (!start_room.empty()) {
        game.load_room(find_room(start_room));
    } else {
        game.start();
    }

    // Recording: a key change applied at the poll before frame G must be replayed by the
    // instrumented GML's keyboard_key_press during frame G-1, whose begin step sees the trace
    // counter's value at the start of G-1. So each event is stamped with that value.
    int trace_var = game.interp.find_var("__trace_f");
    if (!record.empty() && trace_var < 0) {
        std::fprintf(stderr, "--record needs an instrumented build (tools/trace/record.sh)\n");
        return 2;
    }
    auto trace_counter = [&]() -> long {
        auto it = game.globals.find(trace_var);
        if (it == game.globals.end() || !it->second.has(0)) return -1;
        return static_cast<long>(it->second.v.at(0).real());
    };
    long counter_start = -1;
    struct Rec { long f; int vk; bool down; };
    std::vector<Rec> recorded;
    int unreplayable = 0;

    auto apply_key = [&](int vk, bool down) {
        if (vk < 0 || vk > 255) return;
        if (!record.empty() && game.key_held[vk] != down) {
            // polled before frame G: counter_start is G-1's starting value; if the logger didn't
            // run during G-1 (no host on a transition frame), the injected code can't replay it
            if (counter_start < 0 || trace_counter() == counter_start) unreplayable++;
            else recorded.push_back({counter_start, vk, down});
        }
        game.key_event(vk, down);
    };

    Uint64 next_frame = SDL_GetTicksNS();
    std::map<int, long> scripted_release;
    while (!game.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) game.quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
                int vk = vk_from_scancode(e.key.scancode);
                bool down = e.type == SDL_EVENT_KEY_DOWN;
                // a repeat only matters for a key a room change cleared (see Game::load_room)
                if (e.key.repeat && (vk < 0 || vk > 255 || game.key_held[vk])) continue;
                apply_key(vk, down);
            }
        }
        for (auto [vk, down] : game.simulated_keys) game.key_event(vk, down);
        game.simulated_keys.clear();
        if (headless >= 0) {
            long f = static_cast<long>(game.frame_count);
            for (auto it = scripted_release.begin(); it != scripted_release.end();) {
                if (it->second <= f) { apply_key(it->first, false); it = scripted_release.erase(it); }
                else ++it;
            }
            auto range = scripted_keys.equal_range(f);
            for (auto it = range.first; it != range.second; ++it) {
                apply_key(it->second.first, true);
                scripted_release[it->second.first] = f + it->second.second;
            }
        }

        counter_start = trace_counter();
        game.frame();
        game.update_audio();

        if (headless >= 0) {
            if (shot_every > 0 && game.frame_count % shot_every == 0)
                save_screen(game, shot_dir + "/frame" + std::to_string(game.frame_count) + ".bmp");
            if (static_cast<long>(game.frame_count) >= headless) break;
            continue;
        }
        game.present();
        int speed = game.room_state.speed > 0 ? game.room_state.speed : 30;
        next_frame += 1000000000ull / speed;
        Uint64 now = SDL_GetTicksNS();
        if (next_frame > now) SDL_DelayNS(next_frame - now);
        else if (now - next_frame > 250000000ull) next_frame = now;   // don't spiral after a stall
    }
    if (!shot.empty()) save_screen(game, shot);
    if (!record.empty()) {
        // pair presses with releases: "frame:vk:frames,..."
        std::string out;
        size_t pairs = 0;
        for (size_t i = 0; i < recorded.size(); i++) {
            if (!recorded[i].down) continue;
            long dur = 1000000;
            for (size_t j = i + 1; j < recorded.size(); j++)
                if (recorded[j].vk == recorded[i].vk && !recorded[j].down) { dur = std::max(1L, recorded[j].f - recorded[i].f); break; }
            out += (pairs++ ? "," : "") + std::to_string(recorded[i].f) + ":" + std::to_string(recorded[i].vk) + ":" + std::to_string(dur);
        }
        if (FILE* f = std::fopen(record.c_str(), "w")) { std::fputs(out.c_str(), f); std::fputc('\n', f); std::fclose(f); }
        std::fprintf(stderr, "recorded %zu key presses to %s", pairs, record.c_str());
        if (unreplayable) std::fprintf(stderr, " (%d events fell on transition frames and were dropped)", unreplayable);
        std::fprintf(stderr, "\n");
    }
    SDL_Quit();
    return 0;
}
