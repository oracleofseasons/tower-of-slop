// The part of SDL3 the runtime uses, implemented for the Switch 2 (libounce) in ../../sdl_switch.cpp:
// EGL/OpenGL for the renderer, audout for sound, the controller for keys. Names, types and
// behaviour follow SDL3 so the runtime builds unchanged; anything else in SDL3 is absent.
#pragma once
#include <cstddef>
#include <cstdint>

using Uint8 = uint8_t;
using Uint32 = uint32_t;
using Uint64 = uint64_t;
using Sint16 = int16_t;

// ------------------------------------------------------------------ basics
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_HINT_VIDEO_DRIVER "SDL_VIDEO_DRIVER"
bool SDL_Init(Uint32 flags);
void SDL_Quit();
const char* SDL_GetError();
bool SDL_SetHint(const char* name, const char* value);
void SDL_free(void* p);
Uint64 SDL_GetTicksNS();
void SDL_DelayNS(Uint64 ns);

// ------------------------------------------------------------------ files
struct SDL_IOStream;
SDL_IOStream* SDL_IOFromConstMem(const void* mem, size_t size);
enum SDL_PathType { SDL_PATHTYPE_NONE, SDL_PATHTYPE_FILE, SDL_PATHTYPE_DIRECTORY, SDL_PATHTYPE_OTHER };
struct SDL_PathInfo {
    SDL_PathType type;
    Uint64 size;
};
bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info);
bool SDL_RenamePath(const char* oldpath, const char* newpath);
const char* SDL_GetBasePath();
char* SDL_GetPrefPath(const char* org, const char* app);

#define SDL_MESSAGEBOX_ERROR 0x10u
struct SDL_Window;
bool SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window);

// ------------------------------------------------------------------ video
#define SDL_WINDOW_RESIZABLE 0x20u
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Rect { int x, y, w, h; };
struct SDL_FRect { float x, y, w, h; };
struct SDL_FPoint { float x, y; };
struct SDL_FColor { float r, g, b, a; };
struct SDL_Vertex {
    SDL_FPoint position;
    SDL_FColor color;
    SDL_FPoint tex_coord;
};
enum SDL_PixelFormat { SDL_PIXELFORMAT_ABGR8888 = 0x16762004 };   // bytes R, G, B, A
enum SDL_TextureAccess { SDL_TEXTUREACCESS_STATIC, SDL_TEXTUREACCESS_STREAMING, SDL_TEXTUREACCESS_TARGET };
using SDL_BlendMode = Uint32;
#define SDL_BLENDMODE_NONE 0x0u
#define SDL_BLENDMODE_BLEND 0x1u
enum SDL_ScaleMode { SDL_SCALEMODE_NEAREST, SDL_SCALEMODE_LINEAR };
enum SDL_FlipMode { SDL_FLIP_NONE = 0, SDL_FLIP_HORIZONTAL = 1, SDL_FLIP_VERTICAL = 2 };
struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat format;
    int w, h, pitch;
    void* pixels;
};

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, Uint64 flags);
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char* name);
bool SDL_SetRenderVSync(SDL_Renderer* r, int vsync);
SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h);
bool SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch);
void SDL_DestroyTexture(SDL_Texture* t);
bool SDL_GetTextureSize(SDL_Texture* t, float* w, float* h);
bool SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode);
bool SDL_SetTextureScaleMode(SDL_Texture* t, SDL_ScaleMode mode);
bool SDL_SetTextureColorMod(SDL_Texture* t, Uint8 r, Uint8 g, Uint8 b);
bool SDL_SetTextureAlphaMod(SDL_Texture* t, Uint8 a);
bool SDL_SetRenderTarget(SDL_Renderer* r, SDL_Texture* t);
bool SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 red, Uint8 g, Uint8 b, Uint8 a);
bool SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode);
bool SDL_RenderClear(SDL_Renderer* r);
bool SDL_RenderFillRect(SDL_Renderer* r, const SDL_FRect* rect);
bool SDL_RenderTexture(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* src, const SDL_FRect* dst);
bool SDL_RenderTextureRotated(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* src, const SDL_FRect* dst,
                              double angle, const SDL_FPoint* center, SDL_FlipMode flip);
bool SDL_RenderGeometry(SDL_Renderer* r, SDL_Texture* t, const SDL_Vertex* vertices, int num_vertices,
                        const int* indices, int num_indices);
bool SDL_GetCurrentRenderOutputSize(SDL_Renderer* r, int* w, int* h);
SDL_Surface* SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect);
bool SDL_RenderPresent(SDL_Renderer* r);
SDL_Surface* SDL_ConvertSurface(SDL_Surface* s, SDL_PixelFormat format);
void SDL_DestroySurface(SDL_Surface* s);
bool SDL_SaveBMP(SDL_Surface* s, const char* file);

// ------------------------------------------------------------------ audio
enum SDL_AudioFormat { SDL_AUDIO_U8 = 0x0008, SDL_AUDIO_S16 = 0x8010 };
struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
};
using SDL_AudioDeviceID = Uint32;
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu
struct SDL_AudioStream;
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec);
SDL_AudioStream* SDL_CreateAudioStream(const SDL_AudioSpec* src, const SDL_AudioSpec* dst);
bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream* s);
void SDL_DestroyAudioStream(SDL_AudioStream* s);
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len);
bool SDL_ClearAudioStream(SDL_AudioStream* s);
int SDL_GetAudioStreamQueued(SDL_AudioStream* s);
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain);
bool SDL_LoadWAV_IO(SDL_IOStream* src, bool closeio, SDL_AudioSpec* spec, Uint8** audio_buf, Uint32* audio_len);

// ------------------------------------------------------------------ events (the controller acts as a keyboard)
enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_9 = 38, SDL_SCANCODE_0 = 39,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F12 = 69,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT = 229, SDL_SCANCODE_RALT = 230,
};
enum SDL_EventType : Uint32 { SDL_EVENT_QUIT = 0x100, SDL_EVENT_KEY_DOWN = 0x300, SDL_EVENT_KEY_UP = 0x301 };
struct SDL_KeyboardEvent {
    SDL_EventType type;
    SDL_Scancode scancode;
    bool down;
    bool repeat;
};
union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
};
bool SDL_PollEvent(SDL_Event* e);
