#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace gml {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
}  // namespace

void Settings::load(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        std::ifstream f(path);
        if (!f) continue;
        loaded_from = path;
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line.substr(0, line.find('#')));
            if (line.empty()) continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "refresh") {
                int r = std::atoi(value.c_str());
                refresh = r >= 120 ? 120 : r >= 60 ? 60 : 30;
            } else if (key == "menu_slide_follows_direction") {
                menu_slide_follows_direction = truthy(value);
            } else {
                std::fprintf(stderr, "%s: unknown setting %s\n", path.c_str(), key.c_str());
            }
        }
        return;
    }
}

// ------------------------------------------------------------------ the optional changes

// Title menu: right brings the next option in from the right and pushes the old one out to the
// left (the original does the opposite). The menu's step starts a slide by setting `set` to false
// (from the left) or -1 (from the right) and the leaving box's `dir`; this swaps them.
static const Hook kMenuSlide = {
    "obj_main", 3, 0,
    "__mod_set_before = set;",
    "if __mod_set_before = true {\n"
    "    if set = false { set = -1; offset = -offset; bb.dir = \"left\"; }\n"
    "    else if set = -1 { set = false; offset = -offset; bb.dir = \"right\"; }\n"
    "}"};

std::vector<Hook> active_hooks(const Settings& s) {
    std::vector<Hook> out;
    if (s.menu_slide_follows_direction) out.push_back(kMenuSlide);
    return out;
}

}  // namespace gml
