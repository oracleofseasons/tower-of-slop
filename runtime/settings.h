// Player settings (settings.ini) and the optional changes to the game they switch on.
// With no settings file every option is off and the game is the original.
#pragma once
#include <string>
#include <vector>

namespace gml {

struct Settings {
    int refresh = 30;                           // screen redraws per second: 30, 60 or 120
    bool menu_slide_follows_direction = false;  // title menu slides the way you pressed
    std::string loaded_from;                    // the file read, if any

    // reads the first settings.ini that exists among `paths`; unknown keys are reported on stderr
    void load(const std::vector<std::string>& paths);
};

// An optional change: our own GML run before and/or after one event's code. It contains nothing
// from the game; it only reads and adjusts the instance's variables around the original code.
struct Hook {
    const char* object;
    int type, number;
    const char* before;
    const char* after;
};

std::vector<Hook> active_hooks(const Settings& s);

}  // namespace gml
