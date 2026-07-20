// DUSTFALL — platform layer: SDL window, indexed framebuffer present path,
// fixed 60Hz tick, logical input. E3 demo: scrolling isometric desert with
// animated sprite, colormap night lighting, mouse tile picking, text.
#include <SDL.h>

#include <cstdio>

#include "engine/assets.h"
#include "engine/framebuffer.h"
#include "engine/iso.h"
#include "engine/palette.h"
#include "engine/render.h"
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
    {SDL_SCANCODE_N, Key::Sandwalk},  // demo reuse: N toggles night
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

// ---- E3 demo: iso desert ----------------------------------------------------

constexpr int kMapW = 64, kMapH = 64;

uint32_t hash2(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct Demo {
    uint8_t floor[kMapW * kMapH];  // tile frame index into "tiles"
    bool rock_prop[kMapW * kMapH];
    iso::Camera cam;
    bool night = false;
    int tick = 0;
    bool shot = false;  // --shot: exit after writing framebuffer state proof

    void generate() {
        for (int y = 0; y < kMapH; ++y) {
            for (int x = 0; x < kMapW; ++x) {
                const uint32_t coarse = hash2(x >> 3, y >> 3) & 255;
                const uint32_t fine = hash2(x, y) & 255;
                const int v = int(coarse * 3 + fine) / 4;
                uint8_t f;
                bool prop = false;
                if (v > 190) {           // rock islands
                    f = uint8_t(3 + (fine & 1));
                    prop = fine > 236;
                } else if (v < 40) {     // spice-flecked pans
                    f = 5;
                } else if (fine > 251) { // rare drum sand
                    f = 6;
                } else {
                    f = uint8_t(fine % 3);  // sand variants
                }
                floor[y * kMapW + x] = f;
                rock_prop[y * kMapW + x] = prop;
            }
        }
        int cx, cy;
        iso::tile_to_screen(kMapW / 2, kMapH / 2, iso::Camera{}, cx, cy);
        cam.x = cx - fb::W / 2;
        cam.y = cy - fb::H / 2;
    }
};

void demo_tick(Demo& d, const InputState& in) {
    ++d.tick;
    const int speed = 4;
    if (in.is_down(Key::Up)) d.cam.y -= speed;
    if (in.is_down(Key::Down)) d.cam.y += speed;
    if (in.is_down(Key::Left)) d.cam.x -= speed;
    if (in.is_down(Key::Right)) d.cam.x += speed;
    if (in.was_pressed(Key::Sandwalk)) d.night = !d.night;
}

void demo_draw(Framebuffer& frame, const Demo& d, const Assets& assets,
               const Colormaps& cmaps, const InputState& in, iso::DrawList& list) {
    DrawCtx ctx{&frame, &cmaps};
    frame.clear(1);  // near-black backdrop

    const SpriteSheet* tiles = assets.get("tiles");
    const SpriteSheet* player = assets.get("player");
    const SpriteSheet* rock = assets.get("rock");
    const int light = d.night ? 13 : 31;

    // Floors, painter's order (no overlap between floor diamonds, so direct).
    for (int ty = 0; ty < kMapH; ++ty) {
        for (int tx = 0; tx < kMapW; ++tx) {
            int sx, sy;
            iso::tile_to_screen(tx, ty, d.cam, sx, sy);
            if (sx < -32 || sx > fb::W + 32 || sy < -32 || sy > fb::H + 32) continue;
            draw_sprite(ctx, *tiles, d.floor[ty * kMapW + tx], sx, sy, light);
        }
    }

    // Mouse tile highlight, over floors and under sprites.
    int mtx = -1, mty = -1;
    if (in.mouse_x >= 0) {
        iso::screen_to_tile(in.mouse_x, in.mouse_y, d.cam, mtx, mty);
        if (mtx >= 0 && mtx < kMapW && mty >= 0 && mty < kMapH) {
            int sx, sy;
            iso::tile_to_screen(mtx, mty, d.cam, sx, sy);
            draw_sprite(ctx, *tiles, 7, sx, sy, 31);
        }
    }

    // Props + the animated wanderer, y-sorted together.
    for (int ty = 0; ty < kMapH; ++ty) {
        for (int tx = 0; tx < kMapW; ++tx) {
            if (!d.rock_prop[ty * kMapW + tx]) continue;
            int sx, sy;
            iso::tile_to_screen(tx, ty, d.cam, sx, sy);
            if (sx < -48 || sx > fb::W + 48 || sy < -48 || sy > fb::H + 48) continue;
            list.add(rock, 0, sx, sy + 6, light, 0);
        }
    }
    {
        int sx, sy;
        iso::tile_to_screen(kMapW / 2, kMapH / 2, d.cam, sx, sy);
        list.add(player, (d.tick / 9) % 4, sx, sy + 6, light, 1);
    }
    list.flush(ctx);

    // Text overlay.
    draw_text_shadow(ctx, "DUSTFALL iso demo", 4, 4, 230, 1);
    draw_text_shadow(ctx, "arrows scroll  N night  mouse picks tile", 4, 14, 24, 1);
    if (mtx >= 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "tile %d,%d", mtx, mty);
        draw_text_shadow(ctx, buf, 4, 24, 24, 1);
    }
    if (d.night) draw_text_shadow(ctx, "NIGHT", fb::W - 48, 4, 170, 1);
}

}  // namespace

int main(int argc, char** argv) {
    Platform platform;
    if (!platform_init(platform)) {
        platform_shutdown(platform);
        return 1;
    }

    Palette master;
    if (!palette_load("data/palette.pal", master)) {
        std::fprintf(stderr, "palette: data/palette.pal missing, using built-in\n");
        master = palette_make_default();
    }
    Colormaps cmaps;
    colormaps_build(master, cmaps);

    Assets assets;
    if (!assets.load_all("data", master)) {
        platform_shutdown(platform);
        return 1;
    }

    Framebuffer frame{};
    InputState input;
    iso::DrawList draw_list;
    Demo demo;
    demo.generate();
    for (int i = 1; i < argc; ++i)
        if (SDL_strcmp(argv[i], "--night") == 0) demo.night = true;

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
                    if (ev.key.repeat) break;
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
        if (accum > 5 * tick_period) accum = 5 * tick_period;

        while (accum >= tick_period) {
            demo_tick(demo, input);
            input.end_tick();
            accum -= tick_period;
        }

        demo_draw(frame, demo, assets, cmaps, input, draw_list);

        uint32_t lut[256];
        palette_build_lut(master, PaletteFade{}, 0, 0, 0, lut);
        present(platform, frame, lut);

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
