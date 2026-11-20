// The homebrew menu icon: the release's toh.nro carries a plain icon (nothing from the game). On
// first run, when the game's data is read from your exe, the exe's own icon is copied into your
// toh.nro on the SD card, so the menu shows it from then on.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "extract.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb_image_write/stb_image_write.h"

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; }
uint64_t rd64(const std::vector<uint8_t>& d, size_t o) { uint64_t v; std::memcpy(&v, &d[o], 8); return v; }
void wr64(std::vector<uint8_t>& d, size_t o, uint64_t v) { std::memcpy(&d[o], &v, 8); }

}  // namespace

// the icon currently in an NRO, read without loading the whole file
static std::vector<uint8_t> nro_icon(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t head[0x20], aset[0x38];
    uint32_t at = 0;
    if (std::fread(head, 1, sizeof head, f) == sizeof head && std::memcmp(head + 0x10, "NRO0", 4) == 0) {
        std::memcpy(&at, head + 0x18, 4);
        uint64_t off = 0, size = 0;
        if (std::fseek(f, at, SEEK_SET) == 0 && std::fread(aset, 1, sizeof aset, f) == sizeof aset &&
            std::memcmp(aset, "ASET", 4) == 0) {
            std::memcpy(&off, aset + 0x08, 8);
            std::memcpy(&size, aset + 0x10, 8);
            if (size < (1u << 20) && std::fseek(f, static_cast<long>(at + off), SEEK_SET) == 0) {
                out.resize(size);
                if (std::fread(out.data(), 1, size, f) != size) out.clear();
            }
        }
    }
    std::fclose(f);
    return out;
}

static std::vector<uint8_t> icon_jpeg_from_exe(const std::string& exe);
static bool write_icon(const std::string& self_nro, const std::vector<uint8_t>& jpg);

// Puts the game's icon into this NRO when it isn't there (first run, or after the NRO was replaced
// with a new release). The icon comes from the exe once and is kept as icon.jpg in the data folder.
bool toh_switch_install_icon(const std::string& exe, const std::string& self_nro, const std::string& data_dir) {
    std::string saved = data_dir + "icon.jpg";
    std::vector<uint8_t> jpg = read_file(saved);
    if (jpg.empty()) {
        if (exe.empty()) return false;
        jpg = icon_jpeg_from_exe(exe);
        if (jpg.empty()) return false;
        write_file(saved, jpg);
    }
    if (nro_icon(self_nro) == jpg) return true;
    return write_icon(self_nro, jpg);
}

static std::vector<uint8_t> icon_jpeg_from_exe(const std::string& exe) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::string err;
    if (!gml::extract_icon(exe, w, h, rgba, err)) {
        std::fprintf(stderr, "[icon] %s\n", err.c_str());
        return {};
    }
    // 256x256, pixels kept sharp, transparency over black
    const int size = 256;
    std::vector<uint8_t> rgb(static_cast<size_t>(size) * size * 3);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const uint8_t* s = &rgba[(static_cast<size_t>(y * h / size) * w + x * w / size) * 4];
            uint8_t* o = &rgb[(static_cast<size_t>(y) * size + x) * 3];
            for (int c = 0; c < 3; c++) o[c] = static_cast<uint8_t>(s[c] * s[3] / 255);
        }
    std::vector<uint8_t> jpg;
    stbi_write_jpg_to_func([](void* ctx, void* data, int n) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + n);
    }, &jpg, size, size, 3, rgb.data(), 95);
    return jpg;
}

static bool write_icon(const std::string& self_nro, const std::vector<uint8_t>& jpg) {
    // NRO: the asset section starts at the header's size field (0x18); "ASET", version, then
    // (offset, size) pairs for icon, NACP and RomFS, relative to the section
    std::vector<uint8_t> nro = read_file(self_nro);
    if (nro.size() < 0x80 || std::memcmp(&nro[0x10], "NRO0", 4) != 0) {
        std::fprintf(stderr, "[icon] %s is not an NRO\n", self_nro.c_str());
        return false;
    }
    size_t aset = rd32(nro, 0x18);
    if (aset + 0x38 > nro.size() || std::memcmp(&nro[aset], "ASET", 4) != 0) {
        std::fprintf(stderr, "[icon] no asset section\n");
        return false;
    }
    uint64_t nacp_off = rd64(nro, aset + 0x18), nacp_size = rd64(nro, aset + 0x20);
    uint64_t romfs_off = rd64(nro, aset + 0x28), romfs_size = rd64(nro, aset + 0x30);
    if (aset + nacp_off + nacp_size > nro.size() || (romfs_size && aset + romfs_off + romfs_size > nro.size())) {
        std::fprintf(stderr, "[icon] bad asset section\n");
        return false;
    }
    std::vector<uint8_t> out(nro.begin(), nro.begin() + static_cast<long>(aset + 0x38));
    wr64(out, aset + 0x08, 0x38);
    wr64(out, aset + 0x10, jpg.size());
    out.insert(out.end(), jpg.begin(), jpg.end());
    wr64(out, aset + 0x18, out.size() - aset);
    out.insert(out.end(), nro.begin() + static_cast<long>(aset + nacp_off), nro.begin() + static_cast<long>(aset + nacp_off + nacp_size));
    if (romfs_size) {
        wr64(out, aset + 0x28, out.size() - aset);
        out.insert(out.end(), nro.begin() + static_cast<long>(aset + romfs_off), nro.begin() + static_cast<long>(aset + romfs_off + romfs_size));
    }
    std::string tmp = self_nro + ".tmp";
    if (!write_file(tmp, out) || !SDL_RenamePath(tmp.c_str(), self_nro.c_str())) {
        std::fprintf(stderr, "[icon] could not rewrite %s\n", self_nro.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    std::fprintf(stderr, "[icon] installed the game's icon into %s\n", self_nro.c_str());
    return true;
}
