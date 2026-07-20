// DUSTFALL — platform layer: SDL window, indexed framebuffer present path,
// fixed 60Hz tick. E1: palette-cycling gradient + fade demo.
#include <SDL.h>

#include <cstdio>

#include "engine/framebuffer.h"
#include "engine/palette.h"

namespace {

constexpr int kTickHz = 60;

struct Platform {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;  // 640x360 ARGB8888 streaming
    bool fullscreen = false;
};

bool platform_init(Platform& p) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    p.window = SDL_CreateWindow("DUSTFALL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                fb::W * 2, fb::H * 2, SDL_WINDOW_RESIZABLE);
    if (!p.window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    p.renderer = SDL_CreateRenderer(p.window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!p.renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    p.texture = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, fb::W, fb::H);
    if (!p.texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void platform_shutdown(Platform& p) {
    if (p.texture) SDL_DestroyTexture(p.texture);
    if (p.renderer) SDL_DestroyRenderer(p.renderer);
    if (p.window) SDL_DestroyWindow(p.window);
    SDL_Quit();
}

void toggle_fullscreen(Platform& p) {
    p.fullscreen = !p.fullscreen;
    SDL_SetWindowFullscreen(p.window, p.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

// Expand indexed pixels through the LUT and present with integer scaling.
void present(Platform& p, const Framebuffer& frame, const uint32_t lut[256]) {
    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(p.texture, nullptr, &pixels, &pitch) == 0) {
        for (int y = 0; y < fb::H; ++y) {
            const uint8_t* src = frame.px + y * fb::W;
            uint32_t* dst = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(pixels) + y * pitch);
            for (int x = 0; x < fb::W; ++x) dst[x] = lut[src[x]];
        }
        SDL_UnlockTexture(p.texture);
    }

    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(p.renderer, &ww, &wh);
    int scale = SDL_max(1, SDL_min(ww / fb::W, wh / fb::H));
    SDL_Rect dst{(ww - fb::W * scale) / 2, (wh - fb::H * scale) / 2,
                 fb::W * scale, fb::H * scale};

    SDL_SetRenderDrawColor(p.renderer, 0, 0, 0, 255);
    SDL_RenderClear(p.renderer);
    SDL_RenderCopy(p.renderer, p.texture, nullptr, &dst);
    SDL_RenderPresent(p.renderer);
}

// ---- E1 demo state ----------------------------------------------------------

struct Demo {
    int cycle = 0;          // palette rotation offset for the sand band
    int fade_dir = 0;       // -1 fading in, +1 fading out, 0 idle
    int fade_amount = 0;    // 0..256
    bool autofade = false;  // --autofade: ping-pong continuously (dev/testing)
};

void demo_tick(Demo& d) {
    d.cycle = (d.cycle + 1) % 64;
    if (d.autofade && d.fade_dir == 0) d.fade_dir = +1;
    if (d.fade_dir != 0) {
        d.fade_amount += d.fade_dir * 6;
        if (d.fade_amount >= 256) { d.fade_amount = 256; d.fade_dir = -1; }
        if (d.fade_amount <= 0) { d.fade_amount = 0; d.fade_dir = 0; }
    }
}

void demo_draw(Framebuffer& frame) {
    // Diagonal gradient across the sand ramp (indices 32..95), with bands of
    // rock/spice/sky ramps so every region of the palette is visible.
    for (int y = 0; y < fb::H; ++y) {
        for (int x = 0; x < fb::W; ++x) {
            int band = y / 90;  // 4 horizontal bands
            int t = (x + y) % 64;
            uint8_t idx = 0;
            switch (band) {
                case 0: idx = uint8_t(32 + t); break;                 // sand (cycled)
                case 1: idx = uint8_t(96 + (t >> 1)); break;          // rock
                case 2: idx = uint8_t(128 + (t >> 1)); break;         // spice
                default: idx = uint8_t(160 + (t >> 1)); break;        // night sky
            }
            frame.at(x, y) = idx;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Platform platform;
    if (!platform_init(platform)) {
        platform_shutdown(platform);
        return 1;
    }

    const Palette master = palette_make_default();
    Framebuffer frame{};
    Demo demo;
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--autofade") == 0) demo.autofade = true;

    uint64_t prev = SDL_GetPerformanceCounter();
    const double tick_period = 1.0 / kTickHz;
    double accum = 0.0;
    uint64_t fps_frames = 0;
    uint64_t fps_last = prev;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                const SDL_Keycode key = ev.key.keysym.sym;
                const bool alt = (ev.key.keysym.mod & KMOD_ALT) != 0;
                if (key == SDLK_ESCAPE) running = false;
                if (key == SDLK_RETURN && alt) toggle_fullscreen(platform);
                if (key == SDLK_SPACE && demo.fade_dir == 0 && demo.fade_amount == 0)
                    demo.fade_dir = +1;  // fade out, then back in
            }
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        accum += double(now - prev) / double(SDL_GetPerformanceFrequency());
        prev = now;
        if (accum > 5 * tick_period) accum = 5 * tick_period;  // clamp after stalls

        while (accum >= tick_period) {
            demo_tick(demo);
            accum -= tick_period;
        }

        demo_draw(frame);

        PaletteFade fade{0, 0, 0, uint16_t(demo.fade_amount)};
        uint32_t lut[256];
        palette_build_lut(master, fade, demo.cycle, 32, 95, lut);
        present(platform, frame, lut);

        // FPS in the window title, once a second.
        ++fps_frames;
        if (now - fps_last >= SDL_GetPerformanceFrequency()) {
            char title[64];
            std::snprintf(title, sizeof title, "DUSTFALL — %llu fps",
                          static_cast<unsigned long long>(fps_frames));
            SDL_SetWindowTitle(platform.window, title);
            fps_frames = 0;
            fps_last = now;
        }
    }

    platform_shutdown(platform);
    return 0;
}
