// SDL3 subset for the Switch 2 (see include/SDL3/SDL.h): renderer on EGL + OpenGL 4.3 core,
// sound on audout (48 kHz stereo) with a small mixer, the controller as a keyboard.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include <switch.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::string g_error;
const char* const kDataDir = "sdmc:/switch/toh/";
bool fail(const char* msg) {
    g_error = msg;
    std::fprintf(stderr, "[switch] error: %s\n", msg);
    return false;
}
}  // namespace

// ================================================================== log and crash report
// Everything written to stderr goes to sdmc:/switch/toh/toh.log. A crash writes its address and
// a backtrace there (as offsets into toh.elf, for addr2line) and returns to the homebrew menu.

// crash addresses are given relative to SDL_Init; `nm toh.elf | grep SDL_Init` turns them into elf addresses
static u64 anchor() { return reinterpret_cast<u64>(&SDL_Init); }

static void open_log() {
    static bool open = false;
    if (open) return;
    open = true;
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataDir, 0777);
    if (std::freopen("sdmc:/switch/toh/toh.log", "w", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[switch] log open; SDL_Init at %p\n", reinterpret_cast<void*>(anchor()));
    std::set_terminate([] {
        std::fprintf(stderr, "[switch] uncaught exception");
        if (auto ex = std::current_exception()) {
            try { std::rethrow_exception(ex); }
            catch (const std::exception& e) { std::fprintf(stderr, ": %s", e.what()); }
            catch (...) {}
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        svcExitProcess();
    });
}

extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 base = anchor();
    std::fprintf(stderr, "[switch] CRASH: error_desc %u, esr 0x%x\n", ctx->error_desc, ctx->esr);
    std::fprintf(stderr, "[switch]   pc  0x%lx (SDL_Init%+ld)\n", ctx->pc.x, static_cast<long>(ctx->pc.x - base));
    std::fprintf(stderr, "[switch]   lr  0x%lx (SDL_Init%+ld)\n", ctx->lr.x, static_cast<long>(ctx->lr.x - base));
    std::fprintf(stderr, "[switch]   far 0x%lx, sp 0x%lx\n", ctx->far.x, ctx->sp.x);
    u64 fp = ctx->fp.x;
    for (int i = 0; i < 32 && fp && (fp & 7) == 0; i++) {
        u64 ret = reinterpret_cast<u64*>(fp)[1];
        std::fprintf(stderr, "[switch]   #%d 0x%lx (SDL_Init%+ld)\n", i, ret, static_cast<long>(ret - base));
        u64 next = reinterpret_cast<u64*>(fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    std::fflush(stderr);
    svcExitProcess();
}
}

// ================================================================== basics

bool SDL_Init(Uint32) {
    open_log();
    return true;
}
static void audio_shutdown();
static void video_shutdown();
void SDL_Quit() {
    std::fprintf(stderr, "[switch] quitting\n");
    audio_shutdown();
    video_shutdown();
    std::fprintf(stderr, "[switch] shut down\n");
}
const char* SDL_GetError() { return g_error.c_str(); }
bool SDL_SetHint(const char*, const char*) { return true; }
void SDL_free(void* p) { std::free(p); }
Uint64 SDL_GetTicksNS() { return armTicksToNs(armGetSystemTick()); }
void SDL_DelayNS(Uint64 ns) { svcSleepThread(static_cast<s64>(ns)); }

// ================================================================== files

struct SDL_IOStream {
    const Uint8* p;
    size_t n;
};
SDL_IOStream* SDL_IOFromConstMem(const void* mem, size_t size) {
    return new SDL_IOStream{static_cast<const Uint8*>(mem), size};
}

bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info) {
    struct stat st;
    if (stat(path, &st) != 0) return fail("no such file");
    info->type = S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_OTHER;
    info->size = static_cast<Uint64>(st.st_size);
    return true;
}

bool SDL_RenamePath(const char* oldpath, const char* newpath) {
    std::remove(newpath);   // the SD card's FAT won't rename over a file
    return std::rename(oldpath, newpath) == 0 || fail("rename failed");
}

const char* SDL_GetBasePath() { return kDataDir; }

char* SDL_GetPrefPath(const char*, const char*) {
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataDir, 0777);
    return strdup(kDataDir);
}

bool SDL_ShowSimpleMessageBox(Uint32, const char* title, const char* message, SDL_Window*) {
    ErrorApplicationConfig c;
    errorApplicationCreate(&c, title, message);
    errorApplicationShow(&c);
    return true;
}

// ================================================================== video

struct SDL_Window {
    NWindow* nw;
};
struct SDL_Texture {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    float r = 1, g = 1, b = 1, a = 1;
};
struct SDL_Renderer {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint program = 0, vbo = 0, vao = 0, white = 0;
    GLint u_proj = -1;
    SDL_Texture* target = nullptr;
    Uint8 dr = 0, dg = 0, db = 0, da = 255;
    SDL_BlendMode draw_blend = SDL_BLENDMODE_NONE;
    int out_w = 1280, out_h = 720;
};

namespace {
SDL_Renderer* g_renderer = nullptr;

const char* const kVert = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
layout(location = 2) in vec2 uv;
uniform vec4 proj;   // x scale, y scale, x offset, y offset
out vec4 v_col;
out vec2 v_uv;
void main() {
    gl_Position = vec4(pos.x * proj.x + proj.z, pos.y * proj.y + proj.w, 0.0, 1.0);
    v_col = col;
    v_uv = uv;
})";
const char* const kFrag = R"(#version 330 core
in vec4 v_col;
in vec2 v_uv;
uniform sampler2D tex;
out vec4 frag;
void main() { frag = texture(tex, v_uv) * v_col; })";

GLuint shader(GLenum kind, const char* src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[switch] shader compile failed: %s\n", log);
    }
    return s;
}

struct Vtx { float x, y, r, g, b, a, u, v; };

// pixel coordinates of the current target -> clip space. Textures keep row 0 at v = 0, so a
// texture target is drawn with y up (its row 0 is the framebuffer's bottom row) and the
// window with y down.
void bind_target(SDL_Renderer* r) {
    int w = r->target ? r->target->w : r->out_w, h = r->target ? r->target->h : r->out_h;
    glBindFramebuffer(GL_FRAMEBUFFER, r->target ? r->target->fbo : 0);
    glViewport(0, 0, w, h);
    float sy = r->target ? 2.0f / h : -2.0f / h;
    glUniform4f(r->u_proj, 2.0f / w, sy, -1.0f, r->target ? -1.0f : 1.0f);
}

void set_blend(SDL_BlendMode m) {
    if (m == SDL_BLENDMODE_BLEND) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void draw(SDL_Renderer* r, GLuint tex, SDL_BlendMode blend, const Vtx* v, int n) {
    set_blend(blend);
    glBindTexture(GL_TEXTURE_2D, tex ? tex : r->white);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(Vtx) * n), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n);
}
}  // namespace

SDL_Window* SDL_CreateWindow(const char*, int, int, Uint64) { return new SDL_Window{nwindowGetDefault()}; }

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char*) {
    auto* r = new SDL_Renderer;
    r->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!r->display || !eglInitialize(r->display, nullptr, nullptr)) { fail("EGL init failed"); return nullptr; }
    eglBindAPI(EGL_OPENGL_API);
    static const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    eglChooseConfig(r->display, cfg_attrs, &config, 1, &n);
    if (n == 0) { fail("no EGL config"); return nullptr; }
    r->surface = eglCreateWindowSurface(r->display, config, window->nw, nullptr);
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                       EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    r->context = eglCreateContext(r->display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (!r->surface || !r->context) { fail("EGL surface/context failed"); return nullptr; }
    eglMakeCurrent(r->display, r->surface, r->surface, r->context);
    if (!gladLoadGL()) { fail("could not load OpenGL"); return nullptr; }
    std::fprintf(stderr, "[switch] GL %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    EGLint w = 0, h = 0;
    eglQuerySurface(r->display, r->surface, EGL_WIDTH, &w);
    eglQuerySurface(r->display, r->surface, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) { r->out_w = w; r->out_h = h; }

    r->program = glCreateProgram();
    glAttachShader(r->program, shader(GL_VERTEX_SHADER, kVert));
    glAttachShader(r->program, shader(GL_FRAGMENT_SHADER, kFrag));
    glLinkProgram(r->program);
    GLint linked = 0;
    glGetProgramiv(r->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        glGetProgramInfoLog(r->program, sizeof log, nullptr, log);
        std::fprintf(stderr, "[switch] shader link failed: %s\n", log);
    }
    glUseProgram(r->program);
    r->u_proj = glGetUniformLocation(r->program, "proj");
    glUniform1i(glGetUniformLocation(r->program, "tex"), 0);
    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(24));
    const Uint8 white[4] = {255, 255, 255, 255};
    glGenTextures(1, &r->white);
    glBindTexture(GL_TEXTURE_2D, r->white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_renderer = r;
    bind_target(r);
    std::fprintf(stderr, "[switch] renderer ready, %dx%d, GL error 0x%x\n", r->out_w, r->out_h, glGetError());
    return r;
}

bool SDL_SetRenderVSync(SDL_Renderer* r, int vsync) {
    bool ok = eglSwapInterval(r->display, vsync) == EGL_TRUE;
    std::fprintf(stderr, "[switch] vsync %d: %s\n", vsync, ok ? "ok" : "failed");
    return ok;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer*, SDL_PixelFormat, SDL_TextureAccess access, int w, int h) {
    auto* t = new SDL_Texture;
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (access == SDL_TEXTUREACCESS_TARGET) {
        std::fprintf(stderr, "[switch] render target %dx%d\n", w, h);
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) std::fprintf(stderr, "[switch] render target %dx%d incomplete: 0x%x\n", w, h, st);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (g_renderer) bind_target(g_renderer);
    }
    return t;
}

bool SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch) {
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    if (rect) glTexSubImage2D(GL_TEXTURE_2D, 0, rect->x, rect->y, rect->w, rect->h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return true;
}

void SDL_DestroyTexture(SDL_Texture* t) {
    if (!t) return;
    if (g_renderer && g_renderer->target == t) SDL_SetRenderTarget(g_renderer, nullptr);
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    glDeleteTextures(1, &t->tex);
    delete t;
}

bool SDL_GetTextureSize(SDL_Texture* t, float* w, float* h) {
    if (w) *w = static_cast<float>(t->w);
    if (h) *h = static_cast<float>(t->h);
    return true;
}
bool SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode) { t->blend = mode; return true; }
bool SDL_SetTextureScaleMode(SDL_Texture*, SDL_ScaleMode) { return true; }   // always nearest
bool SDL_SetTextureColorMod(SDL_Texture* t, Uint8 r, Uint8 g, Uint8 b) {
    t->r = r / 255.0f; t->g = g / 255.0f; t->b = b / 255.0f;
    return true;
}
bool SDL_SetTextureAlphaMod(SDL_Texture* t, Uint8 a) { t->a = a / 255.0f; return true; }

bool SDL_SetRenderTarget(SDL_Renderer* r, SDL_Texture* t) {
    r->target = t;
    bind_target(r);
    return true;
}
bool SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 red, Uint8 g, Uint8 b, Uint8 a) {
    r->dr = red; r->dg = g; r->db = b; r->da = a;
    return true;
}
bool SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode) { r->draw_blend = mode; return true; }

bool SDL_RenderClear(SDL_Renderer* r) {
    glClearColor(r->dr / 255.0f, r->dg / 255.0f, r->db / 255.0f, r->da / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}

bool SDL_RenderFillRect(SDL_Renderer* r, const SDL_FRect* rc) {
    int w = r->target ? r->target->w : r->out_w, h = r->target ? r->target->h : r->out_h;
    SDL_FRect f = rc ? *rc : SDL_FRect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    float cr = r->dr / 255.0f, cg = r->dg / 255.0f, cb = r->db / 255.0f, ca = r->da / 255.0f;
    Vtx v[6] = {{f.x, f.y, cr, cg, cb, ca, 0, 0},           {f.x + f.w, f.y, cr, cg, cb, ca, 0, 0},
                {f.x + f.w, f.y + f.h, cr, cg, cb, ca, 0, 0}, {f.x, f.y, cr, cg, cb, ca, 0, 0},
                {f.x + f.w, f.y + f.h, cr, cg, cb, ca, 0, 0}, {f.x, f.y + f.h, cr, cg, cb, ca, 0, 0}};
    draw(r, 0, r->draw_blend, v, 6);
    return true;
}

bool SDL_RenderTextureRotated(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* src, const SDL_FRect* dst,
                              double angle, const SDL_FPoint* center, SDL_FlipMode flip) {
    SDL_FRect s = src ? *src : SDL_FRect{0, 0, static_cast<float>(t->w), static_cast<float>(t->h)};
    int ow = r->target ? r->target->w : r->out_w, oh = r->target ? r->target->h : r->out_h;
    SDL_FRect d = dst ? *dst : SDL_FRect{0, 0, static_cast<float>(ow), static_cast<float>(oh)};
    float u0 = s.x / t->w, v0 = s.y / t->h, u1 = (s.x + s.w) / t->w, v1 = (s.y + s.h) / t->h;
    if (flip & SDL_FLIP_HORIZONTAL) std::swap(u0, u1);
    if (flip & SDL_FLIP_VERTICAL) std::swap(v0, v1);
    float cx = center ? center->x : d.w / 2, cy = center ? center->y : d.h / 2;
    // SDL rotates clockwise (y down) about the centre, given relative to dst's top left
    double rad = angle * 3.14159265358979323846 / 180.0;
    float c = static_cast<float>(std::cos(rad)), sn = static_cast<float>(std::sin(rad));
    auto pt = [&](float x, float y, float u, float vv) {
        float px = x - cx, py = y - cy;
        return Vtx{d.x + cx + px * c - py * sn, d.y + cy + px * sn + py * c, t->r, t->g, t->b, t->a, u, vv};
    };
    Vtx tl = pt(0, 0, u0, v0), tr = pt(d.w, 0, u1, v0), br = pt(d.w, d.h, u1, v1), bl = pt(0, d.h, u0, v1);
    Vtx v[6] = {tl, tr, br, tl, br, bl};
    draw(r, t->tex, t->blend, v, 6);
    return true;
}

bool SDL_RenderTexture(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* src, const SDL_FRect* dst) {
    return SDL_RenderTextureRotated(r, t, src, dst, 0.0, nullptr, SDL_FLIP_NONE);
}

bool SDL_RenderGeometry(SDL_Renderer* r, SDL_Texture* t, const SDL_Vertex* vertices, int, const int* indices,
                        int num_indices) {
    std::vector<Vtx> v;
    v.reserve(num_indices);
    for (int i = 0; i < num_indices; i++) {
        const SDL_Vertex& s = vertices[indices[i]];
        v.push_back({s.position.x, s.position.y, s.color.r, s.color.g, s.color.b, s.color.a, s.tex_coord.x, s.tex_coord.y});
    }
    // SDL_RenderGeometry blends as the draw blend mode with no texture, else the texture's
    draw(r, t ? t->tex : 0, t ? t->blend : r->draw_blend, v.data(), static_cast<int>(v.size()));
    return true;
}

bool SDL_GetCurrentRenderOutputSize(SDL_Renderer* r, int* w, int* h) {
    if (w) *w = r->target ? r->target->w : r->out_w;
    if (h) *h = r->target ? r->target->h : r->out_h;
    return true;
}

SDL_Surface* SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect) {
    int tw = r->target ? r->target->w : r->out_w, th = r->target ? r->target->h : r->out_h;
    SDL_Rect rc = rect ? *rect : SDL_Rect{0, 0, tw, th};
    rc.w = std::max(0, std::min(rc.w, tw - rc.x));
    rc.h = std::max(0, std::min(rc.h, th - rc.y));
    auto* s = new SDL_Surface{0, SDL_PIXELFORMAT_ABGR8888, rc.w, rc.h, rc.w * 4, std::malloc(static_cast<size_t>(rc.w) * rc.h * 4 + 1)};
    if (r->target) {
        // texture targets are stored with row 0 at the bottom of the framebuffer: rows come out top first
        glReadPixels(rc.x, rc.y, rc.w, rc.h, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    } else {
        std::vector<Uint8> tmp(static_cast<size_t>(rc.w) * rc.h * 4);
        glReadPixels(rc.x, th - rc.y - rc.h, rc.w, rc.h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
        for (int y = 0; y < rc.h; y++)
            std::memcpy(static_cast<Uint8*>(s->pixels) + static_cast<size_t>(y) * s->pitch,
                        tmp.data() + static_cast<size_t>(rc.h - 1 - y) * s->pitch, s->pitch);
    }
    return s;
}

static void video_shutdown() {
    SDL_Renderer* r = g_renderer;
    if (!r) return;
    glFinish();
    eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(r->display, r->context);
    eglDestroySurface(r->display, r->surface);
    eglTerminate(r->display);
    g_renderer = nullptr;
    std::fprintf(stderr, "[switch] EGL closed\n");
}

bool SDL_RenderPresent(SDL_Renderer* r) {
    eglSwapBuffers(r->display, r->surface);
    return true;
}

SDL_Surface* SDL_ConvertSurface(SDL_Surface* s, SDL_PixelFormat) {
    size_t n = static_cast<size_t>(s->pitch) * s->h;
    auto* c = new SDL_Surface{0, SDL_PIXELFORMAT_ABGR8888, s->w, s->h, s->pitch, std::malloc(n + 1)};
    std::memcpy(c->pixels, s->pixels, n);
    return c;
}
void SDL_DestroySurface(SDL_Surface* s) {
    if (!s) return;
    std::free(s->pixels);
    delete s;
}
bool SDL_SaveBMP(SDL_Surface*, const char*) { return fail("no screenshots on Switch"); }

// ================================================================== audio

struct SDL_AudioStream {
    SDL_AudioSpec spec{};
    std::vector<Uint8> data;
    size_t read = 0;        // bytes consumed
    double frac = 0;        // position between the current frame and the next
    float gain = 1.0f;
    bool bound = false;
};

namespace {
std::mutex g_audio_mutex;
std::atomic<bool> g_audio_quit{false};
bool g_audio_running = false;
Thread g_audio_thread;
AudioOutBuffer g_bufs[4]{};
std::vector<SDL_AudioStream*> g_streams;
constexpr int kOutRate = 48000;
constexpr uint32_t kBufferFrames = 1024;

int frame_bytes(const SDL_AudioSpec& s) { return s.channels * (s.format == SDL_AUDIO_S16 ? 2 : 1); }

float sample(const SDL_AudioStream* s, size_t frame, int ch) {
    size_t off = s->read + frame * frame_bytes(s->spec) + (s->spec.format == SDL_AUDIO_S16 ? ch * 2 : ch);
    if (s->spec.format == SDL_AUDIO_S16) {
        int16_t v;
        std::memcpy(&v, &s->data[off], 2);
        return v / 32768.0f;
    }
    return (s->data[off] - 128) / 128.0f;
}

void mix(int16_t* out, uint32_t frames) {
    std::vector<float> acc(static_cast<size_t>(frames) * 2, 0.0f);
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        for (SDL_AudioStream* s : g_streams) {
            int fb = frame_bytes(s->spec);
            if (fb <= 0 || s->spec.freq <= 0) continue;
            double step = static_cast<double>(s->spec.freq) / kOutRate;
            for (uint32_t i = 0; i < frames; i++) {
                size_t avail = (s->data.size() - s->read) / fb;
                if (avail < 2) break;
                float t = static_cast<float>(s->frac);
                for (int ch = 0; ch < 2; ch++) {
                    int src_ch = s->spec.channels == 1 ? 0 : ch;
                    float a = sample(s, 0, src_ch), b = sample(s, 1, src_ch);
                    acc[i * 2 + ch] += (a + (b - a) * t) * s->gain;
                }
                s->frac += step;
                size_t whole = static_cast<size_t>(s->frac);
                s->frac -= whole;
                s->read += whole * fb;
            }
            if ((s->data.size() - s->read) / fb < 2) {   // finished: drop the tail
                s->data.clear();
                s->read = 0;
                s->frac = 0;
            } else if (s->read > (1u << 20)) {
                s->data.erase(s->data.begin(), s->data.begin() + static_cast<long>(s->read));
                s->read = 0;
            }
        }
    }
    for (size_t i = 0; i < acc.size(); i++)
        out[i] = static_cast<int16_t>(std::clamp(acc[i], -1.0f, 1.0f) * 32767.0f);
}

void audio_thread(void*) {
    for (auto& b : g_bufs) {
        b.buffer = aligned_alloc(0x1000, kBufferFrames * 4);
        b.buffer_size = kBufferFrames * 4;
        b.data_size = kBufferFrames * 4;
        mix(static_cast<int16_t*>(b.buffer), kBufferFrames);
        armDCacheFlush(b.buffer, b.buffer_size);
        audoutAppendAudioOutBuffer(&b);
    }
    while (!g_audio_quit) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        // wake every 100 ms so SDL_Quit can stop this thread
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull)) || !released) continue;
        mix(static_cast<int16_t*>(released->buffer), kBufferFrames);
        armDCacheFlush(released->buffer, released->buffer_size);
        audoutAppendAudioOutBuffer(released);
    }
}
}  // namespace

static void audio_shutdown() {
    if (!g_audio_running) return;
    g_audio_quit = true;
    threadWaitForExit(&g_audio_thread);
    threadClose(&g_audio_thread);
    audoutStopAudioOut();
    audoutExit();
    for (auto& b : g_bufs) std::free(b.buffer);
    g_audio_running = false;
    std::fprintf(stderr, "[switch] audio stopped\n");
}

SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID, const SDL_AudioSpec*) {
    std::fprintf(stderr, "[switch] starting audout\n");
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) { std::fprintf(stderr, "[switch] audoutInitialize: 0x%x\n", rc); return fail("audout unavailable"), 0; }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) { std::fprintf(stderr, "[switch] audoutStartAudioOut: 0x%x\n", rc); return fail("audout unavailable"), 0; }
    rc = threadCreate(&g_audio_thread, audio_thread, nullptr, nullptr, 0x20000, 0x2B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_audio_thread);
    if (R_FAILED(rc)) { std::fprintf(stderr, "[switch] audio thread: 0x%x\n", rc); return fail("no audio thread"), 0; }
    g_audio_running = true;
    std::fprintf(stderr, "[switch] audio: %u Hz, %u channels\n", audoutGetSampleRate(), audoutGetChannelCount());
    return 1;
}

SDL_AudioStream* SDL_CreateAudioStream(const SDL_AudioSpec* src, const SDL_AudioSpec*) {
    auto* s = new SDL_AudioStream;
    if (src) s->spec = *src;
    return s;
}
bool SDL_BindAudioStream(SDL_AudioDeviceID, SDL_AudioStream* s) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!s->bound) g_streams.push_back(s);
    s->bound = true;
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream* s) {
    if (!s) return;
    {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_streams.erase(std::remove(g_streams.begin(), g_streams.end(), s), g_streams.end());
    }
    delete s;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    const Uint8* p = static_cast<const Uint8*>(buf);
    s->data.insert(s->data.end(), p, p + len);
    return true;
}
bool SDL_ClearAudioStream(SDL_AudioStream* s) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    s->data.clear();
    s->read = 0;
    s->frac = 0;
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    return static_cast<int>(s->data.size() - s->read);
}
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    s->gain = gain;
    return true;
}

// RIFF WAVE, PCM 8 or 16 bit (what Game Maker stores)
bool SDL_LoadWAV_IO(SDL_IOStream* io, bool closeio, SDL_AudioSpec* spec, Uint8** audio_buf, Uint32* audio_len) {
    const Uint8* d = io->p;
    size_t n = io->n;
    if (closeio) delete io;
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return fail("not a WAV file");
    bool have_fmt = false;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = u32(p + 4);
        size_t body = p + 8;
        if (std::memcmp(d + p, "fmt ", 4) == 0 && body + 16 <= n) {
            if (u16(body) != 1) return fail("WAV is not PCM");
            spec->channels = static_cast<int>(u16(body + 2));
            spec->freq = static_cast<int>(u32(body + 4));
            uint32_t bits = u16(body + 14);
            if (bits != 8 && bits != 16) return fail("unsupported WAV sample size");
            spec->format = bits == 16 ? SDL_AUDIO_S16 : SDL_AUDIO_U8;
            have_fmt = true;
        } else if (std::memcmp(d + p, "data", 4) == 0 && have_fmt) {
            uint32_t size = static_cast<uint32_t>(std::min<size_t>(len, n - body));
            *audio_buf = static_cast<Uint8*>(std::malloc(size + 1));
            std::memcpy(*audio_buf, d + body, size);
            *audio_len = size;
            return true;
        }
        p = body + len + (len & 1);
    }
    return fail("WAV has no data");
}

// ================================================================== events

namespace {
PadState g_pad;
bool g_pad_ready = false;
u64 g_prev_buttons = 0;
std::deque<SDL_Event> g_events;

// the controller as the original's keys: move, A/X confirm, talk and doors (X), B/Y jump (Z),
// + menu (Q), - rulebook (W)
struct Bind { u64 button; SDL_Scancode sc; };
const Bind kBinds[] = {
    {HidNpadButton_Left | HidNpadButton_StickLLeft, SDL_SCANCODE_LEFT},
    {HidNpadButton_Right | HidNpadButton_StickLRight, SDL_SCANCODE_RIGHT},
    {HidNpadButton_Up | HidNpadButton_StickLUp, SDL_SCANCODE_UP},
    {HidNpadButton_Down | HidNpadButton_StickLDown, SDL_SCANCODE_DOWN},
    {HidNpadButton_B | HidNpadButton_Y, SDL_SCANCODE_Z},
    {HidNpadButton_A | HidNpadButton_X, static_cast<SDL_Scancode>(SDL_SCANCODE_A + ('X' - 'A'))},
    {HidNpadButton_Plus, static_cast<SDL_Scancode>(SDL_SCANCODE_A + ('Q' - 'A'))},
    {HidNpadButton_Minus, static_cast<SDL_Scancode>(SDL_SCANCODE_A + ('W' - 'A'))},
};

void poll_pad() {
    if (!g_pad_ready) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        g_pad_ready = true;
    }
    padUpdate(&g_pad);
    u64 now = padGetButtons(&g_pad);
    if ((now & HidNpadButton_Plus) && (now & HidNpadButton_Minus)) {   // + and - together: back to hbmenu
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        g_events.push_back(e);
        return;
    }
    for (const Bind& b : kBinds) {
        bool was = (g_prev_buttons & b.button) != 0, is = (now & b.button) != 0;
        if (was == is) continue;
        SDL_Event e{};
        e.key.type = is ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = b.sc;
        e.key.down = is;
        e.key.repeat = false;
        g_events.push_back(e);
    }
    g_prev_buttons = now;
}
}  // namespace

bool SDL_PollEvent(SDL_Event* e) {
    if (g_events.empty()) {
        if (!appletMainLoop()) {
            e->type = SDL_EVENT_QUIT;
            return true;
        }
        poll_pad();
        if (g_events.empty()) {
            g_events.push_back(SDL_Event{});   // a marker: this poll is over
            g_events.back().type = 0;
        }
    }
    SDL_Event next = g_events.front();
    g_events.pop_front();
    if (next.type == 0) return false;
    *e = next;
    if (g_events.empty()) {   // end this poll after the last event
        g_events.push_back(SDL_Event{});
        g_events.back().type = 0;
    }
    return true;
}
