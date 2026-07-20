// DUSTFALL — platform layer: SDL window, indexed framebuffer present path,
// fixed 60Hz tick, logical input. E2: tick-exact movable square demo.
#include <SDL.h>

#include <cstdio>

#include "engine/framebuffer.h"
#include "engine/palette.h"
#include "platform/input.h"

namespace {

constexpr int kTickHz = 60;

struct Platform {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;  // 640x360 ARGB8888 streaming
    bool fullscreen = false;
    SDL_Rect present_dst{0, 0, fb::W, fb::H};  // last blit rect, for mouse mapping
    int present_scale = 1;
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

// ---- input mapping ----------------------------------------------------------

// Rebindable scancode -> logical key table. Multiple physical keys may map to
// the same logical key (arrows + WASD + numpad).
struct Binding { SDL_Scancode sc; Key key; };
constexpr Binding kBindings[] = {
    {SDL_SCANCODE_UP, Key::Up},       {SDL_SCANCODE_W, Key::Up},     {SDL_SCANCODE_KP_8, Key::Up},
    {SDL_SCANCODE_DOWN, Key::Down},   {SDL_SCANCODE_S, Key::Down},   {SDL_SCANCODE_KP_2, Key::Down},
    {SDL_SCANCODE_LEFT, Key::Left},   {SDL_SCANCODE_A, Key::Left},   {SDL_SCANCODE_KP_4, Key::Left},
    {SDL_SCANCODE_RIGHT, Key::Right}, {SDL_SCANCODE_D, Key::Right},  {SDL_SCANCODE_KP_6, Key::Right},
    {SDL_SCANCODE_KP_7, Key::UpLeft},   {SDL_SCANCODE_KP_9, Key::UpRight},
    {SDL_SCANCODE_KP_1, Key::DownLeft}, {SDL_SCANCODE_KP_3, Key::DownRight},
    {SDL_SCANCODE_KP_5, Key::Wait},     {SDL_SCANCODE_PERIOD, Key::Wait},
    {SDL_SCANCODE_RETURN, Key::Confirm}, {SDL_SCANCODE_KP_ENTER, Key::Confirm},
    {SDL_SCANCODE_ESCAPE, Key::Cancel},
    {SDL_SCANCODE_I, Key::Inventory},
    {SDL_SCANCODE_X, Key::Look},
    {SDL_SCANCODE_Z, Key::Powers},
    {SDL_SCANCODE_C, Key::Sandwalk},
    {SDL_SCANCODE_M, Key::Map},
};

void map_scancode(InputState& in, SDL_Scancode sc, bool is_down) {
    for (const Binding& b : kBindings) {
        if (b.sc != sc) continue;
        const int k = int(b.key);
        if (is_down && !in.down[k]) in.pressed[k] = true;
        if (!is_down && in.down[k]) in.released[k] = true;
        in.down[k] = is_down;
    }
}

void update_mouse(const Platform& p, InputState& in, int wx, int wy) {
    const int fx = (wx - p.present_dst.x) / p.present_scale;
    const int fy = (wy - p.present_dst.y) / p.present_scale;
    const bool inside = wx >= p.present_dst.x && wy >= p.present_dst.y &&
                        fx < fb::W && fy < fb::H;
    in.mouse_x = inside ? fx : -1;
    in.mouse_y = inside ? fy : -1;
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
    p.present_scale = SDL_max(1, SDL_min(ww / fb::W, wh / fb::H));
    p.present_dst = SDL_Rect{(ww - fb::W * p.present_scale) / 2,
                             (wh - fb::H * p.present_scale) / 2,
                             fb::W * p.present_scale, fb::H * p.present_scale};

    SDL_SetRenderDrawColor(p.renderer, 0, 0, 0, 255);
    SDL_RenderClear(p.renderer);
    SDL_RenderCopy(p.renderer, p.texture, nullptr, &p.present_dst);
    SDL_RenderPresent(p.renderer);
}

// ---- E2 demo: tick-exact movable square -------------------------------------

struct Demo {
    int sq_x = fb::W / 2, sq_y = fb::H / 2;
    bool demolog = false;
};

void demo_tick(Demo& d, const InputState& in) {
    const int step = 4;
    int dx = 0, dy = 0;
    if (in.pressed_or_repeat(Key::Up)) dy -= step;
    if (in.pressed_or_repeat(Key::Down)) dy += step;
    if (in.pressed_or_repeat(Key::Left)) dx -= step;
    if (in.pressed_or_repeat(Key::Right)) dx += step;
    d.sq_x = SDL_clamp(d.sq_x + dx, 8, fb::W - 8);
    d.sq_y = SDL_clamp(d.sq_y + dy, 8, fb::H - 8);
    if (in.mouse_pressed[0] && in.mouse_x >= 0) {
        d.sq_x = SDL_clamp(in.mouse_x, 8, fb::W - 8);
        d.sq_y = SDL_clamp(in.mouse_y, 8, fb::H - 8);
    }
}

void demo_draw(Framebuffer& frame, const Demo& d, const InputState& in) {
    // Dim sand backdrop with a subtle grid so motion reads.
    for (int y = 0; y < fb::H; ++y)
        for (int x = 0; x < fb::W; ++x)
            frame.at(x, y) = uint8_t((x % 32 == 0 || y % 32 == 0) ? 40 : 36);

    // The square: bright spice orange.
    for (int y = d.sq_y - 8; y < d.sq_y + 8; ++y)
        for (int x = d.sq_x - 8; x < d.sq_x + 8; ++x)
            frame.at(x, y) = 156;

    // Mouse crosshair, if inside the framebuffer.
    if (in.mouse_x >= 0) {
        for (int o = -3; o <= 3; ++o) {
            int cx = SDL_clamp(in.mouse_x + o, 0, fb::W - 1);
            int cy = SDL_clamp(in.mouse_y + o, 0, fb::H - 1);
            frame.at(cx, SDL_clamp(in.mouse_y, 0, fb::H - 1)) = 31;
            frame.at(SDL_clamp(in.mouse_x, 0, fb::W - 1), cy) = 31;
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
    InputState input;
    Demo demo;
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--demolog") == 0) demo.demolog = true;

    uint64_t prev = SDL_GetPerformanceCounter();
    const double tick_period = 1.0 / kTickHz;
    double accum = 0.0;
    uint64_t fps_frames = 0;
    uint64_t fps_last = prev;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (ev.key.repeat) break;  // repeat is tick-driven, not OS-driven
                    if (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                        toggle_fullscreen(platform);
                        break;
                    }
                    if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                    map_scancode(input, ev.key.keysym.scancode, true);
                    break;
                case SDL_KEYUP:
                    map_scancode(input, ev.key.keysym.scancode, false);
                    break;
                case SDL_MOUSEMOTION:
                    update_mouse(platform, input, ev.motion.x, ev.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    const int b = ev.button.button == SDL_BUTTON_LEFT    ? 0
                                  : ev.button.button == SDL_BUTTON_RIGHT ? 1
                                  : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
                    if (b >= 0) {
                        const bool down = ev.type == SDL_MOUSEBUTTONDOWN;
                        if (down && !input.mouse_down[b]) input.mouse_pressed[b] = true;
                        input.mouse_down[b] = down;
                        update_mouse(platform, input, ev.button.x, ev.button.y);
                    }
                    break;
                }
            }
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        accum += double(now - prev) / double(SDL_GetPerformanceFrequency());
        prev = now;
        if (accum > 5 * tick_period) accum = 5 * tick_period;  // clamp after stalls

        while (accum >= tick_period) {
            demo_tick(demo, input);
            input.end_tick();
            accum -= tick_period;
        }

        demo_draw(frame, demo, input);

        uint32_t lut[256];
        palette_build_lut(master, PaletteFade{}, 0, 0, 0, lut);
        present(platform, frame, lut);

        // FPS in the window title, once a second.
        ++fps_frames;
        if (now - fps_last >= SDL_GetPerformanceFrequency()) {
            char title[64];
            std::snprintf(title, sizeof title, "DUSTFALL — %llu fps",
                          static_cast<unsigned long long>(fps_frames));
            SDL_SetWindowTitle(platform.window, title);
            if (demo.demolog) {
                if (FILE* log = std::fopen("demolog.txt", "a")) {
                    std::fprintf(log, "sq=%d,%d mouse=%d,%d\n",
                                 demo.sq_x, demo.sq_y, input.mouse_x, input.mouse_y);
                    std::fclose(log);
                }
            }
            fps_frames = 0;
            fps_last = now;
        }
    }

    platform_shutdown(platform);
    return 0;
}
