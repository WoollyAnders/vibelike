// DUSTFALL — platform layer: SDL window, indexed framebuffer present path,
// fixed 60Hz tick, logical input, audio device. Drives the game through the
// seam in game/game.h.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/framebuffer.h"
#include "engine/palette.h"
#include "engine/wav.h"
#include "game/game.h"
#include "platform/input.h"

namespace {

constexpr int kTickHz = 60;

// ---- audio data (placeholder music until the M5 music pass) ------------------

const audio::FmInstrument kInstruments[] = {
    {1.0f, 2.0f, 1.6f, 0.3f, {0.9f, 0.6f, 0.7f, 1.4f}, {1.2f, 0.8f, 0.5f, 1.0f}, 0.30f},
    {1.0f, 3.01f, 4.0f, 0.2f, {0.004f, 0.35f, 0.0f, 0.35f}, {0.004f, 0.2f, 0.2f, 0.2f}, 0.38f},
    {0.5f, 1.0f, 2.2f, 0.4f, {1.5f, 1.0f, 0.8f, 2.0f}, {2.0f, 1.5f, 0.6f, 1.5f}, 0.34f},
};

const audio::SongEvent kAmbient[] = {
    {0, 0, 38, 2, 100},
    {0, 1, 57, 0, 70},
    {8, 2, 62, 1, 90},
    {14, 2, 65, 1, 80},
    {16, 1, 0, 0, 0},
    {18, 1, 60, 0, 70},
    {22, 2, 69, 1, 85},
    {30, 2, 72, 1, 75},
    {32, 1, 0, 0, 0},
    {34, 1, 57, 0, 70},
    {44, 2, 69, 1, 70},
    {52, 2, 65, 1, 65},
    {0xFFFF, 0, 0, 0, 0},
};

int g_song_ambient = -1;
int g_sfx_roar = -1;
int g_sfx_blip = -1;

bool audio_load(audio::Mixer& mixer) {
    for (const audio::FmInstrument& in : kInstruments) mixer.add_instrument(in);
    g_song_ambient = mixer.add_song(audio::Song{kAmbient, 4.0f, 64, 0});

    Sample roar, blip;
    if (!wav_load("data/sfx/worm_roar.wav", roar) || !wav_load("data/sfx/blip.wav", blip)) {
        std::fprintf(stderr, "audio: failed to load sfx\n");
        return false;
    }
    g_sfx_roar = mixer.add_sample(std::move(roar));   // id 0
    g_sfx_blip = mixer.add_sample(std::move(blip));   // id 1
    return true;
}

void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    static_cast<audio::Mixer*>(userdata)->render(reinterpret_cast<int16_t*>(stream), len / 4);
}

// ---- platform ----------------------------------------------------------------

struct Platform {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioDeviceID audio_dev = 0;
    bool fullscreen = false;
    SDL_Rect present_dst{0, 0, fb::W, fb::H};
    int present_scale = 1;
};

bool platform_init(Platform& p, audio::Mixer* mixer) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    p.window = SDL_CreateWindow("DUSTFALL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                fb::W * 2, fb::H * 2, SDL_WINDOW_RESIZABLE);
    if (!p.window) return false;
    p.renderer = SDL_CreateRenderer(p.window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!p.renderer) return false;
    p.texture = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, fb::W, fb::H);
    if (!p.texture) return false;

    if (mixer) {
        SDL_AudioSpec want{}, have{};
        want.freq = audio::kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = sdl_audio_callback;
        want.userdata = mixer;
        p.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!p.audio_dev)
            std::fprintf(stderr, "audio device unavailable: %s\n", SDL_GetError());
        else
            SDL_PauseAudioDevice(p.audio_dev, 0);
    }
    return true;
}

void platform_shutdown(Platform& p) {
    if (p.audio_dev) SDL_CloseAudioDevice(p.audio_dev);
    if (p.texture) SDL_DestroyTexture(p.texture);
    if (p.renderer) SDL_DestroyRenderer(p.renderer);
    if (p.window) SDL_DestroyWindow(p.window);
    SDL_Quit();
}

void toggle_fullscreen(Platform& p) {
    p.fullscreen = !p.fullscreen;
    SDL_SetWindowFullscreen(p.window, p.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

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
    {SDL_SCANCODE_G, Key::Harvest},
    {SDL_SCANCODE_T, Key::Thumper},
    {SDL_SCANCODE_F5, Key::Save},
    {SDL_SCANCODE_F9, Key::Load},
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

// ---- offline verification modes ----------------------------------------------

int run_dumpwav(audio::Mixer& mixer) {
    constexpr int kSeconds = 8;
    std::vector<int16_t> out(size_t(audio::kRate) * kSeconds * 2);
    mixer.push({audio::Cmd::PlaySong, uint8_t(g_song_ambient), 0, 0});
    for (int s = 0; s < kSeconds; ++s) {
        if (s == 2) mixer.push({audio::Cmd::PlaySfx, uint8_t(g_sfx_roar), 255, 128});
        if (s == 4) mixer.push({audio::Cmd::Rumble, 0, 220, 0});
        if (s == 6) mixer.push({audio::Cmd::Rumble, 0, 0, 0});
        mixer.render(out.data() + size_t(s) * audio::kRate * 2, audio::kRate);
        double acc = 0;
        for (int i = 0; i < audio::kRate; ++i) {
            const double v = out[size_t(s) * audio::kRate * 2 + i * 2] / 32768.0;
            acc += v * v;
        }
        std::printf("second %d rms %.4f\n", s, std::sqrt(acc / audio::kRate));
    }
    const bool ok = wav_save("data/dump.wav", out.data(), audio::kRate * kSeconds, audio::kRate);
    std::printf("dump.wav %s\n", ok ? "written" : "FAILED");
    return ok ? 0 : 1;
}

// Headless seam test: deterministic ticks, save mid-run, diverge, load,
// verify the state hash returns to the saved point.
int run_seamtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(12345, services);

    InputState in;
    auto tick_with = [&](Key k, int n) {
        for (int i = 0; i < n; ++i) {
            if (k != Key::Count) in.pressed[int(k)] = true;
            game_update(in);
            in.end_tick();
        }
    };

    tick_with(Key::Right, 3);
    tick_with(Key::Down, 2);
    tick_with(Key::Save, 1);
    const uint64_t h_saved = game_state_hash();

    tick_with(Key::Left, 4);  // diverge (backtracks over known-walkable tiles)
    const uint64_t h_diverged = game_state_hash();

    tick_with(Key::Load, 1);
    const uint64_t h_loaded = game_state_hash();

    // Re-init from the same seed and replay the pre-save inputs: determinism.
    game_init(12345, services);
    tick_with(Key::Right, 3);
    tick_with(Key::Down, 2);
    const uint64_t h_replayed = game_state_hash();

    std::printf("saved    %016llx\ndiverged %016llx\nloaded   %016llx\nreplayed %016llx\n",
                (unsigned long long)h_saved, (unsigned long long)h_diverged,
                (unsigned long long)h_loaded, (unsigned long long)h_replayed);
    const bool pass = h_saved == h_loaded && h_saved == h_replayed && h_saved != h_diverged;
    std::printf("seamtest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// Headless survival test: leave the sietch, then stand in the open desert
// until thirst and sun finish the job. Proves upkeep/heat/death end to end.
int run_slicetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    InputState in;
    auto tick_with = [&](Key k) {
        in.pressed[int(k)] = true;
        game_update(in);
        in.end_tick();
    };

    for (int i = 0; i < 12; ++i) tick_with(Key::Down);  // corridor -> door -> desert
    int waits = 0;
    while (!game_is_dead() && waits < 6000) {
        tick_with(Key::Wait);
        ++waits;
    }
    std::printf("dead after %d waits\n", waits);
    const bool pass = game_is_dead() && waits > 50;
    std::printf("slicetest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// Writes the indexed framebuffer through a LUT as a 24-bit BMP (debug/demo).
bool bmp_save(const char* path, const Framebuffer& frame, const uint32_t lut[256]) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int w = fb::W, h = fb::H;
    const uint32_t row = uint32_t(w * 3 + 3) & ~3u;
    const uint32_t data = row * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) {
        hdr[o] = v & 255; hdr[o + 1] = (v >> 8) & 255;
        hdr[o + 2] = (v >> 16) & 255; hdr[o + 3] = v >> 24;
    };
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, data);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t c = lut[frame.px[y * w + x]];
            line[x * 3] = c & 255;
            line[x * 3 + 1] = (c >> 8) & 255;
            line[x * 3 + 2] = (c >> 16) & 255;
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    return true;
}

// Headless worm test: leave the sietch, walk into the sand, plant a thumper,
// fall back, and watch the state machine escalate to a surfaced worm.
int run_wormtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);

    InputState in;
    auto tick_with = [&](Key k) {
        in.pressed[int(k)] = true;
        game_update(in);
        in.end_tick();
    };

    for (int i = 0; i < 12; ++i) tick_with(Key::Down);   // out of the sietch
    for (int i = 0; i < 8; ++i) tick_with(Key::Right);   // onto open sand
    tick_with(Key::Thumper);
    for (int i = 0; i < 7; ++i) tick_with(Key::Left);    // fall back

    int max_state = 0;
    int turns = 0;
    while (turns < 120 && !game_is_dead()) {
        tick_with(Key::Wait);
        if (game_worm_state() > max_state) {
            max_state = game_worm_state();
            std::printf("turn %3d worm state -> %d\n", turns, max_state);
        }
        if (max_state >= 4) break;
        ++turns;
    }
    const bool pass = max_state >= 4 && !game_is_dead();
    std::printf("wormtest %s (max state %d, dead %d)\n",
                pass ? "PASS" : "FAIL", max_state, game_is_dead() ? 1 : 0);
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    audio::Mixer mixer;
    if (!audio_load(mixer)) return 1;

    bool seamtest = false, slicetest = false, wormtest = false, autodemo = false;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--dumpwav") == 0) return run_dumpwav(mixer);
        if (SDL_strcmp(argv[i], "--seamtest") == 0) seamtest = true;
        if (SDL_strcmp(argv[i], "--slicetest") == 0) slicetest = true;
        if (SDL_strcmp(argv[i], "--wormtest") == 0) wormtest = true;
        if (SDL_strcmp(argv[i], "--autodemo") == 0) autodemo = true;
    }

    Palette master;
    if (!palette_load("data/palette.pal", master)) {
        std::fprintf(stderr, "palette: data/palette.pal missing, using built-in\n");
        master = palette_make_default();
    }
    Colormaps cmaps;
    colormaps_build(master, cmaps);

    Assets assets;
    if (!assets.load_all("data", master)) return 1;

    if (seamtest) return run_seamtest(mixer, assets, cmaps);
    if (slicetest) return run_slicetest(mixer, assets, cmaps);
    if (wormtest) return run_wormtest(mixer, assets, cmaps);

    Platform platform;
    if (!platform_init(platform, &mixer)) {
        platform_shutdown(platform);
        return 1;
    }

    GameServices services{&mixer, &assets, &cmaps};
    game_init(autodemo ? 4242 : uint64_t(SDL_GetPerformanceCounter()), services);
    mixer.push({audio::Cmd::PlaySong, uint8_t(g_song_ambient), 0, 0});

    // --autodemo: the worm-bait scenario played by the game itself, one input
    // every few ticks. Purely for demos/screenshots; no OS input needed.
    struct DemoStep { Key key; int count; };
    static const DemoStep kDemoScript[] = {
        {Key::Down, 13}, {Key::Right, 8}, {Key::Thumper, 1}, {Key::Left, 7},
        {Key::Wait, 60},
    };
    int demo_step = 0, demo_left = autodemo ? kDemoScript[0].count : 0, demo_cooldown = 0;

    Framebuffer frame{};
    InputState input;

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
            if (autodemo && demo_step < int(SDL_arraysize(kDemoScript))) {
                if (--demo_cooldown <= 0) {
                    input.pressed[int(kDemoScript[demo_step].key)] = true;
                    demo_cooldown = 6;
                    if (--demo_left <= 0 && ++demo_step < int(SDL_arraysize(kDemoScript)))
                        demo_left = kDemoScript[demo_step].count;
                }
            }
            if (!game_update(input)) running = false;
            input.end_tick();
            accum -= tick_period;
        }

        game_render(frame);

        uint32_t lut[256];
        palette_build_lut(master, game_fade(), 0, 0, 0, lut);
        present(platform, frame, lut);

        if (autodemo) {
            static int demo_ticks = 0, shot = 0;
            if (++demo_ticks % 24 == 0 && shot < 24) {
                char name[32];
                std::snprintf(name, sizeof name, "demo_%02d.bmp", shot++);
                bmp_save(name, frame, lut);
            }
        }

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
