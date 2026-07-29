// DUSTFALL — platform layer: SDL window, indexed framebuffer present path,
// fixed 60Hz tick, logical input, audio device. Drives the game through the
// seam in game/game.h.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/animator.h"
#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/framebuffer.h"
#include "engine/palette.h"
#include "engine/wav.h"
#include "game/game.h"
#include "game/items.h"
#include "game/tunables.h"
#include "platform/input.h"

// RT1.2: the world is real-time now — it advances on a fixed sim-clock timer, not once per
// player action. So a headless test "turn" is kWorldStepTicks fixed frames (== exactly one world
// tick, regardless of phase); a player grid-action fires at most once per kPlayerActionTicks. The
// harness helpers below step whole frame-blocks accordingly. Combat loops instead step single
// frames with a large frame budget (an attack lands every kPlayerActionTicks frames).
namespace { constexpr int kWorldStep = tune::kWorldStepTicks; }

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

// The sietch: warm, patient, home. Low drone, soft pad swells, kind plucks.
const audio::SongEvent kSietch[] = {
    {0, 0, 43, 2, 90},
    {0, 1, 60, 0, 60},
    {12, 2, 64, 1, 70},
    {16, 1, 0, 0, 0},
    {18, 1, 62, 0, 60},
    {24, 2, 67, 1, 65},
    {32, 1, 0, 0, 0},
    {34, 1, 60, 0, 60},
    {40, 2, 72, 1, 60},
    {52, 2, 67, 1, 55},
    {0xFFFF, 0, 0, 0, 0},
};

// Danger: wormsign, alarms, storms. Low pressure drone + minor-second ostinato.
const audio::SongEvent kDanger[] = {
    {0, 0, 36, 2, 110},
    {0, 1, 48, 0, 70},
    {4, 2, 62, 1, 85},
    {8, 2, 63, 1, 80},
    {12, 2, 62, 1, 75},
    {16, 1, 0, 0, 0},
    {17, 1, 47, 0, 70},
    {20, 2, 66, 1, 85},
    {24, 2, 63, 1, 80},
    {28, 2, 62, 1, 75},
    {0xFFFF, 0, 0, 0, 0},
};

int g_song_ambient = -1;
int g_sfx_roar = -1;
int g_sfx_blip = -1;

bool audio_load(audio::Mixer& mixer) {
    for (const audio::FmInstrument& in : kInstruments) mixer.add_instrument(in);
    g_song_ambient = mixer.add_song(audio::Song{kAmbient, 4.0f, 64, 0});   // id 0
    mixer.add_song(audio::Song{kSietch, 4.0f, 64, 0});                     // id 1
    mixer.add_song(audio::Song{kDanger, 8.0f, 32, 0});                     // id 2

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
    // Linear filtering so the aspect-fit down/upscale of the 1080p frame to the
    // window is smooth rather than shimmery on non-integer ratios.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    // R2: the framebuffer is already 1080p, so open the window at a size that fits
    // the monitor (was fb*2 = 3840x2160, which overflowed every screen). Pick the
    // largest 16:9 that fits ~90% of the usable desktop, clamped to the native size.
    int win_w = fb::W, win_h = fb::H;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        const int max_w = usable.w * 9 / 10, max_h = usable.h * 9 / 10;
        if (win_w > max_w) { win_w = max_w; win_h = win_w * fb::H / fb::W; }
        if (win_h > max_h) { win_h = max_h; win_w = win_h * fb::W / fb::H; }
    }
    // Start MAXIMIZED so it fills the work area (minus the taskbar) without
    // overflowing; win_w/win_h become the restored (un-maximized) size. Aspect-fit
    // present letterboxes as needed. F toggles true fullscreen.
    p.window = SDL_CreateWindow("DUSTFALL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
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
    // Arrows + numpad keep the tile-cardinal directions (also used by tests).
    {SDL_SCANCODE_UP, Key::Up},       {SDL_SCANCODE_KP_8, Key::Up},
    {SDL_SCANCODE_DOWN, Key::Down},   {SDL_SCANCODE_KP_2, Key::Down},
    {SDL_SCANCODE_LEFT, Key::Left},   {SDL_SCANCODE_KP_4, Key::Left},
    {SDL_SCANCODE_RIGHT, Key::Right}, {SDL_SCANCODE_KP_6, Key::Right},
    // WASD is PoE2 screen-relative (W = up on screen), composed to iso moves in-game.
    {SDL_SCANCODE_W, Key::WalkUp},    {SDL_SCANCODE_S, Key::WalkDown},
    {SDL_SCANCODE_A, Key::WalkLeft},  {SDL_SCANCODE_D, Key::WalkRight},
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
    {SDL_SCANCODE_B, Key::Plant},
    {SDL_SCANCODE_V, Key::Poison},
    {SDL_SCANCODE_L, Key::Glowglobe},
    {SDL_SCANCODE_R, Key::Ride},
    {SDL_SCANCODE_P, Key::Character},
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
    // Map window pixel -> framebuffer pixel through the (possibly non-integer)
    // aspect-fit present rect.
    const SDL_Rect& d = p.present_dst;
    const int fx = d.w > 0 ? (wx - d.x) * fb::W / d.w : -1;
    const int fy = d.h > 0 ? (wy - d.y) * fb::H / d.h : -1;
    const bool inside = wx >= d.x && wy >= d.y && fx >= 0 && fy >= 0 &&
                        fx < fb::W && fy < fb::H;
    in.mouse_x = inside ? fx : -1;
    in.mouse_y = inside ? fy : -1;
}

void present(Platform& p, const Framebuffer& frame, PaletteFade fade) {
    void* pixels = nullptr;
    int pitch = 0;
    const int a = fade.amount > 256 ? 256 : int(fade.amount);
    if (SDL_LockTexture(p.texture, nullptr, &pixels, &pitch) == 0) {
        for (int y = 0; y < fb::H; ++y) {
            const uint32_t* src = frame.px + y * fb::W;
            uint32_t* dst = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(pixels) + y * pitch);
            if (a <= 0) {
                for (int x = 0; x < fb::W; ++x) dst[x] = src[x];
            } else {  // R1: global fade toward fade.rgb replaces the old palette LUT fade
                for (int x = 0; x < fb::W; ++x) {
                    const uint32_t c = src[x];
                    int r = int((c >> 16) & 0xff), g = int((c >> 8) & 0xff), b = int(c & 0xff);
                    r += (int(fade.r) - r) * a / 256;
                    g += (int(fade.g) - g) * a / 256;
                    b += (int(fade.b) - b) * a / 256;
                    dst[x] = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
                }
            }
        }
        SDL_UnlockTexture(p.texture);
    }

    // Aspect-preserving fit that can DOWNSCALE (the frame is native 1080p; the
    // window may be smaller). Letterbox the remainder; SDL scales the texture.
    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(p.renderer, &ww, &wh);
    int dw = ww, dh = ww * fb::H / fb::W;   // fit to width...
    if (dh > wh) { dh = wh; dw = wh * fb::W / fb::H; }  // ...unless height is the limit
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    p.present_scale = SDL_max(1, dw / fb::W);  // (kept for any integer-scale callers)
    p.present_dst = SDL_Rect{(ww - dw) / 2, (wh - dh) / 2, dw, dh};

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

    game_debug_use_door();    // B1.3: traverse into field node 1 so the save round-trips cur_node
    tick_with(Key::Right, 3);
    tick_with(Key::Down, 2);
    tick_with(Key::Save, 1);
    const uint64_t h_saved = game_state_hash();

    tick_with(Key::Left, 4);  // diverge (backtracks over known-walkable tiles)
    const uint64_t h_diverged = game_state_hash();

    tick_with(Key::Load, 1);
    const uint64_t h_loaded = game_state_hash();

    // Re-init from the same seed and replay the pre-save inputs: determinism.
    // RT1.2: sim_clock is now hashed, and the Save above consumed one frame (advancing the clock
    // + one world-timer step), so the replay must consume a matching frame to land on the same
    // clock/world state — a no-op Wait frame does exactly that.
    game_init(12345, services);
    game_debug_use_door();
    tick_with(Key::Right, 3);
    tick_with(Key::Down, 2);
    tick_with(Key::Wait, 1);  // mirror the Save frame's clock/world-timer advance
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

    game_debug_use_door();  // leave the sietch (doors are click-only now)
    int waits = 0;
    while (!game_is_dead() && waits < 6000) {
        game_debug_clear_hostiles();  // B3: isolate survival from the field node's escort
        tick_with(Key::Wait);
        ++waits;
    }
    std::printf("dead after %d waits\n", waits);
    const bool pass = game_is_dead() && waits > 50;
    std::printf("slicetest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// Writes the RGBA framebuffer as a 24-bit BMP (debug/demo).
bool bmp_save(const char* path, const Framebuffer& frame) {
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
            const uint32_t c = frame.px[y * w + x];
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
    auto tick_with = [&](Key k) {  // RT1.2: one world tick == kWorldStep frames (+~1-2 grid actions)
        for (int f = 0; f < kWorldStep; ++f) {
            in.pressed[int(k)] = true;
            game_update(in);
            in.end_tick();
        }
    };

    game_debug_use_door();                               // out of the sietch (click-only doors)
    for (int i = 0; i < 8; ++i) tick_with(Key::Right);   // onto open sand
    tick_with(Key::Thumper);
    for (int i = 0; i < 7; ++i) tick_with(Key::Left);    // fall back

    int max_state = 0;
    int turns = 0;
    while (turns < 240 && !game_is_dead()) {
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

// Headless harvest-op test: the spotter-kill route. Warp to the Deep Pan op,
// silence the spotters, and wait — the unwarned crawler's own noise brings
// the worm down on it.
int run_optest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(9001, services);

    InputState in;
    auto tick_with = [&](Key k) {
        in.pressed[int(k)] = true;
        game_update(in);
        in.end_tick();
    };

    // ops are campaign-scheduled: wait for the season's first harvest
    game_debug_warp(2 /* Deep Pan */, 2, 32);
    int op_state = 0, ox = 0, oy = 0, op_zone = 0;
    for (int t = 0; t < 400 && op_state != 1; ++t) {
        if (t % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
        game_debug_op_info(&op_state, &ox, &oy);
    }
    if (op_state != 1) { std::printf("optest FAIL (no op)\n"); return 1; }
    game_debug_op_zone(&op_zone);
    game_debug_warp(uint8_t(op_zone), ox - 12 >= 1 ? ox - 12 : 1, oy);
    game_debug_clear_hostiles();  // stale wanderers aren't this test's subject

    // Phase 1 (control): spotters alive — the pause discipline must keep the
    // crawler alive through 120 turns of worm cycles.
    for (int t = 0; t < 120 && !game_is_dead(); ++t) {
        if (t % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
    }
    game_debug_op_info(&op_state, nullptr, nullptr);
    const bool control_ok = !game_is_dead() && (op_state >= 1 && op_state <= 3);
    std::printf("  control: after 120 turns op state %d (live=%d)\n", op_state,
                control_ok ? 1 : 0);

    // Phase 2: knife the spotters — with no wormsign warning, the crawler's
    // own noise must doom it.
    game_debug_kill_spotters();
    int turns = 0;
    while (turns < 150 && !game_is_dead()) {
        if (turns % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
        game_debug_op_info(&op_state, nullptr, nullptr);
        if (op_state >= 4) break;
        ++turns;
    }
    game_debug_op_info(&op_state, nullptr, nullptr);
    std::printf("  spotters killed: %d turns -> op state %d (worm %d)\n", turns,
                op_state, game_worm_state());
    const bool pass = control_ok && op_state == 5 && !game_is_dead();
    std::printf("optest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// C2: the Energy Shield (Focus). An Awareness build soaks a hit on the shield (Life
// untouched), then the shield recharges after a quiet spell. (Replaces the retired
// Holtzman shieldtest — there is no slow-blade rule anymore.)
int run_focustest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    InputState in;
    // RT1.2: one world tick == kWorldStep frames (the shield recharges per world tick).
    auto wait = [&]() {
        for (int f = 0; f < kWorldStep; ++f) {
            in.pressed[int(Key::Wait)] = true; game_update(in); in.end_tick();
        }
    };

    game_debug_clear_hostiles();  // isolate the shield, not a fight
    game_debug_buff();            // Awareness 30 -> a real Focus pool; fills shield to max
    game_debug_refill();

    int shield0 = 0, shmax = 0, hp0 = 0;
    game_debug_shield_state(&shield0, &shmax, &hp0);
    const bool has_focus = shmax > 0 && shield0 == shmax;   // an Awareness build has a full shield

    // a hit smaller than the shield: Life must be untouched, the shield must drop
    game_debug_hurt(shmax / 2);
    int shield1 = 0, hp1 = 0;
    game_debug_shield_state(&shield1, nullptr, &hp1);
    const bool soaked = hp1 == hp0 && shield1 < shield0 && shield1 >= 0;

    // wait out the delay + recharge: the shield climbs back toward full
    for (int i = 0; i < 40; ++i) wait();
    int shield2 = 0, hp2 = 0;
    game_debug_shield_state(&shield2, nullptr, &hp2);
    const bool recharged = shield2 > shield1 && hp2 == hp0;

    // a hit bigger than the shield spills into Life
    game_debug_shield_state(&shield2, nullptr, &hp2);
    game_debug_hurt(shield2 + 5);
    int shield3 = 0, hp3 = 0;
    game_debug_shield_state(&shield3, nullptr, &hp3);
    const bool spilled = shield3 == 0 && hp3 == hp2 - 5;

    const bool pass = has_focus && soaked && recharged && spilled;
    std::printf("focustest %s (max %d soak %d->%d recharge ->%d spill hp %d->%d)\n",
                pass ? "PASS" : "FAIL", shmax, shield0, shield1, shield2, hp2, hp3);
    return pass ? 0 : 1;
}

// D1: equipping an affix moves the right derived stat.
int run_statmodtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);
    int hp, leth, sh, will, arm, dodge, cm;

    game_debug_equip_affix(int(items::AK_INC_LIFE), 0);
    game_debug_derived(&hp, 0, 0, 0, 0, 0, 0); const int hp_base = hp;
    game_debug_equip_affix(int(items::AK_INC_LIFE), 50);
    game_debug_derived(&hp, 0, 0, 0, 0, 0, 0);
    const bool inc_life = hp > hp_base;                         // +50% Life multiplies

    game_debug_equip_affix(int(items::AK_ADD_COND), 0);
    int hp_c0, leth_c0; game_debug_derived(&hp_c0, &leth_c0, 0, 0, 0, 0, 0);
    game_debug_equip_affix(int(items::AK_ADD_COND), 20);
    int hp_c1, leth_c1; game_debug_derived(&hp_c1, &leth_c1, 0, 0, 0, 0, 0);
    const bool cond = hp_c1 > hp_c0 && leth_c1 > leth_c0;       // Conditioning -> Life AND Lethality

    game_debug_equip_affix(int(items::AK_SHIELD), 0); int sh0; game_debug_derived(0, 0, &sh0, 0, 0, 0, 0);
    game_debug_equip_affix(int(items::AK_SHIELD), 30); game_debug_derived(0, 0, &sh, 0, 0, 0, 0);
    const bool focus = sh == sh0 + 30;                          // +flat Focus

    game_debug_equip_affix(int(items::AK_CRIT_MULT), 0); int cm0; game_debug_derived(0, 0, 0, 0, 0, 0, &cm0);
    game_debug_equip_affix(int(items::AK_CRIT_MULT), 50); game_debug_derived(0, 0, 0, 0, 0, 0, &cm);
    const bool critm = cm0 == 200 && cm == 250;                 // Vital Strike Damage (base x2 -> +50)

    game_debug_equip_affix(int(items::AK_ADD_ALL), 0); int d0; game_debug_derived(0, 0, 0, 0, 0, &d0, 0);
    game_debug_equip_affix(int(items::AK_ADD_ALL), 10); game_debug_derived(0, 0, 0, 0, 0, &dodge, 0);
    (void)will; (void)arm;
    const bool allattr = dodge > d0;                            // +all attributes -> Versatility -> Flow

    const bool pass = inc_life && cond && focus && critm && allattr;
    std::printf("statmodtest %s (life %d>%d cond[hp %d>%d leth %d>%d] focus %d=%d cm %d->%d flow %d>%d)\n",
                pass ? "PASS" : "FAIL", hp, hp_base, hp_c1, hp_c0, leth_c1, leth_c0,
                sh, sh0 + 30, cm0, cm, dodge, d0);
    return pass ? 0 : 1;
}

// D1: item rolls are tiered (deeper->higher), multi-affix at rare, ilvl-gated.
int run_affixtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);
    const int TOKEN = int(items::Slot::Token);
    int low_maxT = 0, hi_maxT = 0, best_naffix = 0, rares = 0;
    for (int i = 0; i < 300; ++i) {
        int rar, na, mt;
        game_debug_roll_item(1, 90, TOKEN, &rar, &na, &mt);   // ilvl 1, forced-rare bias
        if (mt > low_maxT) low_maxT = mt;
    }
    for (int i = 0; i < 300; ++i) {
        int rar, na, mt;
        game_debug_roll_item(10, 90, TOKEN, &rar, &na, &mt);  // ilvl 10
        if (mt > hi_maxT) hi_maxT = mt;
        if (rar == items::R_RARE) { ++rares; if (na > best_naffix) best_naffix = na; }
    }
    const bool tier_gate = low_maxT <= 1 && hi_maxT >= 4;   // T1 only at ilvl1; deep reaches high tiers
    const bool multi = best_naffix >= 3;                    // rares carry 3+ mods
    const bool got_rares = rares > 0;
    const bool pass = tier_gate && multi && got_rares;
    std::printf("affixtest %s (lowMaxT %d hiMaxT %d bestN %d rares %d)\n",
                pass ? "PASS" : "FAIL", low_maxT, hi_maxT, best_naffix, rares);
    return pass ? 0 : 1;
}

// C3: the combat-poison ailment — the player inflicts it via AK_POISON, and Toxin Resistance
// mitigates it when an enemy inflicts it on the player. Activates D1's inert AK_POISON + AK_RES_TOX.
int run_ailmenttest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);
    InputState in;
    // RT1.2: tick_with is a single frame (attacks land every kPlayerActionTicks frames, so combat
    // loops step frame-by-frame with a large guard); wait() is one full world tick (DoTs/freeze
    // tick once per world tick), == kWorldStep frames.
    auto tick_with = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };
    auto wait = [&]() {
        for (int f = 0; f < kWorldStep; ++f) {
            in.pressed[int(Key::Wait)] = true; game_update(in); in.end_tick();
        }
    };

    // (A) player inflicts Poison via AK_POISON on a landed hit -> enemy takes a DoT.
    // (mirror rpgtest's warm-up: an initial warp + a tick settles the resident zone before spawns.)
    game_debug_equip_affix(int(items::AK_POISON), 60);
    game_debug_warp(1, 30, 30);
    tick_with(Key::Sandwalk);
    int hp0 = -1;
    for (int sx = 18; sx <= 46 && hp0 <= 0; sx += 4) {  // find a spot where a bulwark can spawn adjacent
        game_debug_warp(1, sx, 30);
        game_debug_clear_hostiles();  // isolate the bulwark from zone wildlife
        game_debug_spawn_shield_adjacent(&hp0);
    }
    int guard = 0;
    while (game_debug_enemy_poison_turns() <= 0 && game_debug_shield_hp() > 0 && guard++ < 160) {
        game_debug_refill();
        tick_with(Key::Right);  // attack the adjacent bulwark until a hit lands + poisons it
    }
    const bool inflicted = game_debug_enemy_poison_turns() > 0;
    const int hp_a = game_debug_shield_hp();
    wait(); wait();  // poison ticks on the enemy
    const bool enemy_dot = game_debug_shield_hp() < hp_a;  // hp dropped (or enemy gone -> -1)

    // (B) player-side ailments: reset to the sheltered sietch (indoors -> no exposure/thirst noise
    // that would otherwise pollute the Life/shield deltas).
    game_init(778, services);
    game_debug_clear_hostiles();
    int hp = 0;
    game_debug_equip_affix(int(items::AK_POISON), 0);  // clean carrier, NO resistance
    game_debug_refill();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int start_noR = hp;
    game_debug_poison_player(5, 3);
    for (int i = 0; i < 3; ++i) wait();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int lost_noR = start_noR - hp;

    game_debug_equip_affix(int(items::AK_RES_TOX), 60);  // 60% Toxin Resistance
    game_debug_refill();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int start_R = hp;
    game_debug_poison_player(5, 3);
    for (int i = 0; i < 3; ++i) wait();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int lost_R = start_R - hp;

    const bool resisted = lost_noR > 0 && lost_R < lost_noR;

    // (C) Scorch (Heat DoT) — Heat Resistance mitigates it, like poison/Toxin.
    game_debug_equip_affix(int(items::AK_HP), 0);  // clean carrier, no resistances
    game_debug_refill();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int sc_start_noR = hp;
    game_debug_scorch_player(5, 3);
    for (int i = 0; i < 3; ++i) wait();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int sc_lost_noR = sc_start_noR - hp;
    game_debug_equip_affix(int(items::AK_HEAT), 60);  // 60% Heat Resistance ("of Noon")
    game_debug_refill();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int sc_start_R = hp;
    game_debug_scorch_player(5, 3);
    for (int i = 0; i < 3; ++i) wait();
    game_debug_player_stats(&hp, nullptr, nullptr, nullptr, nullptr, nullptr);
    const int sc_lost_R = sc_start_R - hp;
    const bool scorch_resisted = sc_lost_noR > 0 && sc_lost_R < sc_lost_noR;

    // (D) Bleed — a physical DoT that bypasses the Focus shield (Heat Resist does NOT help it).
    game_debug_equip_affix(int(items::AK_SHIELD), 40);  // give a shield; bleed should ignore it
    game_debug_refill();
    int bhp = 0, bsh = 0;
    game_debug_shield_state(&bsh, nullptr, &bhp);
    game_debug_bleed_player(4, 2);
    wait(); wait();
    int bhp2 = 0, bsh2 = 0;
    game_debug_shield_state(&bsh2, nullptr, &bhp2);
    const bool bleed_bypasses_shield = bhp2 < bhp && bsh2 == bsh;  // Life dropped, shield untouched

    // (E) Blind (Storm) — the player's accuracy drops while Blinded.
    game_debug_equip_affix(int(items::AK_HP), 0);  // clean, no precision affixes
    const int th0 = game_debug_player_tohit(8);
    game_debug_blind_player(3);
    const int th1 = game_debug_player_tohit(8);
    const bool blind_works = th1 < th0;

    // (F) Freeze (Cold control) — a frozen enemy loses its turn (frozen counter ticks down on skip).
    game_init(779, services);
    game_debug_equip_affix(int(items::AK_HP), 0);
    game_debug_warp(1, 30, 30);
    tick_with(Key::Sandwalk);
    int fhp = -1;
    for (int sx = 18; sx <= 46 && fhp <= 0; sx += 4) {
        game_debug_warp(1, sx, 30);
        game_debug_clear_hostiles();
        game_debug_spawn_shield_adjacent(&fhp);
    }
    game_debug_freeze_first_enemy(3);
    const int fz0 = game_debug_enemy_frozen_turns();
    wait();
    const int fz1 = game_debug_enemy_frozen_turns();
    const bool freeze_works = fz0 == 3 && fz1 == 2;  // the frozen enemy skipped its turn (counter ticked)

    const bool pass = inflicted && enemy_dot && resisted && scorch_resisted &&
                      bleed_bypasses_shield && blind_works && freeze_works;
    std::printf("ailmenttest %s (poison %d/%d scorch %d/%d bleed[hp %d->%d sh %d->%d] "
                "blind tohit %d->%d freeze %d->%d)\n",
                pass ? "PASS" : "FAIL", lost_noR, lost_R, sc_lost_noR, sc_lost_R,
                bhp, bhp2, bsh, bsh2, th0, th1, fz0, fz1);
    (void)hp_a; (void)inflicted; (void)enemy_dot;
    return pass ? 0 : 1;
}

// D1: a legacy DME3 vault converts to DME4 without losing banked items or the wallet.
int run_metamigratetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    (void)assets; (void)cmaps; (void)mixer;
    const bool pass = game_debug_meta_migrate_selftest() == 1;
    std::remove("meta.dat");
    std::printf("metamigratetest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// A whole season, three ways: win by denials + climax, lose by quota, lose by raid.
int run_campaigntest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    InputState in;
    // RT1.2: the season paces on world ticks, so one loop step == one world tick == kWorldStep
    // frames. (Boss re-warps every call and lands ~1-2 attacks per block — fine for a flow test.)
    auto tick_with = [&](Key k) {
        for (int f = 0; f < kWorldStep; ++f) {
            in.pressed[int(k)] = true;
            game_update(in);
            in.end_tick();
        }
    };

    // Leg 1: deny everything -> climax -> victory
    game_init(31337, services);
    game_debug_buff();
    int ending = 0, guard = 0;
    while (guard++ < 9000 && !game_is_dead()) {
        if (guard % 12 == 0) game_debug_refill();
        int opstate = 0, ox = 0, oy = 0, opzone = 0;
        game_debug_op_info(&opstate, &ox, &oy);
        game_debug_op_zone(&opzone);
        if (opstate >= 1 && opstate <= 3) {
            const char* phase = "op-wait";
            game_debug_warp(opzone, ox - 12 >= 1 ? ox - 12 : 1, oy);
            game_debug_clear_hostiles();  // flow test, not a combat test
            game_debug_kill_spotters();
            int w = 0;
            while (w++ < 400 && !game_is_dead()) {
                if (w % 12 == 0) game_debug_refill();
                tick_with(Key::Wait);
                game_debug_op_info(&opstate, nullptr, nullptr);
                if (opstate >= 4) break;
            }
            if (game_is_dead()) std::printf("  [died in %s]\n", phase);
            // let the worm finish gorging and dive before crossing the sand
            game_debug_warp(opzone, 2, 2);
            for (int w2 = 0; w2 < 16 && !game_is_dead(); ++w2) {
                game_debug_refill();
                tick_with(Key::Wait);
            }
            if (game_is_dead()) std::printf("  [died in corner-wait]\n");
            // Y3: champions roam but need not be fought; the BARON must die
            int guard2 = 0;
            tick_with(Key::Sandwalk);  // slow blade beats any shield trait
            while (game_debug_boss_alive() && game_debug_boss_is_baron() &&
                   guard2++ < 200 && !game_is_dead()) {
                game_debug_refill();  // flow test: the champion cannot outpace this
                int adx = 0, ady = 0;
                game_debug_warp_to_boss(&adx, &ady);
                Key k = Key::Right;
                if (adx > 0 && ady == 0) k = Key::Right;
                else if (adx < 0 && ady == 0) k = Key::Left;
                else if (adx == 0 && ady > 0) k = Key::Down;
                else if (adx == 0 && ady < 0) k = Key::Up;
                else if (adx > 0 && ady > 0) k = Key::DownRight;
                else if (adx > 0 && ady < 0) k = Key::UpRight;
                else if (adx < 0 && ady > 0) k = Key::DownLeft;
                else if (adx < 0 && ady < 0) k = Key::UpLeft;
                tick_with(k);
            }
            tick_with(Key::Sandwalk);
            if (game_is_dead()) std::printf("  [died in boss-fight, guard2 %d]\n", guard2);
            game_debug_warp(0, 33, 30);  // melt back to the sietch, Fremen-style
        } else {
            tick_with(Key::Wait);
        }
        int sh, de, al, st;
        game_debug_campaign(&sh, &de, &al, &st, &ending);
        if (ending) {
            std::printf("  leg1: ending %d after %d loop steps (ship %d deny %d "
                        "alert %d standing %d)\n", ending, guard, sh, de, al, st);
            break;
        }
    }
    if (!ending)
        std::printf("  leg1: no ending (dead %d, guard %d, cause '%s')\n",
                    game_is_dead() ? 1 : 0, guard, game_debug_death_cause());
    const bool leg1 = ending == 1;

    // Leg 2: do nothing -> the quota ships
    game_init(31338, services);
    ending = 0;
    game_debug_warp(0, 33, 30);  // wait it out in the sietch
    for (guard = 0; guard < 9000 && !ending; ++guard) {
        if (guard % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
        game_debug_campaign(nullptr, nullptr, nullptr, nullptr, &ending);
    }
    std::printf("  leg2: ending %d after %d waits\n", ending, guard);
    const bool leg2 = ending == 2;

    // Leg 3: blow the alert -> discovery -> raid
    game_init(31339, services);
    ending = 0;
    game_debug_warp(0, 33, 30);
    game_debug_set_alert(85);
    for (guard = 0; guard < 800 && !ending; ++guard) {
        if (guard % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
        game_debug_campaign(nullptr, nullptr, nullptr, nullptr, &ending);
    }
    std::printf("  leg3: ending %d after %d waits\n", ending, guard);
    const bool leg3 = ending == 3;

    const bool pass = leg1 && leg2 && leg3;
    std::printf("campaigntest %s (win %d quota %d raid %d)\n", pass ? "PASS" : "FAIL",
                leg1 ? 1 : 0, leg2 ? 1 : 0, leg3 ? 1 : 0);
    return pass ? 0 : 1;
}

// RT1.2: the defining real-time behavior — the world advances on its own fixed clock even when
// the player takes NO action. Stand in a field node (outdoors) with an EMPTY input for many
// frames; upkeep must run (thirst drains) while the player never moves. Proves the world tick is
// driven by the sim clock, not by player input (the whole point of the real-time conversion).
int run_rttest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    game_debug_warp(1, 30, 30);       // a field node: outdoors, so thirst accrues per world tick
    game_debug_clear_hostiles();      // isolate the clock from a fight
    game_debug_refill();              // full water/Life

    int w0 = 0, hp0 = 0, px0 = 0, py0 = 0;
    game_debug_player_stats(&hp0, nullptr, &w0, nullptr, nullptr, nullptr);
    game_debug_player_pos(&px0, &py0);

    InputState idle;                  // EMPTY — no key ever pressed or held
    for (int f = 0; f < kWorldStep * 20; ++f) { game_update(idle); idle.end_tick(); }

    int w1 = 0, hp1 = 0, px1 = 0, py1 = 0;
    game_debug_player_stats(&hp1, nullptr, &w1, nullptr, nullptr, nullptr);
    game_debug_player_pos(&px1, &py1);

    const bool world_ticked = w1 < w0;                  // upkeep ran with NO player input
    const bool player_idle = px1 == px0 && py1 == py0;  // no input -> the player never acted
    const bool pass = world_ticked && player_idle && !game_is_dead();
    std::printf("rttest %s (water %d->%d moved %d dead %d)\n", pass ? "PASS" : "FAIL",
                w0, w1, player_idle ? 0 : 1, game_is_dead() ? 1 : 0);
    return pass ? 0 : 1;
}

// Determinism: the same seed and input script twice must reach the same hash.
int run_dettest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    uint64_t hashes[2];
    for (int run = 0; run < 2; ++run) {
        game_init(20260721, services);
        InputState in;
        auto tick_with = [&](Key k, int n) {
            for (int i = 0; i < n; ++i) {
                in.pressed[int(k)] = true;
                game_update(in);
                in.end_tick();
            }
        };
        tick_with(Key::Down, 13);
        tick_with(Key::Right, 8);
        tick_with(Key::Thumper, 1);
        tick_with(Key::Left, 7);
        tick_with(Key::Wait, 120);
        tick_with(Key::Right, 10);
        tick_with(Key::Wait, 60);
        hashes[run] = game_state_hash();
    }
    const bool pass = hashes[0] == hashes[1];
    std::printf("dettest %s (%016llx vs %016llx)\n", pass ? "PASS" : "FAIL",
                (unsigned long long)hashes[0], (unsigned long long)hashes[1]);
    return pass ? 0 : 1;
}

// Worm-riding: bait, mount, steer east, and confirm the Maker obeys.
int run_ridetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);
    InputState in;
    auto tick_with = [&](Key k) {  // RT1.2: one world tick == kWorldStep frames (+~1-2 grid actions)
        for (int f = 0; f < kWorldStep; ++f) {
            in.pressed[int(k)] = true;
            game_update(in);
            in.end_tick();
        }
    };
    // RT1.2: exactly ONE player action — settle kWorldStep frames with no input so the action
    // cooldown expires, then one ready press. (A held Ride would mount AND then re-fire the
    // commit-extract; ride mount/steer/dismount must each be a single discrete action.)
    auto act = [&](Key k) {
        for (int f = 0; f < kWorldStep; ++f) { game_update(in); in.end_tick(); }
        in.pressed[int(k)] = true; game_update(in); in.end_tick();
    };

    game_debug_use_door();                               // out of the sietch (click-only doors)
    for (int i = 0; i < 8; ++i) tick_with(Key::Right);   // onto open sand
    tick_with(Key::Thumper);
    for (int i = 0; i < 7; ++i) tick_with(Key::Left);    // fall back
    int turns = 0;
    while (turns++ < 240 && game_worm_state() != 4 && !game_is_dead()) {
        if (turns % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
    }
    if (game_worm_state() != 4) { std::printf("ridetest FAIL (no surface)\n"); return 1; }

    game_debug_grant_hooks();
    game_debug_warp_to_worm();
    act(Key::Ride);
    if (!game_debug_riding()) { std::printf("ridetest FAIL (mount)\n"); return 1; }

    // Ride until the worm moves. The worm only swims onto sand ("shies from rock"), so try each
    // cardinal with ONE steering action and keep the first that advances it. One ride_move per
    // direction stays well under kExtractRideTurns, so a shy direction can't auto-extract the ride.
    int wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;
    game_debug_worm_pos(&wx0, &wy0);
    const Key dirs[4] = {Key::Right, Key::Left, Key::Down, Key::Up};
    bool moved = false;
    for (Key k : dirs) {
        int bx, by; game_debug_worm_pos(&bx, &by);
        act(k);
        int ax, ay; game_debug_worm_pos(&ax, &ay);
        if (ax != bx || ay != by) { moved = true; break; }
        if (!game_debug_riding()) break;  // safety: the ride ended
    }
    game_debug_worm_pos(&wx1, &wy1);
    act(Key::Ride);  // dismount / commit the ride home

    std::printf("worm rode from %d,%d to %d,%d (riding now %d, dead %d)\n",
                wx0, wy0, wx1, wy1, game_debug_riding() ? 1 : 0, game_is_dead() ? 1 : 0);
    const bool pass = moved && !game_debug_riding() && !game_is_dead();
    std::printf("ridetest %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// B4: worm-ride extraction bears you home — surface a Maker, mount, ride, and wake in the sietch.
int run_extractiontest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);
    InputState in;
    auto tick_with = [&](Key k) {  // RT1.2: one world tick == kWorldStep frames (+~1-2 grid actions)
        for (int f = 0; f < kWorldStep; ++f) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); }
    };
    // RT1.2: exactly ONE player action (settle the cooldown, then one ready press) — ride
    // mount and the commit-extract must each be a single discrete action, not a held key.
    auto act = [&](Key k) {
        for (int f = 0; f < kWorldStep; ++f) { game_update(in); in.end_tick(); }
        in.pressed[int(k)] = true; game_update(in); in.end_tick();
    };

    game_debug_use_door();                               // hub -> field node 1
    const int node_field = game_debug_cur_node();
    for (int i = 0; i < 8; ++i) tick_with(Key::Right);
    tick_with(Key::Thumper);
    for (int i = 0; i < 7; ++i) tick_with(Key::Left);
    int turns = 0;
    while (turns++ < 240 && game_worm_state() != 4 && !game_is_dead()) {
        if (turns % 12 == 0) game_debug_refill();
        tick_with(Key::Wait);
    }
    if (game_worm_state() != 4) { std::printf("extractiontest FAIL (no surface)\n"); return 1; }

    game_debug_warp_to_worm();
    act(Key::Ride);
    const bool mounted = game_debug_riding();
    act(Key::Ride);                                      // commit: the Maker bears you home
    const int node_home = game_debug_cur_node();

    const bool extracted = node_home == 0 && !game_debug_riding() && !game_is_dead();
    const bool pass = node_field > 0 && mounted && extracted;
    std::printf("extractiontest %s (field_node %d mounted %d home_node %d)\n",
                pass ? "PASS" : "FAIL", node_field, mounted, node_home);
    return pass ? 0 : 1;
}

// The extraction loop, end to end: kill -> XP/levels -> loot -> bank at the
// vault -> die/restart -> the banked gear survives; carried gear does not.
int run_rpgtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");  // clean slate for the persistence proof
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    InputState in;
    auto tick_with = [&](Key k) {
        in.pressed[int(k)] = true;
        game_update(in);
        in.end_tick();
    };

    game_debug_warp(1, 30, 30);
    tick_with(Key::Sandwalk);  // fight from the sandwalk stance (bulwarks have no slow-blade rule now)

    int level = 0, inv = 0, stash = 0;
    for (int kills = 0; kills < 8; ++kills) {
        // B1.3 retired edge-travel recycling; hunt at a fresh spot each kill so the slain
        // trooper's corpse/loot doesn't block the next spawn (Right walks when not adjacent).
        game_debug_warp(1, 18 + kills * 4, 30);
        int hp = 0;
        game_debug_spawn_shield_adjacent(&hp);
        if (hp <= 0) continue;  // unwalkable spot; try the next one
        int guard = 0;
        while (game_debug_shield_hp() > 0 && guard++ < 400) {  // RT1.2: ~1 attack / kPlayerActionTicks frames
            game_debug_refill();
            tick_with(Key::Right);
        }
        game_debug_clear_hostiles();  // clear any reinforcements drawn by noise
    }
    game_debug_grab_loot();
    game_debug_rpg(&level, &inv, &stash);
    std::printf("  after hunt: level %d, carrying %d, banked %d\n", level, inv, stash);
    const bool leveled = level >= 2;
    const bool looted = inv >= 1;

    game_debug_vault_deposit_all();
    game_debug_rpg(&level, &inv, &stash);
    const int banked = stash;
    std::printf("  banked %d items; dying now\n", banked);

    game_init(778, services);  // a new season: carried gear is gone
    game_debug_rpg(&level, &inv, &stash);
    std::printf("  fresh run: level %d, carrying %d, vault holds %d\n", level, inv, stash);
    const bool persisted = stash == banked && banked >= 1 && inv == 0 && level == 1;

    const int got = game_debug_vault_withdraw_one();
    game_debug_rpg(&level, &inv, &stash);
    std::printf("  withdrew %d -> carrying %d, vault %d\n", got, inv, stash);
    const bool withdrew = got == 1 && inv == 1 && stash == banked - 1;

    const bool pass = leveled && looted && persisted && withdrew;
    std::printf("rpgtest %s (leveled %d looted %d persisted %d withdrew %d)\n",
                pass ? "PASS" : "FAIL", leveled, looted, persisted, withdrew);
    return pass ? 0 : 1;
}

// B2b: the Revelation gear-pick fires at a forward door: pick a slot, then 1 of 3 items.
int run_reveltest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4321, services);

    game_debug_use_door();                 // hub -> node 1 (first entry: no Revelation)
    const int node_before = game_debug_cur_node();
    int stage = -1, inv0 = 0;
    game_debug_revelation(&stage, &inv0);
    const bool no_rev_on_entry = (stage == 0);

    game_debug_use_door();                 // forward door at node 1 -> opens the Revelation
    game_debug_revelation(&stage, &inv0);
    const bool opened_slot = (stage == 1);

    game_debug_revelation_pick(0);         // choose a slot -> roll 3 items
    game_debug_revelation(&stage, &inv0);
    const bool opened_item = (stage == 2);

    game_debug_revelation_pick(0);         // choose an item -> take it + advance
    int stage_end = -1, inv1 = 0;
    game_debug_revelation(&stage_end, &inv1);
    const int node_after = game_debug_cur_node();

    const bool gained = inv1 == inv0 + 1;
    const bool closed = stage_end == 0;
    const bool advanced = node_after > node_before;
    const bool pass = no_rev_on_entry && opened_slot && opened_item && gained && closed && advanced;
    std::printf("reveltest %s (entry_clean %d slot %d item %d gained %d closed %d advanced %d: %d->%d)\n",
                pass ? "PASS" : "FAIL", no_rev_on_entry, opened_slot, opened_item, gained, closed,
                advanced, node_before, node_after);
    return pass ? 0 : 1;
}

// B3: field nodes field an elite escort; the Awaken-10 Pinnacle is the Baron; killing him wins.
int run_gauntlettest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(2468, services);

    InputState in;
    auto tick_with = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };

    game_debug_warp_to_node(1);                 // Awaken-1 field node
    const int escort = game_debug_hostile_count();
    const bool has_escort = escort >= 1;

    game_debug_warp_to_node(40);                // Awaken-10 Pinnacle (the Baron)
    const bool boss_here = game_debug_boss_alive();
    const bool is_baron = game_debug_boss_is_baron();

    game_debug_buff();                          // end the fight before the worm intervenes
    tick_with(Key::Sandwalk);                   // sandwalk stance (the Baron has no Holtzman shield now)
    int atkx = 0, atky = 0, guard = 0;
    while (game_debug_boss_alive() && guard++ < 800) {
        game_debug_refill();
        game_debug_warp_to_boss(&atkx, &atky);
        Key k = atkx > 0 ? Key::Right : atkx < 0 ? Key::Left : atky > 0 ? Key::Down : Key::Up;
        tick_with(k);
    }
    int ending = 0;
    game_debug_campaign(nullptr, nullptr, nullptr, nullptr, &ending);
    const bool victory = ending == 1;  // campaign::kVictory

    const bool pass = has_escort && boss_here && is_baron && victory;
    std::printf("gauntlettest %s (escort %d boss %d baron %d victory %d)\n",
                pass ? "PASS" : "FAIL", escort, boss_here, is_baron, victory);
    return pass ? 0 : 1;
}

// B5: clearing a pinnacle advances the Awaken ladder (unlock the next level) in persistent meta.
int run_metatest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(1357, services);
    InputState in;
    auto tick_with = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };

    int u0 = 0, c0 = 0, b0 = 0;
    game_debug_awaken(&u0, &c0, &b0);           // fresh: unlocked 1, cleared 0, banked 0

    game_debug_warp_to_node(4);                 // the Awaken-1 pinnacle (Esmar Tuek)
    const bool boss = game_debug_boss_alive();
    game_debug_buff();
    int guard = 0;
    while (game_debug_boss_alive() && guard++ < 400) {
        game_debug_refill();
        int ax = 0, ay = 0;
        game_debug_warp_to_boss(&ax, &ay);
        Key k = ax > 0 ? Key::Right : ax < 0 ? Key::Left : ay > 0 ? Key::Down : Key::Up;
        tick_with(k);
    }
    int u1 = 0, c1 = 0, b1 = 0;
    game_debug_awaken(&u1, &c1, &b1);           // pinnacle cleared -> Awaken 2 unlocked

    const bool started = u0 == 1 && c0 == 0 && b0 == 0;
    const bool unlocked = u1 >= 2 && c1 >= 1;
    const bool pass = started && boss && unlocked;
    std::printf("metatest %s (start[u%d c%d b%d] boss %d -> [u%d c%d])\n",
                pass ? "PASS" : "FAIL", u0, c0, b0, boss, u1, c1);
    return pass ? 0 : 1;
}

// A4: click-to-move pathfinds to the goal and is deterministic across runs.
int run_clicktest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    const int gx = 33, gy = 33;  // a clear sietch-hall tile below spawn (33,30), no NPCs
    uint64_t h[2] = {0, 0};
    int reached[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        game_init(20260722, services);
        game_debug_click(gx, gy);
        InputState in;
        for (int i = 0; i < 400 && !reached[run]; ++i) {
            game_update(in);
            in.end_tick();
            int cx = 0, cy = 0;
            game_debug_player_pos(&cx, &cy);
            if (cx == gx && cy == gy) reached[run] = 1;
        }
        h[run] = game_state_hash();
    }
    const bool pass = reached[0] && reached[1] && h[0] == h[1];
    std::printf("clicktest %s (reached %d/%d, %016llx vs %016llx)\n", pass ? "PASS" : "FAIL",
                reached[0], reached[1], (unsigned long long)h[0], (unsigned long long)h[1]);
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    audio::Mixer mixer;
    if (!audio_load(mixer)) return 1;

    bool seamtest = false, slicetest = false, wormtest = false, autodemo = false,
         optest = false, opdemo = false, uishot = false, focustest = false,
         campaigntest = false, dettest = false, ridetest = false, animdebug = false,
         circledemo = false, fightdemo = false, rpgtest = false, clicktest = false,
         reveltest = false, gauntlettest = false, extractiontest = false, metatest = false,
         statmodtest = false, affixtest = false, metamigratetest = false, ailmenttest = false,
         rttest = false;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--dumpwav") == 0) return run_dumpwav(mixer);
        if (SDL_strcmp(argv[i], "--seamtest") == 0) seamtest = true;
        if (SDL_strcmp(argv[i], "--slicetest") == 0) slicetest = true;
        if (SDL_strcmp(argv[i], "--wormtest") == 0) wormtest = true;
        if (SDL_strcmp(argv[i], "--autodemo") == 0) autodemo = true;
        if (SDL_strcmp(argv[i], "--opdemo") == 0) { autodemo = true; opdemo = true; }
        if (SDL_strcmp(argv[i], "--circledemo") == 0) { autodemo = true; circledemo = true; }
        if (SDL_strcmp(argv[i], "--fightdemo") == 0) { autodemo = true; fightdemo = true; }
        if (SDL_strcmp(argv[i], "--optest") == 0) optest = true;
        if (SDL_strcmp(argv[i], "--uishot") == 0) uishot = true;
        if (SDL_strcmp(argv[i], "--focustest") == 0) focustest = true;
        if (SDL_strcmp(argv[i], "--statmodtest") == 0) statmodtest = true;
        if (SDL_strcmp(argv[i], "--affixtest") == 0) affixtest = true;
        if (SDL_strcmp(argv[i], "--metamigratetest") == 0) metamigratetest = true;
        if (SDL_strcmp(argv[i], "--ailmenttest") == 0) ailmenttest = true;
        if (SDL_strcmp(argv[i], "--campaigntest") == 0) campaigntest = true;
        if (SDL_strcmp(argv[i], "--dettest") == 0) dettest = true;
        if (SDL_strcmp(argv[i], "--rttest") == 0) rttest = true;
        if (SDL_strcmp(argv[i], "--ridetest") == 0) ridetest = true;
        if (SDL_strcmp(argv[i], "--animdebug") == 0) animdebug = true;
        if (SDL_strcmp(argv[i], "--rpgtest") == 0) rpgtest = true;
        if (SDL_strcmp(argv[i], "--clicktest") == 0) clicktest = true;
        if (SDL_strcmp(argv[i], "--reveltest") == 0) reveltest = true;
        if (SDL_strcmp(argv[i], "--gauntlettest") == 0) gauntlettest = true;
        if (SDL_strcmp(argv[i], "--extractiontest") == 0) extractiontest = true;
        if (SDL_strcmp(argv[i], "--metatest") == 0) metatest = true;
    }

    Palette master;
    if (!palette_load("data/palette.pal", master)) {
        std::fprintf(stderr, "palette: data/palette.pal missing, using built-in\n");
        master = palette_make_default();
    }
    Colormaps cmaps;
    colormaps_build(master, cmaps);

    Assets assets;
    if (!assets.load_all("data")) return 1;

    static anim::Registry anims;
    const bool anims_ok = anims.load("data", assets);
    if (!anims_ok) std::fprintf(stderr, "anims.txt missing or empty; static sprites\n");

    if (clicktest) return run_clicktest(mixer, assets, cmaps);
    if (seamtest) return run_seamtest(mixer, assets, cmaps);
    if (slicetest) return run_slicetest(mixer, assets, cmaps);
    if (wormtest) return run_wormtest(mixer, assets, cmaps);
    if (optest) return run_optest(mixer, assets, cmaps);
    if (focustest) return run_focustest(mixer, assets, cmaps);
    if (statmodtest) return run_statmodtest(mixer, assets, cmaps);
    if (affixtest) return run_affixtest(mixer, assets, cmaps);
    if (metamigratetest) return run_metamigratetest(mixer, assets, cmaps);
    if (ailmenttest) return run_ailmenttest(mixer, assets, cmaps);
    if (campaigntest) return run_campaigntest(mixer, assets, cmaps);
    if (dettest) return run_dettest(mixer, assets, cmaps);
    if (rttest) return run_rttest(mixer, assets, cmaps);
    if (ridetest) return run_ridetest(mixer, assets, cmaps);
    if (rpgtest) return run_rpgtest(mixer, assets, cmaps);
    if (reveltest) return run_reveltest(mixer, assets, cmaps);
    if (gauntlettest) return run_gauntlettest(mixer, assets, cmaps);
    if (extractiontest) return run_extractiontest(mixer, assets, cmaps);
    if (metatest) return run_metatest(mixer, assets, cmaps);

    Platform platform;
    if (!platform_init(platform, &mixer)) {
        platform_shutdown(platform);
        return 1;
    }

    GameServices services{&mixer, &assets, &cmaps, anims_ok ? &anims : nullptr,
                          /*interactive=*/!autodemo || uishot, animdebug, &master};
    game_init(autodemo ? (opdemo ? 9001 : 4242) : uint64_t(SDL_GetPerformanceCounter()),
              services);
    mixer.push({audio::Cmd::PlaySong, uint8_t(g_song_ambient), 0, 0});

    // --autodemo: the worm-bait scenario played by the game itself, one input
    // every few ticks. --opdemo instead warps beside the Deep Pan harvest op
    // and watches it play out. Purely for demos/screenshots.
    struct DemoStep { Key key; int count; };
    static const DemoStep kWormScript[] = {
        {Key::Down, 13}, {Key::Right, 8}, {Key::Thumper, 1}, {Key::Left, 4},
        {Key::Glowglobe, 1}, {Key::Left, 3}, {Key::Wait, 60},
    };
    static const DemoStep kOpScript[] = {{Key::Wait, 220}};
    static const DemoStep kFightScript[] = {
        {Key::Sandwalk, 1}, {Key::Right, 8}, {Key::Thumper, 1}, {Key::Wait, 10},
    };
    static const DemoStep kCircleScript[] = {
        {Key::Down, 13},
        {Key::Right, 4}, {Key::UpRight, 3}, {Key::Up, 4}, {Key::UpLeft, 3},
        {Key::Left, 4}, {Key::DownLeft, 3}, {Key::Down, 4}, {Key::DownRight, 3},
        {Key::Right, 4}, {Key::Up, 4}, {Key::Left, 4}, {Key::Down, 4},
        {Key::Wait, 30},
    };
    const DemoStep* demo_script = opdemo ? kOpScript
                                  : circledemo ? kCircleScript
                                  : fightdemo ? kFightScript : kWormScript;
    const int demo_script_len = opdemo ? int(SDL_arraysize(kOpScript))
                                : circledemo ? int(SDL_arraysize(kCircleScript))
                                : fightdemo ? int(SDL_arraysize(kFightScript))
                                            : int(SDL_arraysize(kWormScript));
    if (opdemo) {
        game_debug_warp(2, 2, 32);
        int st = 0, ox = 0, oy = 0;
        game_debug_op_info(&st, &ox, &oy);
        game_debug_warp(2, ox - 7 >= 1 ? ox - 7 : 1, oy - 2);
    }
    if (fightdemo) {
        game_debug_warp(1, 30, 30);
        int hp = 0;
        game_debug_spawn_shield_adjacent(&hp);
    }
    int demo_step = 0, demo_left = autodemo ? demo_script[0].count : 0, demo_cooldown = 0;

    static Framebuffer frame{};  // ~8MB at 1080p — keep off the stack
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
            if (autodemo && demo_step < demo_script_len) {
                if (--demo_cooldown <= 0) {
                    input.pressed[int(demo_script[demo_step].key)] = true;
                    demo_cooldown = 6;
                    if (--demo_left <= 0 && ++demo_step < demo_script_len)
                        demo_left = demo_script[demo_step].count;
                }
            }
            if (!game_update(input)) running = false;
            input.end_tick();
            accum -= tick_period;
            // Options screen: apply the game's fullscreen/sound requests.
            if (game_consume_fullscreen_request()) toggle_fullscreen(platform);
            if (platform.audio_dev)
                SDL_PauseAudioDevice(platform.audio_dev, game_muted() ? 1 : 0);
        }

        game_render(frame);
        present(platform, frame, game_fade());

        if (autodemo) {
            static int demo_ticks = 0, shot = 0;
            if (++demo_ticks % (fightdemo ? 6 : 24) == 0 && shot < 24) {
                char name[32];
                std::snprintf(name, sizeof name, "demo_%02d.bmp", shot++);
                bmp_save(name, frame);
            }
        }
        if (uishot) {
            static int ui_ticks = 0;
            ++ui_ticks;
            if (ui_ticks == 90 || ui_ticks == 150) input.pressed[int(Key::Right)] = true;
            if (ui_ticks == 60) bmp_save("uishot_0.bmp", frame);
            if (ui_ticks == 120) bmp_save("uishot_1.bmp", frame);
            if (ui_ticks == 180) bmp_save("uishot_2.bmp", frame);
            if (ui_ticks > 200) running = false;
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
