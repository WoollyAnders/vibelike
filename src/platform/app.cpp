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
    {SDL_SCANCODE_Q, Key::Ability1}, {SDL_SCANCODE_E, Key::Ability2}, {SDL_SCANCODE_F, Key::Ability3},
    {SDL_SCANCODE_1, Key::Flask1},   {SDL_SCANCODE_2, Key::Flask2},   {SDL_SCANCODE_3, Key::Flask3},
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

    // (G) C3 enemy-element roll (defensive half): elemental enemy hits are GATED by Awaken depth,
    // and a rolled element inflicts its ailment on the player, mitigated by the matching resist.
    const bool elem_gate = game_debug_enemy_element_chance(1) == 0 &&
                           game_debug_enemy_element_chance(2) == 0 &&   // below the Awaken-3 gate
                           game_debug_enemy_element_chance(3) > 0 &&
                           game_debug_enemy_element_chance(10) > game_debug_enemy_element_chance(3);
    game_init(780, services);
    game_debug_clear_hostiles();
    game_debug_equip_affix(int(items::AK_HP), 0);  // clean, no Cold Resist
    game_debug_inflict_element(1 /*Cold*/, 10);
    const int fz_noR = game_debug_player_frozen_turns();
    game_init(780, services);
    game_debug_clear_hostiles();
    game_debug_equip_affix(int(items::AK_RES_COLD), 75);  // 75% Cold Resist shortens the Freeze
    game_debug_inflict_element(1 /*Cold*/, 10);
    const int fz_R = game_debug_player_frozen_turns();
    const bool elem_route = fz_noR > 0 && fz_R > 0 && fz_R < fz_noR;

    const bool pass = inflicted && enemy_dot && resisted && scorch_resisted &&
                      bleed_bypasses_shield && blind_works && freeze_works && elem_gate && elem_route;
    std::printf("ailmenttest %s (poison %d/%d scorch %d/%d bleed[hp %d->%d sh %d->%d] "
                "blind tohit %d->%d freeze %d->%d elem gate%d route[%d>%d])\n",
                pass ? "PASS" : "FAIL", lost_noR, lost_R, sc_lost_noR, sc_lost_R,
                bhp, bhp2, bsh, bsh2, th0, th1, fz0, fz1, elem_gate, fz_noR, fz_R);
    (void)hp_a; (void)inflicted; (void)enemy_dot;
    return pass ? 0 : 1;
}

// RT2: timed Freeze (0.3-3 s, scaled by cold_dmg:max-Life) + the Stun ailment (PoE threshold +
// chance, two-way). (A)/(B) assert the deterministic seconds-based formulas; (C) the player Stun
// action-lockout; (D) a real melee hit Stuns a foe (the player->enemy wiring), forced via a huge
// Stun Buildup carrier so the roll is a certainty, not a coin-flip.
int run_stuntest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);
    InputState in;
    auto tick_with = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };
    auto wait = [&]() {  // one world tick
        for (int f = 0; f < kWorldStep; ++f) { in.pressed[int(Key::Wait)] = true; game_update(in); in.end_tick(); }
    };
    const int ML = 100;  // a round max-Life so the ratios read as percentages

    // (A) Freeze scales with cold_dmg : max-Life, floored at 0.3 s, capped at 3.0 s.
    const int fz_lo  = game_debug_freeze_ticks(5,   ML);  // 5%  -> below the 0.3 s floor -> none
    const int fz_mid = game_debug_freeze_ticks(50,  ML);  // 50% -> mid
    const int fz_hi  = game_debug_freeze_ticks(100, ML);  // 100% -> cap
    const int fz_ovr = game_debug_freeze_ticks(300, ML);  // >100% -> still capped
    const bool freeze_scales = fz_lo == 0 && fz_mid >= tune::kFreezeMinTicks && fz_mid < fz_hi &&
                               fz_hi == tune::kFreezeMaxTicks && fz_ovr == tune::kFreezeMaxTicks;
    const bool freeze_secs = tune::kFreezeMinTicks == tune::secs_to_ticks(0.3) &&
                             tune::kFreezeMaxTicks == tune::secs_to_ticks(3.0);

    // (B) Stun: below threshold -> none; scales with the ratio (capped); reduction shortens it;
    // Stun Buildup inflates a sub-threshold hit over the line.
    const int st_lo   = game_debug_stun_ticks(10,  ML, 0,   0);   // 10% < 15% threshold -> none
    const int st_mid  = game_debug_stun_ticks(30,  ML, 0,   0);
    const int st_hi   = game_debug_stun_ticks(100, ML, 0,   0);   // full-Life hit -> longest
    const int st_red  = game_debug_stun_ticks(100, ML, 0,   50);  // 50% Stun Reduction -> shorter
    const int st_mult = game_debug_stun_ticks(10,  ML, 100, 0);   // 10% x2 buildup -> now stuns
    const bool stun_formula = st_lo == 0 && st_mid > 0 && st_hi > st_mid &&
                              st_hi <= tune::kStunMaxTicks && st_red > 0 && st_red < st_hi && st_mult > 0;
    const bool stun_chance = game_debug_stun_chance(10, ML, 0, 0) == 0 &&      // below threshold
                             game_debug_stun_chance(100, ML, 0, 0) > 0 &&      // big hit can stun
                             // at a mid ratio (chance not yet saturated at 100), reduction lowers it
                             game_debug_stun_chance(40, ML, 0, 50) < game_debug_stun_chance(40, ML, 0, 0);

    // (C) a Stunned player cannot act until it wears off.
    game_debug_warp(1, 30, 30);
    game_debug_clear_hostiles();
    game_debug_refill();
    game_debug_stun_player(6);  // ~0.9 s
    const bool locked = game_debug_player_ready() == 0;
    int bx = 0, by = 0; game_debug_player_pos(&bx, &by);
    for (int i = 0; i < 6; ++i) tick_with(Key::Right);  // input ignored while stunned
    int mx = 0, my = 0; game_debug_player_pos(&mx, &my);
    const bool no_move_stunned = mx == bx && my == by;
    for (int i = 0; i < 8; ++i) wait();                 // wear the Stun off (>= 6 world ticks)
    const bool freed = game_debug_player_stun_turns() == 0 && game_debug_player_ready() == 1;
    int fx = 0, fy = 0; game_debug_player_pos(&fx, &fy);
    for (int i = 0; i < 24; ++i) tick_with(Key::Right);  // now the player moves (non-vacuous: tile walkable)
    int gx = 0, gy = 0; game_debug_player_pos(&gx, &gy);
    const bool moves_after = gx != fx || gy != fy;

    // (D) a real melee hit Stuns a foe. A huge Stun Buildup carrier makes the roll a certainty.
    game_debug_equip_affix(int(items::AK_STUN_MULT), 2000);
    int ehp = -1;
    for (int sx = 18; sx <= 46 && ehp <= 0; sx += 4) {
        game_debug_warp(1, sx, 30);
        game_debug_clear_hostiles();
        game_debug_spawn_shield_adjacent(&ehp);
    }
    bool stunned_foe = false;
    for (int guard = 0; guard < 160 && game_debug_shield_hp() > 0 && !stunned_foe; ++guard) {
        game_debug_refill();
        tick_with(Key::Right);
        if (game_debug_enemy_stun_turns() > 0) stunned_foe = true;
    }

    const bool pass = freeze_scales && freeze_secs && stun_formula && stun_chance &&
                      locked && no_move_stunned && freed && moves_after && stunned_foe;
    std::printf("stuntest %s (freeze %d/%d/%d/%d stun %d/%d/%d red%d mult%d "
                "lock %d nomove %d freed %d moved %d foe %d)\n", pass ? "PASS" : "FAIL",
                fz_lo, fz_mid, fz_hi, fz_ovr, st_lo, st_mid, st_hi, st_red, st_mult,
                locked, no_move_stunned, freed, moves_after, stunned_foe);
    return pass ? 0 : 1;
}

// C3: base classes + branching ascendancies. Each base creates distinctly + unascended; the sietch
// rite is Awaken-gated and base-matched, happens once, and the ascendancy persists across death
// (meta.dat DME5 round-trip). Ascendancy enum: None=0, Fedaykin=1..DesertGhost=2..WaterReaver=3
// (Knifehand); Saboteur=4..Wayfinder=5..Sunwalker=6 (Wanderer); Sayyadina=7.. (Oracle).
int run_classtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");  // fresh: awaken_unlocked == 1, no persisted character
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    // (A) each base applies distinctly and starts unascended (Ascendancy::None == 0)
    int b = -1, a = -1;
    game_debug_apply_base(0); game_debug_class(&b, &a); const bool knifehand = b == 0 && a == 0;
    game_debug_apply_base(1); game_debug_class(&b, &a); const bool wanderer  = b == 1 && a == 0;
    game_debug_apply_base(2); game_debug_class(&b, &a); const bool oracle    = b == 2 && a == 0;

    // (B) the rite is GATED by the Desert Trials — a fresh character (no ordeals passed) is refused
    game_debug_apply_base(0);  // Knifehand
    const bool gated = game_debug_ascend(1 /*Fedaykin*/) == 0;
    game_debug_complete_trials();  // pass the three desert ordeals -> the Awakening opens

    // (C) you may only pick one of your base's ascendancies, and only once
    const bool wrong_base = game_debug_ascend(6 /*Sunwalker: a Wanderer ascendancy*/) == 0;
    const bool right = game_debug_ascend(1 /*Fedaykin*/) == 1;
    game_debug_class(&b, &a); const bool ascended = a == 1;
    const bool once = game_debug_ascend(2 /*Desert Ghost*/) == 0;  // already ascended -> refused

    // (D) the ascendancy persists across death: ascend() saved meta; reload it and confirm
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = mb == 0 && ma == 1;  // Knifehand / Fedaykin survived write->read

    const bool pass = knifehand && wanderer && oracle && gated && wrong_base && right &&
                      ascended && once && persisted;
    std::printf("classtest %s (bases %d/%d/%d gated %d wrongbase %d right %d asc %d once %d persist %d/%d)\n",
                pass ? "PASS" : "FAIL", knifehand, wanderer, oracle, gated, wrong_base, right,
                ascended, once, mb, ma);
    std::remove("meta.dat");
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

// Step 1a: extraction is "let the vision fade" (no worm) — enter a vision-run, choose to end it,
// wake in the sietch with the haul banked. (Worm-riding is parked for the endgame Long Ride, so the
// old worm-ride ridetest is retired.)
int run_extractiontest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);

    game_debug_use_door();                 // hub -> a field vision-node
    const int node_field = game_debug_cur_node();
    game_debug_extract();                  // fade the vision = extraction (no Maker needed)
    const int node_home = game_debug_cur_node();

    const bool pass = node_field > 0 && node_home == 0 && !game_debug_riding() && !game_is_dead();
    std::printf("extractiontest %s (field_node %d home_node %d)\n",
                pass ? "PASS" : "FAIL", node_field, node_home);
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

// B6: the Guild tithe difficulty dial. Withholding the tithe (unpaid runs) makes mobs tougher +
// richer; paying the Guild grinds it down; the campaign caps it, and breaking the Guild (endgame)
// raises the cap. Persists across a meta.dat DME6 round-trip. Debt defaults 0 (baseline).
int run_guildtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");  // fresh: paid up (debt 0)
    GameServices services{&mixer, &assets, &cmaps};
    game_init(777, services);

    const bool fresh = game_debug_guild_debt() == 0 && game_debug_guild_difficulty() == 0;

    // baseline scaled mob HP at a field spot (find one where a bulwark can spawn adjacent)
    int hp0 = -1;
    for (int sx = 18; sx <= 46 && hp0 <= 0; sx += 4) {
        game_debug_warp(1, sx, 30);
        game_debug_clear_hostiles();
        game_debug_spawn_shield_adjacent(&hp0);
    }

    // (A) withhold the tithe -> harder mobs + higher difficulty (which also feeds richer loot)
    game_debug_add_guild_debt(4);
    const int diff4 = game_debug_guild_difficulty();
    game_debug_clear_hostiles();
    int hp4 = -1; game_debug_spawn_shield_adjacent(&hp4);
    const bool harder = diff4 == 4 && hp0 > 0 && hp4 > hp0;

    // (B) pay the Guild -> difficulty drops back down
    game_debug_pay_guild(3);
    const bool paid = game_debug_guild_debt() == 1 && game_debug_guild_difficulty() == 1;

    // (C) campaign cap clamps the difficulty
    game_debug_add_guild_debt(50);
    const bool camp_capped = game_debug_guild_difficulty() == tune::kGuildDebtCampaignCap;

    // (D) breaking the Guild Navigator raises the cap; more debt now climbs past the campaign cap
    game_debug_break_guild();
    game_debug_add_guild_debt(50);
    const bool end_capped = game_debug_guild_difficulty() == tune::kGuildDebtEndgameCap;

    // (E) the tithe + broken flag survive a meta.dat DME6 round-trip
    int mb = 0, ma = 0; game_debug_meta_reload_class(&mb, &ma);  // reloads meta.dat into g_meta
    const bool persisted = game_debug_guild_debt() == tune::kGuildDebtEndgameCap &&
                           game_debug_guild_difficulty() == tune::kGuildDebtEndgameCap;  // broken persisted

    // (F) B7 hub-signal: with spice, signalling the Guild from the sietch pays the tithe down
    const int debt_before = game_debug_guild_debt();
    const bool broke_signal = game_debug_signal_guild() == 0;  // no spice -> can't pay
    game_debug_add_spice(500);
    const bool signalled = game_debug_signal_guild() == 1 && game_debug_guild_debt() < debt_before;

    const bool pass = fresh && harder && paid && camp_capped && end_capped && persisted &&
                      broke_signal && signalled;
    std::printf("guildtest %s (fresh %d hp %d->%d diff4 %d paid %d camp %d end %d persist %d signal[%d %d])\n",
                pass ? "PASS" : "FAIL", fresh, hp0, hp4, diff4, paid, camp_capped, end_capped, persisted,
                broke_signal, signalled);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// B7: a wandering field vendor leaves a footprint trail leading to it. The trail is deterministic
// (RNG-free, derived from the distance field), connects the vendor to the player, and clears when
// the vendor expires (ignored).
int run_vendortest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};

    // (A) a materialized vendor lays a trail that connects it to the player
    game_init(4242, services);
    game_debug_use_door();                 // hub -> a field vision-node (open desert)
    int px = 0, py = 0; game_debug_player_pos(&px, &py);
    game_debug_spawn_sideobj(1 /*caravan*/, px + 10, py);
    const bool active = game_debug_sideobj_active() == 1;
    const int len1 = game_debug_footprint_count();
    const int ok1 = game_debug_footprint_ok();
    const bool leads = len1 > 0 && ok1 == 1;

    // (B) deterministic: the identical scenario yields the identical trail (footprints are RNG-free,
    //     derived from the distance field + the serialized vendor tile -> save/load-identical too)
    game_init(4242, services);
    game_debug_use_door();
    int qx = 0, qy = 0; game_debug_player_pos(&qx, &qy);
    game_debug_spawn_sideobj(1, qx + 10, qy);
    const int len2 = game_debug_footprint_count();
    const bool deterministic = len2 == len1;

    // (C) ignored -> the vendor (and its trail) expire
    game_debug_expire_sideobj();
    const bool expired = game_debug_sideobj_active() == 0 && game_debug_footprint_count() == 0;

    // (D) the Ninth Rock rendezvous: riding out puts you in the remote-desert zone with the Guild
    // agent (a sideobj Envoy) + a footprint trail to it; returning wakes you back in the sietch hub.
    game_init(4242, services);           // start in the sietch hub (cur_node 0)
    game_debug_enter_rendezvous();
    const bool at_rz = game_debug_at_rendezvous() == 1;
    const bool agent = game_debug_sideobj_active() == 2;         // the Guild Envoy waits
    const bool rz_trail = game_debug_footprint_count() > 0 && game_debug_footprint_ok() == 1;
    game_debug_return_rendezvous();
    const bool home = game_debug_at_rendezvous() == 0 && game_debug_cur_node() == 0;
    const bool rendezvous = at_rz && agent && rz_trail && home;

    const bool pass = active && leads && deterministic && expired && rendezvous;
    std::printf("vendortest %s (active %d trail %d ok %d det[%d==%d] expired %d rz[at%d agent%d trail%d home%d])\n",
                pass ? "PASS" : "FAIL", active, len1, ok1, len1, len2, expired,
                at_rz, agent, rz_trail, home);
    return pass ? 0 : 1;
}

// B8: Harkonnen patrols are route-walkers (Ai::Patrol) that pace a beat around their anchor and
// chase the moment they see you. (Ai::Patrol == 4: Idle,Wander,Hunt,Guard,Patrol.)
int run_patroltest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};

    // (A) a patroller far from the player paces its beat: it moves, stays within the leash of its
    //     anchor, and never flips to Hunt (the player is well out of sight).
    game_init(31337, services);
    game_debug_use_door();                 // hub -> a field vision-node (open desert)
    int px = 0, py = 0; game_debug_player_pos(&px, &py);
    const int dirx = px < 32 ? 1 : -1, diry = py < 32 ? 1 : -1;
    for (int m = 20; m >= 14 && game_debug_patrol_info(0, 0, 0, 0, 0) == 0; --m)
        game_debug_spawn_patrol(px + dirx * m, py + diry * m);
    int sx = 0, sy = 0, sai = 0, ax = 0, ay = 0;
    const int n0 = game_debug_patrol_info(&sx, &sy, &sai, &ax, &ay);
    const bool spawned = n0 >= 1 && sai == 4 && ax == sx && ay == sy;  // fresh: pos == anchor

    game_debug_world_tick(6);
    int x1 = 0, y1 = 0, ai1 = 0, ax1 = 0, ay1 = 0;
    const int n1 = game_debug_patrol_info(&x1, &y1, &ai1, &ax1, &ay1);
    int cheb = x1 - ax1 < 0 ? ax1 - x1 : x1 - ax1;
    const int cheb_y = y1 - ay1 < 0 ? ay1 - y1 : y1 - ay1;
    if (cheb_y > cheb) cheb = cheb_y;
    const bool moved = x1 != ax1 || y1 != ay1;
    const bool paced = n1 >= 1 && ai1 == 4 && cheb <= 6 && moved;  // still patrolling, within leash

    // (B) a patroller dropped right next to the player must see you and flip to Hunt (chase on sight).
    game_init(31337, services);
    game_debug_use_door();
    int bx = 0, by = 0; game_debug_player_pos(&bx, &by);
    const int off[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,-1},{1,-1},{-1,1}};
    for (int k = 0; k < 8 && game_debug_patrol_info(0, 0, 0, 0, 0) == 0; ++k)
        game_debug_spawn_patrol(bx + off[k][0], by + off[k][1]);
    game_debug_world_tick(2);
    int cai = 0;
    const int n2 = game_debug_patrol_info(nullptr, nullptr, &cai, nullptr, nullptr);
    const bool chased = n2 >= 1 && cai == 2;  // Ai::Hunt

    const bool pass = spawned && paced && chased;
    std::printf("patroltest %s (spawn %d pace[ai%d leash%d moved%d] chase_ai %d)\n",
                pass ? "PASS" : "FAIL", spawned, ai1, cheb, moved, cai);
    return pass ? 0 : 1;
}

// B9: a rare hidden cavern is a transient sub-zone (kZoneCave == 3). Stepping into a cave mouth in a
// field node drops you into a stocked cavern (loot + spice, a guardian); the cavern door climbs you
// back out to the same field spot. cur_node never changes — it's an excursion, not a node move.
int run_cavetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    GameServices services{&mixer, &assets, &cmaps};

    // (A) enter from a field node: land in the cave zone, stocked, cur_node unchanged.
    game_init(9001, services);
    game_debug_use_door();                 // hub -> field vision-node
    const int node = game_debug_cur_node();
    int px = 0, py = 0; game_debug_player_pos(&px, &py);
    game_debug_enter_cave();
    const bool inside = game_debug_in_cave() == 1 && game_debug_cur_zone() == 3;
    const int reward = game_debug_cave_reward_count();
    const bool node_kept = game_debug_cur_node() == node;

    // (B) climb back out: field zone resumes at the return spot, the cave flag clears.
    game_debug_exit_cave();
    int rx = 0, ry = 0; game_debug_player_pos(&rx, &ry);
    const bool back = game_debug_in_cave() == 0 && game_debug_cur_zone() != 3 &&
                      game_debug_cur_node() == node && rx == px && ry == py;

    // (C) deterministic: the identical run yields the identical cavern contents.
    game_init(9001, services);
    game_debug_use_door();
    game_debug_enter_cave();
    const int reward2 = game_debug_cave_reward_count();
    const bool deterministic = reward2 == reward;

    // (D) save + load while inside the cavern restores it (exercises the in_cave load-regen branch).
    game_init(9001, services);
    game_debug_use_door();
    game_debug_enter_cave();
    const int r_before = game_debug_cave_reward_count();
    InputState in;
    auto tick1 = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };
    tick1(Key::Save);
    tick1(Key::Load);
    const bool roundtrip = game_debug_in_cave() == 1 && game_debug_cur_zone() == 3 &&
                           game_debug_cave_reward_count() == r_before;

    const bool pass = inside && reward > 0 && node_kept && back && deterministic && roundtrip;
    std::printf("cavetest %s (in %d zone3 reward %d node_kept %d back %d det[%d==%d] saveload %d)\n",
                pass ? "PASS" : "FAIL", inside, reward, node_kept, back, reward, reward2, roundtrip);
    return pass ? 0 : 1;
}

// Desert Trials: three desert ordeals faced inside vision-runs. Passing all three (persisted in
// meta.dat, DME7) opens the Awakening — the sietch ascension rite — replacing the placeholder
// Awaken>=2 gate. Bits: 0 sandwalk, 1 storm, 2 worm.
int run_trialtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(555, services);

    // (A) in the sietch hub (not a vision-run), an ordeal cannot be passed — the gate guards it.
    game_debug_trigger_trial(1);
    const bool hub_noop = game_debug_trials() == 0 && game_debug_can_ascend() == 0;

    // (B) inside a run each ordeal sets its bit; the Awakening opens only once all three are passed.
    game_debug_use_door();                 // hub -> field vision-run
    game_debug_trigger_trial(0); const int m1 = game_debug_trials();
    game_debug_trigger_trial(1); const int m2 = game_debug_trials();
    const bool partial_gated = game_debug_can_ascend() == 0;   // 2 of 3 -> still shut
    game_debug_trigger_trial(2); const int m3 = game_debug_trials();
    const bool progressed = m1 == 1 && m2 == 3 && m3 == 7;
    const bool opens = game_debug_can_ascend() == 1;

    // (C) DME7 round-trip: the Trials persist across a meta reload (like the character/ascendancy).
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_trials() == 7;

    const bool pass = hub_noop && progressed && partial_gated && opens && persisted;
    std::printf("trialtest %s (hub_noop %d prog %d/%d/%d partial_gated %d opens %d persist %d)\n",
                pass ? "PASS" : "FAIL", hub_noop, m1, m2, m3, partial_gated, opens, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// D3e: Spice Essence — the Awakening resource. It is NOT a bench currency; a rare deep-desert find
// banks straight to meta and opens the rite as an alternative to the Desert Trials, spent when so used.
int run_essencetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(4242, services);
    game_debug_apply_base(0);  // Knifehand (a Fedaykin base)

    // (A) fresh: no Essence, no Trials -> the rite is shut
    const bool shut = game_debug_spice_essence() == 0 && game_debug_can_ascend() == 0;

    // (B) a banked Spice Essence opens the Awakening even with the Trials unpassed (the Water of Life)
    game_debug_grant_spice_essence(2);
    const bool opens = game_debug_spice_essence() == 2 && game_debug_can_ascend() == 1;

    // (C) ascending by the Essence path succeeds and CONSUMES one (the Trials path would not)
    const bool right = game_debug_ascend(1 /*Fedaykin*/) == 1;
    const bool spent = game_debug_spice_essence() == 1;

    // (D) DMED round-trip: the banked Essence survives a meta reload
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_spice_essence() == 1;

    const bool pass = shut && opens && right && spent && persisted;
    std::printf("essencetest %s (shut %d opens %d ascend %d spent %d persist %d)\n",
                pass ? "PASS" : "FAIL", shut, opens, right, spent, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// Endgame (the Jihad): beating the Baron opens the Chosen Path; spice-commissioned strikes build
// directed pressure per Great Power (factions 0 BeneGesserit,1 Tleilaxu,2 Ixian,3 Corrino,4 Guild);
// full pressure opens that homeworld pinnacle; each pinnacle yields a Golden Path shard; all five
// open the capstone (the alternate Paul). Data core — the strike/pinnacle zones layer on later.
int run_endgametest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(2077, services);

    // (A) shut until the Chosen Path — no strikes, no pinnacles
    const bool shut = game_debug_endgame_open() == 0 &&
                      game_debug_commission_strike(2 /*Ixian*/, 1) == 0 &&
                      game_debug_pinnacle_available(2) == 0;

    // (B) the Baron falls -> the Jihad opens
    game_debug_open_chosen_path();
    const bool opened = game_debug_endgame_open() == 1;

    // (C) a strike commission is refused with no spice; directed pressure builds toward the pinnacle
    // (pressure applies on a strike's CLEAR — the raid itself is exercised by striketest, so here we
    // drive the data-core press directly).
    const bool broke = game_debug_commission_strike(2, 5) == 0;  // no spice yet
    const int p0 = game_debug_jihad_pressure(2);
    game_debug_press_faction(2, 5);
    const int p1 = game_debug_jihad_pressure(2);
    const bool not_yet = game_debug_pinnacle_available(2) == 0;  // 60 < 100 -> still building
    game_debug_press_faction(2, 5);
    const int p2 = game_debug_jihad_pressure(2);
    const bool pressed = p0 == 0 && p1 == 60 && p2 == 100;
    const bool opens_pinnacle = game_debug_pinnacle_available(2) == 1;

    // (D) defeat the pinnacle: shard taken, that pinnacle now spent
    game_debug_clear_pinnacle(2);
    const bool shard = (game_debug_golden_shards() & (1 << 2)) != 0 &&
                       game_debug_pinnacle_available(2) == 0;

    // (E) the Guild pinnacle breaks the spice monopoly (guild_broken)
    game_debug_clear_pinnacle(4);
    const bool guild_broke = game_debug_guild_broken_flag() == 1;

    // (F) all five shards -> the capstone opens; refusing the Golden Path finishes it
    for (int f = 0; f < 5; ++f)
        if (!(game_debug_golden_shards() & (1 << f))) game_debug_clear_pinnacle(f);
    const bool capstone = game_debug_capstone_available() == 1;
    game_debug_finish_capstone();
    const bool refused = game_debug_capstone_available() == 0;

    // (G) DME8 round-trip: the whole Jihad state survives a meta reload
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_endgame_open() == 1 && game_debug_golden_shards() == 0x1f &&
                           game_debug_jihad_pressure(2) == 100 && game_debug_guild_broken_flag() == 1;

    const bool pass = shut && opened && broke && pressed && not_yet && opens_pinnacle && shard &&
                      guild_broke && capstone && refused && persisted;
    std::printf("endgametest %s (shut %d open %d press %d/%d/%d gate %d shard %d guildbroke %d cap %d refused %d persist %d)\n",
                pass ? "PASS" : "FAIL", shut, opened, p0, p1, p2, opens_pinnacle, shard,
                guild_broke, capstone, refused, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// B10.2: a commissioned Jihad strike drops you into a procedural raid zone (an objective champion +
// escort); clearing the objective presses the target Great Power and melts you home, while bailing
// out the door abandons the strike (no pressure gained). Factions: 3 = House Corrino.
int run_striketest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(31, services);
    game_debug_open_chosen_path();
    game_debug_add_spice(1000);

    int hp, mhp, wa, mwa, lv, sp0; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp0);
    const int ok = game_debug_commission_strike(3 /*Corrino*/, 3);
    int sp1; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp1);
    const bool entered = ok == 1 && game_debug_in_strike() == 1 && game_debug_cur_zone() == 2;
    const bool paid = sp0 - sp1 == 60;                     // strike_cost(3) = 3 * kStrikeSpiceBase
    const bool has_obj = game_debug_strike_boss_count() >= 1;
    const int press_before = game_debug_jihad_pressure(3);

    // clear the objective -> press the target + melt back to the hub
    game_debug_resolve_strike();
    const bool resolved = game_debug_in_strike() == 0 && game_debug_cur_node() == 0 &&
                          game_debug_jihad_pressure(3) == press_before + 36;  // 3 * kStrikePressPerTier

    // abandon path: commission again, then break off via the door -> pressure unchanged
    game_debug_add_spice(1000);
    game_debug_commission_strike(3, 2);
    const bool re_entered = game_debug_in_strike() == 1;
    const int press_mid = game_debug_jihad_pressure(3);
    game_debug_abandon_strike();
    const bool abandoned = game_debug_in_strike() == 0 && game_debug_cur_node() == 0 &&
                           game_debug_jihad_pressure(3) == press_mid;

    const bool pass = entered && paid && has_obj && resolved && re_entered && abandoned;
    std::printf("striketest %s (entered %d paid %d obj %d resolved %d reenter %d abandoned %d)\n",
                pass ? "PASS" : "FAIL", entered, paid, has_obj, resolved, re_entered, abandoned);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// B10.3: a Great Power's homeworld pinnacle opens once its Jihad-pressure is full; assaulting it
// enters a fixed-seed arena with a named boss, and slaying the boss takes that faction's Golden Path
// shard (the Guild's fall also breaks the spice monopoly). Factions: 0 BG, 1 Tleilaxu, 2 Ixian, 4 Guild.
int run_pinnacletest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(88, services);
    game_debug_open_chosen_path();

    // (A) shut until the pressure is full, then it opens
    const bool gated = game_debug_assault_pinnacle(0) == 0;
    game_debug_press_faction(0, 5); game_debug_press_faction(0, 5);  // 120 -> clamped to 100
    const bool avail = game_debug_pinnacle_available(0) == 1;

    // (B) assault the arena — a named boss awaits
    const bool entered = game_debug_assault_pinnacle(0) == 1 && game_debug_in_pinnacle() == 1 &&
                         game_debug_cur_zone() == 2;
    const bool has_boss = game_debug_pinnacle_boss_count() >= 1;

    // (C) slay the boss -> Golden Path shard + home; the pinnacle is now spent
    game_debug_resolve_pinnacle();
    const bool cleared = game_debug_in_pinnacle() == 0 && game_debug_cur_node() == 0 &&
                         (game_debug_golden_shards() & (1 << 0)) != 0 &&
                         game_debug_pinnacle_available(0) == 0;

    // (D) the Guild pinnacle breaks the spice monopoly
    game_debug_press_faction(4, 5); game_debug_press_faction(4, 5);
    game_debug_assault_pinnacle(4);
    const bool in_guild = game_debug_in_pinnacle() == 1;
    game_debug_resolve_pinnacle();
    const bool guild_broke = game_debug_guild_broken_flag() == 1 &&
                             (game_debug_golden_shards() & (1 << 4)) != 0;

    // (E) withdraw: assault faction 1, break off -> no shard, still available
    game_debug_press_faction(1, 5); game_debug_press_faction(1, 5);
    game_debug_assault_pinnacle(1);
    game_debug_abandon_pinnacle();
    const bool abandoned = game_debug_in_pinnacle() == 0 && game_debug_cur_node() == 0 &&
                           (game_debug_golden_shards() & (1 << 1)) == 0 &&
                           game_debug_pinnacle_available(1) == 1;

    // (F) save + load inside a pinnacle restores the arena (the in_pinnacle load-regen branch)
    game_debug_press_faction(2, 5); game_debug_press_faction(2, 5);
    game_debug_assault_pinnacle(2);
    InputState in;
    auto tick1 = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };
    tick1(Key::Save);
    tick1(Key::Load);
    const bool roundtrip = game_debug_in_pinnacle() == 1 && game_debug_cur_zone() == 2 &&
                           game_debug_pinnacle_boss_count() >= 1;
    game_debug_abandon_pinnacle();

    const bool pass = gated && avail && entered && has_boss && cleared && in_guild && guild_broke &&
                      abandoned && roundtrip;
    std::printf("pinnacletest %s (gated %d avail %d enter %d boss %d clear %d guild %d abandon %d saveload %d)\n",
                pass ? "PASS" : "FAIL", gated, avail, entered, has_boss, cleared, guild_broke,
                abandoned, roundtrip);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// B10.4: gathering all five Golden Path shards opens the capstone — the alternate-Paul mirror boss.
// Defeating it refuses the Path (the apex trophy). A fixed-seed arena; one confrontation only.
int run_capstonetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(404, services);
    game_debug_open_chosen_path();

    // (A) shut until all five shards are gathered
    const bool gated = game_debug_confront_capstone() == 0;
    for (int f = 0; f < 5; ++f) game_debug_clear_pinnacle(f);  // take all five shards
    const bool avail = game_debug_capstone_available() == 1;

    // (B) confront: the mirror boss awaits in its arena
    const bool entered = game_debug_confront_capstone() == 1 && game_debug_in_capstone() == 1 &&
                         game_debug_cur_zone() == 2 && game_debug_pinnacle_boss_count() >= 1;

    // (C) save + load inside the capstone restores it (the in_capstone load-regen branch)
    InputState in;
    auto tick1 = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };
    tick1(Key::Save);
    tick1(Key::Load);
    const bool roundtrip = game_debug_in_capstone() == 1 && game_debug_cur_zone() == 2 &&
                           game_debug_pinnacle_boss_count() >= 1;

    // (D) defeat the mirror -> the Path is refused; the capstone is spent
    game_debug_resolve_capstone();
    const bool done = game_debug_in_capstone() == 0 && game_debug_cur_node() == 0 &&
                      game_debug_capstone_done() == 1 && game_debug_capstone_available() == 0;

    const bool pass = gated && avail && entered && roundtrip && done;
    std::printf("capstonetest %s (gated %d avail %d enter %d saveload %d done %d)\n",
                pass ? "PASS" : "FAIL", gated, avail, entered, roundtrip, done);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// The Long Ride: the deep-desert greed gauntlet. Ride the Maker leg by leg building an un-banked
// haul; bank it or push on. Past the buck threshold the Maker may buck & dive (ride over, haul lost).
// Jacurutu is the sole Maker Hide source. Tithe-free, leg-scaled.
int run_longridetest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(1789, services);

    // (A) shut until the Jihad (Chosen Path) is open
    const bool gated = game_debug_ride_begin() == 0;
    game_debug_open_chosen_path();
    const bool began = game_debug_ride_begin() == 1 && game_debug_in_ride() == 1;

    // (B) the first legs (before the buck threshold) never buck; the haul grows
    int ok = 0;
    for (int i = 0; i < 3; ++i) if (game_debug_ride_push() == 0) ++ok;
    const bool safe = ok == 3 && game_debug_ride_leg() == 3;

    // (C) bank: the un-banked haul is secured into spice, the ride ends
    int hp, mhp, wa, mwa, lv, sp0; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp0);
    const int haul = game_debug_ride_haul();
    game_debug_ride_bank();
    int sp1; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp1);
    const bool banked = game_debug_in_ride() == 0 && (sp1 - sp0) == haul && haul > 0;

    // (D) push your luck until the Maker bucks -> ride ends, the un-banked haul is lost
    game_debug_ride_begin();
    bool bucked = false;
    for (int i = 0; i < 200 && !bucked; ++i) if (game_debug_ride_push() == 1) bucked = true;
    const bool buck = bucked && game_debug_in_ride() == 0 && game_debug_ride_haul() == 0;

    // (E) records + Jacurutu accrual, and the meta round-trip. (D3: the Maker Hide counter is migrated
    // to a currency stack on meta load, so we assert the persistent RECORD survives, not the counter.)
    const bool records = game_debug_best_ride_leg() > 0;
    const int hides = game_debug_maker_hides();
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_best_ride_leg() > 0;

    const bool pass = gated && began && safe && banked && buck && records && persisted;
    std::printf("longridetest %s (gated %d began %d safe %d banked %d buck %d best %d hides %d persist %d)\n",
                pass ? "PASS" : "FAIL", gated, began, safe, banked, buck,
                game_debug_best_ride_leg(), hides, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// Landsraad / CHOAM: the Imperial politics layer. CHOAM control is derived from toppled homeworlds
// (conquest) + courted Houses (diplomacy); a Great Power you are actively warring opposes you (and
// reinforces your strikes), a courted one is an ally that pays dividends. Factions 0..4.
int run_choamtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(9, services);
    game_debug_open_chosen_path();
    game_debug_add_spice(1000);

    // (A) baseline: no control, everyone neutral
    const bool base = game_debug_choam_control() == 0 && game_debug_house_stance(0) == 0 &&
                      game_debug_opposing_houses() == 0;

    // (B) warring a Power (pressure >= threshold, un-toppled) makes it an opponent you cannot court
    game_debug_press_faction(2, 5);  // 60 >= kHouseOpposePressure
    const bool opposes = game_debug_house_stance(2) == 2 && game_debug_opposing_houses() == 1;
    const bool cant_court_opponent = game_debug_court_house(2) == 0 && game_debug_house_stance(2) == 2;

    // (C) court a neutral House: it becomes an ally, control rises, spice is paid
    int hp, mhp, wa, mwa, lv, sp0; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp0);
    const int ctrl0 = game_debug_choam_control();
    const bool courted = game_debug_court_house(0) == 1 && game_debug_house_stance(0) == 1;
    int sp1; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp1);
    const bool paid = (sp0 - sp1) == 30;                           // kCourtCost
    const bool control_rose = game_debug_choam_control() == ctrl0 + 6;  // kChoamPerAlly

    // (D) toppling a homeworld seizes a directorship — a bigger jump in control
    const int ctrl1 = game_debug_choam_control();
    game_debug_clear_pinnacle(3);
    const bool conquest = game_debug_choam_control() == ctrl1 + 16;  // kChoamPerPinnacle

    // (E) the CHOAM dividend scales with control
    const int div = game_debug_collect_dividend();
    const bool dividend = div == game_debug_choam_control();       // kDividendPerControl == 1

    // (F) DME10 round-trip: the alliance survives a meta reload
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_house_stance(0) == 1 && game_debug_choam_control() == 22;

    const bool pass = base && opposes && cant_court_opponent && courted && paid && control_rose &&
                      conquest && dividend && persisted;
    std::printf("choamtest %s (base %d oppose %d noCourt %d court %d paid %d rose %d conquest %d div %d persist %d)\n",
                pass ? "PASS" : "FAIL", base, opposes, cant_court_opponent, courted, paid,
                control_rose, conquest, dividend, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// D3a: the currency framework + the "simple" PoE orbs (those that reuse the affix engine). Each orb
// is a stacking currency the bench applies to a gear item. Currency kinds are items::CUR_* (in items.h).
int run_currencytest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(7, services);
    auto grant = [&](int c) { game_debug_grant_currency(c, 5); };
    int rar = 0, naf = 0, frac = 0;

    // (A) the bench refuses an orb you don't hold
    game_debug_setup_gear(0, 0 /*common*/, 8);
    const bool no_cur = game_debug_bench(items::CUR_TRANSMUTE, 0) == 0;

    // (B) Transmutation: Common -> Magic (1 mod); spends one from the stack
    grant(items::CUR_TRANSMUTE);
    const bool transmuted = game_debug_bench(items::CUR_TRANSMUTE, 0) == 1;
    game_debug_item_sig(0, &rar, &naf, &frac);
    const bool is_magic = rar == 1 && naf == 1 && game_debug_currency_count(items::CUR_TRANSMUTE) == 4;

    // (C) Augmentation fills the magic to 2 mods
    grant(items::CUR_AUGMENT);
    const bool augmented = game_debug_bench(items::CUR_AUGMENT, 0) == 1;
    game_debug_item_sig(0, &rar, &naf, nullptr);
    const bool aug_ok = rar == 1 && naf == 2;

    // (D) Regal: Magic -> Rare (+1 mod)
    grant(items::CUR_REGAL);
    game_debug_bench(items::CUR_REGAL, 0);
    game_debug_item_sig(0, &rar, &naf, nullptr);
    const bool regaled = rar == 2 && naf >= 3;

    // (E) Fracture locks one mod; a Chaos reforge then PRESERVES it
    grant(items::CUR_FRACTURE); grant(items::CUR_CHAOS);
    game_debug_bench(items::CUR_FRACTURE, 0);
    game_debug_item_sig(0, nullptr, nullptr, &frac);
    const bool fractured = frac != 0;
    game_debug_bench(items::CUR_CHAOS, 0);
    int frac2 = 0; game_debug_item_sig(0, &rar, &naf, &frac2);
    const bool frac_kept = frac2 != 0 && rar == 2;

    // (F) Divine rerolls values but keeps the affix COUNT
    grant(items::CUR_DIVINE);
    int nb = 0; game_debug_item_sig(0, nullptr, &nb, nullptr);
    game_debug_bench(items::CUR_DIVINE, 0);
    int na = 0; game_debug_item_sig(0, nullptr, &na, nullptr);
    const bool divined = na == nb;

    // (G) Annulment removes one mod
    grant(items::CUR_ANNUL);
    int ab = 0; game_debug_item_sig(0, nullptr, &ab, nullptr);
    game_debug_bench(items::CUR_ANNUL, 0);
    int aa = 0; game_debug_item_sig(0, nullptr, &aa, nullptr);
    const bool annulled = aa == ab - 1;

    // (H) Scouring strips a fresh Rare back to Common (a fractured item would refuse — tested via frac_kept)
    game_debug_setup_gear(1, 2 /*rare*/, 8);
    grant(items::CUR_SCOURING);
    const bool scoured = game_debug_bench(items::CUR_SCOURING, 1) == 1;
    game_debug_item_sig(1, &rar, &naf, nullptr);
    const bool to_common = rar == 0 && naf == 0;

    const bool pass = no_cur && transmuted && is_magic && augmented && aug_ok && regaled &&
                      fractured && frac_kept && divined && annulled && scoured && to_common;
    std::printf("currencytest %s (noCur %d trans %d magic %d aug %d regal %d frac %d fracKept %d divine %d annul %d scour %d common %d)\n",
                pass ? "PASS" : "FAIL", no_cur, transmuted, is_magic, augmented, regaled, fractured,
                frac_kept, divined, annulled, scoured, to_common);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// D3b: the new-mechanic orbs — identify (unID), Blessed (implicit reroll), quality (armour/weapon/
// flask), Vaal (corrupt + lock), Mirror (duplicate). Currency kinds are items::CUR_* (items.h).
int run_currencymechtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(11, services);
    auto grant = [&](int c) { game_debug_grant_currency(c, 5); };

    // (A) Ancient Scroll identifies; while unID, other orbs refuse
    game_debug_setup_gear(0, 2 /*rare*/, 8);
    game_debug_set_unidentified(0, 1);
    grant(items::CUR_CHAOS); grant(items::CUR_WISDOM);
    const bool unid_locks = game_debug_bench(items::CUR_CHAOS, 0) == 0;      // can't craft an unID item
    const bool identified = game_debug_bench(items::CUR_WISDOM, 0) == 1 && (game_debug_item_state(0) & 1) == 0;

    // (B) Osmotic Membrane rerolls the implicit % (0 default -> a rolled value)
    grant(items::CUR_BLESSED);
    const bool imp0 = game_debug_item_implicit(0) == 0;
    const bool blessed = game_debug_bench(items::CUR_BLESSED, 0) == 1 && game_debug_item_implicit(0) != 0;

    // (C) Maker Hide raises ARMOUR quality; Worm Tooth refuses on armour; Worm Tooth raises WEAPON quality
    grant(items::CUR_ARMORQUAL); grant(items::CUR_WEAPONQUAL);
    const bool armor_q = game_debug_bench(items::CUR_ARMORQUAL, 0) == 1 && game_debug_item_quality(0) == 5;
    const bool weap_on_armor = game_debug_bench(items::CUR_WEAPONQUAL, 0) == 0;  // wrong slot
    game_debug_setup_weapon(1, 8);
    const bool weap_q = game_debug_bench(items::CUR_WEAPONQUAL, 1) == 1 && game_debug_item_quality(1) == 5;

    // (D) Distillate Microtubing raises flask quality (targets the belt, not a gear item)
    grant(items::CUR_FLASKQUAL);
    const int fq0 = game_debug_flask_quality();
    const bool flaskq = game_debug_bench(items::CUR_FLASKQUAL, 0) == 1 && game_debug_flask_quality() == fq0 + 5;

    // (E) Unstable Catalyst corrupts + LOCKS the item (further orbs refuse)
    game_debug_setup_gear(2, 2 /*rare*/, 8);
    grant(items::CUR_VAAL); grant(items::CUR_DIVINE);
    const bool vaaled = game_debug_bench(items::CUR_VAAL, 2) == 1 && (game_debug_item_state(2) & 2) != 0;
    const bool locked = game_debug_bench(items::CUR_DIVINE, 2) == 0;  // corrupted -> no more crafting

    // (F) Echo of Shai'Hulud duplicates into a free cell; both copies are mirrored (unmodifiable)
    game_debug_setup_gear(3, 2 /*rare*/, 8);
    grant(items::CUR_MIRROR);
    const int used0 = game_debug_inv_used();
    const bool mirrored = game_debug_bench(items::CUR_MIRROR, 3) == 1;
    const bool copy_made = game_debug_inv_used() == used0 + 1 && (game_debug_item_state(3) & 4) != 0;

    const bool pass = unid_locks && identified && imp0 && blessed && armor_q && weap_on_armor &&
                      weap_q && flaskq && vaaled && locked && mirrored && copy_made;
    std::printf("currencymechtest %s (unidLock %d id %d bless %d armorQ %d weapX %d weapQ %d flaskQ %d vaal %d lock %d mirror %d copy %d)\n",
                pass ? "PASS" : "FAIL", unid_locks, identified, blessed, armor_q, weap_on_armor,
                weap_q, flaskq, vaaled, locked, mirrored, copy_made);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// D3c: PoE area-level-gated currency drops. Shallow areas drop only low-tier currency and kills drop
// none (below the min area); deep areas (an endgame strike here) unlock high-tier drops from kills.
int run_currencydroptest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(50, services);
    game_debug_open_chosen_path();
    game_debug_add_spice(2000);

    // (A) at the hub (area level 1) forced drops are only low-tier (min area level <= 1)
    bool shallow_low = true;
    for (int i = 0; i < 40; ++i) {
        const int k = game_debug_force_currency_drop();
        if (k > 0 && items::kCurrency[k].min_alvl > 1) shallow_low = false;
    }
    // (B) at the hub, kills drop NO currency (area < kDropMinArea)
    const bool shallow_no_drop = game_debug_kill_drops(60) == 0;

    // (C) inside a strike (area level ~15) high-tier currency is eligible; kills DO drop currency
    game_debug_commission_strike(0 /*BG*/, 5);
    bool deep_high = false;
    for (int i = 0; i < 80; ++i) {
        const int k = game_debug_force_currency_drop();
        if (k > 0 && items::kCurrency[k].min_alvl >= 5) deep_high = true;
    }
    const bool deep_drop = game_debug_kill_drops(80) > 0;

    const bool pass = shallow_low && shallow_no_drop && deep_high && deep_drop;
    std::printf("currencydroptest %s (shallowLow %d shallowNoDrop %d deepHigh %d deepDrop %d)\n",
                pass ? "PASS" : "FAIL", shallow_low, shallow_no_drop, deep_high, deep_drop);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// G1: the pack is an 8x12 grid of variable-size items. Chest = 2x3, 1H weapon = 1x3, currency = 1x1;
// items pack first-fit; a move onto an occupied footprint is rejected; a full grid stops accepting.
int run_gridtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(3131, services);

    // (A) footprints
    game_debug_clear_pack();
    const int bcell = game_debug_pack_add_slot(int(items::Slot::Body));
    int bw = 0, bh = 0; game_debug_grid_size_of(0, &bw, &bh);
    const bool a_body = bw == 2 && bh == 3 && bcell == 0;   // chest = 6 cells, first item at cell 0
    game_debug_clear_pack();
    game_debug_pack_add_slot(int(items::Slot::Weapon));
    int ww = 0, wh = 0; game_debug_grid_size_of(0, &ww, &wh);
    const bool a_wpn = ww == 1 && wh == 3;                  // 1H weapon = 3 cells

    // (B) first-fit: two chests sit side by side (cols 0-1, then 2-3), sharing no cell
    game_debug_clear_pack();
    const int c0 = game_debug_pack_add_slot(int(items::Slot::Body));
    const int c1 = game_debug_pack_add_slot(int(items::Slot::Body));
    const bool firstfit = c0 == 0 && c1 == 2 &&
                          game_debug_grid_item_at(0) == 0 && game_debug_grid_item_at(1) == 0 &&
                          game_debug_grid_item_at(2) == 1;

    // (C) collision: chest #1 cannot move onto chest #0; it can move to free cells (cols 4-5)
    const bool block  = game_debug_grid_move(1, 0) == 0;
    const bool freeok = game_debug_grid_move(1, 4) == 1;

    // (D) carried currency is 1x1 in the grid
    game_debug_clear_pack();
    game_debug_grant_currency_grid(items::CUR_TRANSMUTE, 5);
    int cw = 0, ch = 0; game_debug_grid_size_of(0, &cw, &ch);
    const bool cur1x1 = cw == 1 && ch == 1 && game_debug_inv_used() == 1;

    // (E) the 8x12 grid holds exactly 16 chests (16*6 = 96), then rejects the 17th
    game_debug_clear_pack();
    int placed = 0;
    for (int i = 0; i < 20; ++i) if (game_debug_pack_add_slot(int(items::Slot::Body)) >= 0) ++placed;
    const bool full = placed == 16 && game_debug_pack_free_cells() == 0;

    const bool pass = a_body && a_wpn && firstfit && block && freeok && cur1x1 && full;
    std::printf("gridtest %s (body %d wpn %d fit %d block %d free %d cur %d full %d)\n",
                pass ? "PASS" : "FAIL", a_body, a_wpn, firstfit, block, freeok, cur1x1, placed);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// D3d: currency is playable — walk-over pickup collects a ground drop into the pack, and the crafting
// bench (pick a currency, then a target gear item) applies it through the keyboard menu path.
int run_currencyuitest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(88, services);
    InputState in;
    auto key = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };

    // (A) walk-over pickup: a currency dropped on the player's tile lands in the pack GRID (carried)
    game_debug_spawn_currency(items::CUR_TRANSMUTE);
    const int c0 = game_debug_currency_count_grid(items::CUR_TRANSMUTE);
    game_debug_world_tick(1);
    const bool picked = game_debug_currency_count_grid(items::CUR_TRANSMUTE) == c0 + 1;

    // (B) the bench applies currency from the crafting TABLE (its dedicated slots) to a gear item
    game_debug_grant_currency(items::CUR_TRANSMUTE, 3);
    game_debug_setup_gear(5, 0 /*common*/, 8);
    int rar = -1; game_debug_item_sig(5, &rar, nullptr, nullptr);
    const bool starts_common = rar == 0;
    const int held = game_debug_currency_count(items::CUR_TRANSMUTE);
    game_debug_open_menu(11, 0);   // the bench, choosing a currency (only Transmute is held)
    key(Key::Confirm);             // pick the currency
    key(Key::Confirm);             // pick the first gear target -> apply
    game_debug_item_sig(5, &rar, nullptr, nullptr);
    const bool crafted = rar == 1;  // Transmutation: Common -> Magic
    const bool spent = game_debug_currency_count(items::CUR_TRANSMUTE) == held - 1;

    const bool pass = picked && starts_common && crafted && spent;
    std::printf("currencyuitest %s (pickup %d common %d craft %d spent %d)\n",
                pass ? "PASS" : "FAIL", picked, starts_common, crafted, spent);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// G3: the Vault is its own sietch feature — a multi-tab grid stash (each tab bigger than the pack),
// bought with spice at an escalating cost. It is no longer opened through the Naib.
int run_vaulttest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(321, services);  // starts in the sietch

    // (A) the Vault stands in the sietch as its own interactable
    const bool present = game_debug_vault_present() >= 1;

    // (B) fresh: 1 tab; the next costs the base (20); poor -> refused
    const bool start1 = game_debug_stash_tabs() == 1 && game_debug_stash_tab_cost() == 20;
    const bool poor = game_debug_buy_stash_tab() == 0 && game_debug_stash_tabs() == 1;

    // (C) buying escalates (doubling): 20 -> tab2, then 40 -> tab3
    game_debug_add_spice(20);
    const bool buy2 = game_debug_buy_stash_tab() == 1 && game_debug_stash_tabs() == 2 &&
                      game_debug_stash_tab_cost() == 40 && game_debug_buy_stash_tab() == 0;  // spice spent
    game_debug_add_spice(40);
    const bool buy3 = game_debug_buy_stash_tab() == 1 && game_debug_stash_tabs() == 3 &&
                      game_debug_stash_tab_cost() == 80;

    // (D) deposit a pack item into a bought tab (tab 1), freeing its pack cell; then withdraw it back
    game_debug_clear_pack();
    game_debug_setup_gear(0, 2 /*rare*/, 8);
    const bool dep = game_debug_stash_deposit(0, 1) == 1 && game_debug_stash_used(1) == 1 &&
                     game_debug_grid_size_of(0, nullptr, nullptr) == -1;  // pack cell 0 freed
    const int sidx = game_debug_stash_item_at(1, 0);  // top-left cell of the tab holds it
    const bool wd = sidx >= 0 && game_debug_stash_withdraw(1, sidx) == 1 &&
                    game_debug_stash_used(1) == 0 && game_debug_inv_used() == 1;

    // (E) tabs + their contents persist across a meta reload (DMEG round-trip)
    game_debug_setup_gear(0, 1 /*magic*/, 8);
    game_debug_stash_deposit(0, 2);
    int mb = -1, ma = -1; game_debug_meta_reload_class(&mb, &ma);
    const bool persisted = game_debug_stash_tabs() == 3 && game_debug_stash_used(2) == 1;

    const bool pass = present && start1 && poor && buy2 && buy3 && dep && wd && persisted;
    std::printf("vaulttest %s (present %d start1 %d poor %d buy2 %d buy3 %d dep %d wd %d persist %d)\n",
                pass ? "PASS" : "FAIL", present, start1, poor, buy2, buy3, dep, wd, persisted);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// G2: the crafting table is the currency store. Carried currency sits in the pack grid (1 cell each);
// depositing moves a stack into the table's dedicated slots (freeing the cell); the bench applies from
// the table; Ctrl-click quick-moves (currency -> table, gear -> equip). No deposit menus.
int run_currencytabtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(909, services);
    game_debug_clear_pack();

    // (A) deposit: a carried grid stack moves into the table's slots and frees its cell
    game_debug_grant_currency_grid(items::CUR_TRANSMUTE, 7);
    const int used0 = game_debug_inv_used();                          // 1 occupied cell (the stack)
    const bool dep = game_debug_tab_deposit(0) == 1;
    const bool deposited = dep && game_debug_currency_count(items::CUR_TRANSMUTE) == 7 &&
                           game_debug_currency_count_grid(items::CUR_TRANSMUTE) == 0 &&
                           game_debug_inv_used() == used0 - 1;

    // (B) withdraw: pull 3 back into a grid cell
    const bool wd = game_debug_tab_withdraw(items::CUR_TRANSMUTE, 3) == 1;
    const bool withdrawn = wd && game_debug_currency_count(items::CUR_TRANSMUTE) == 4 &&
                           game_debug_currency_count_grid(items::CUR_TRANSMUTE) == 3 &&
                           game_debug_inv_used() == 1;

    // (C) the bench applies from the TABLE (Common -> Magic) and spends a table stack
    game_debug_setup_gear(5, 0 /*common*/, 8);
    int rar = -1; game_debug_item_sig(5, &rar, nullptr, nullptr);
    const bool common0 = rar == 0;
    const int tab0 = game_debug_currency_count(items::CUR_TRANSMUTE);  // 4
    const bool benched = game_debug_bench(items::CUR_TRANSMUTE, 5) == 1;
    game_debug_item_sig(5, &rar, nullptr, nullptr);
    const bool applied = rar == 1 && game_debug_currency_count(items::CUR_TRANSMUTE) == tab0 - 1;

    // (D) Ctrl-click quick-move: a carried currency stack deposits to the table; gear equips (empty
    // Cloak slot -> the pack cell frees)
    game_debug_clear_pack();
    game_debug_grant_currency_grid(items::CUR_ALCHEMY, 2);
    const bool qm_cur = game_debug_quickmove(0) == 1 &&
                        game_debug_currency_count_grid(items::CUR_ALCHEMY) == 0 &&
                        game_debug_currency_count(items::CUR_ALCHEMY) == 2;
    const int ccell = game_debug_pack_add_slot(int(items::Slot::Cloak));
    const int gidx = game_debug_grid_item_at(ccell);
    const int usedb = game_debug_inv_used();
    const bool qm_gear = game_debug_quickmove(gidx) == 1 && game_debug_inv_used() == usedb - 1;

    const bool pass = deposited && withdrawn && common0 && benched && applied && qm_cur && qm_gear;
    std::printf("currencytabtest %s (deposit %d withdraw %d common %d bench %d applied %d qmCur %d qmGear %d)\n",
                pass ? "PASS" : "FAIL", deposited, withdrawn, common0, benched, applied, qm_cur, qm_gear);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// C7: Will-powered prescience abilities. One shared Will pool + cooldown. 1 Voice (stun nearest seen
// foe), 2 Blink (step toward a tile), 3 Prescient Calm (Focus-shield burst). Input/targeting/FX defer.
int run_abilitytest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(55, services);  // start in the sietch: indoor light, no wildlife/escort

    // (A) the Voice stuns the nearest seen foe, spends Will, and starts the shared cooldown
    int shp; game_debug_spawn_shield_adjacent(&shp);
    game_debug_set_will(100);
    const int w0 = game_debug_will();
    const bool voiced = game_debug_cast_ability(1, 0, 0) == 1;
    const bool stunned = game_debug_enemy_stun_turns() > 0;
    const bool will_spent = game_debug_will() == w0 - 6;   // kWillVoice
    const bool on_cd = game_debug_ability_cd() > 0;

    // (B) the shared cooldown blocks a second cast (no Will spent)
    const int w1 = game_debug_will();
    const bool cd_blocks = game_debug_cast_ability(3, 0, 0) == 0 && game_debug_will() == w1;
    game_debug_clear_hostiles();
    game_debug_world_tick(6);
    const bool cd_cleared = game_debug_ability_cd() == 0;

    // (C) Blink moves the player (try each cardinal until one has room)
    bool blinked = false;
    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (int d = 0; d < 4 && !blinked; ++d) {
        game_debug_set_will(100);
        game_debug_world_tick(6);  // clear any cooldown from a prior attempt
        int ax, ay; game_debug_player_pos(&ax, &ay);
        if (game_debug_cast_ability(2, ax + dirs[d][0] * 5, ay + dirs[d][1] * 5) == 1) {
            int bx, by; game_debug_player_pos(&bx, &by);
            blinked = (bx != ax || by != ay);
        }
    }

    // (D) Will gates the cast: too little Will -> refused
    game_debug_world_tick(6);
    game_debug_set_will(2);
    const bool no_will = game_debug_cast_ability(3, 0, 0) == 0;  // Prescience costs 8 > 2

    // (E) Prescient Calm fires and spends its Will
    game_debug_set_will(100);
    game_debug_world_tick(6);
    const int wp0 = game_debug_will();
    const bool presc = game_debug_cast_ability(3, 0, 0) == 1 && game_debug_will() == wp0 - 8;

    const bool pass = voiced && stunned && will_spent && on_cd && cd_blocks && cd_cleared &&
                      blinked && no_will && presc;
    std::printf("abilitytest %s (voice %d stun %d willspent %d cd %d cdblock %d cdclear %d blink %d nowill %d presc %d)\n",
                pass ? "PASS" : "FAIL", voiced, stunned, will_spent, on_cd, cd_blocks, cd_cleared,
                blinked, no_will, presc);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// Flasks: a three-slot belt (0 Life, 1 Will, 2 Utility) with charges that refill from kills + time.
// Each sip spends kFlaskUseCost charges. (Input binding on the number keys is the UI phase.)
int run_flasktest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(88, services);

    // (A) the belt starts full
    const bool full = game_debug_flask_charges(0) == 6 && game_debug_flask_charges(1) == 6 &&
                      game_debug_flask_charges(2) == 6;

    // (B) the Life flask heals and spends its charges
    int hp, mhp, wa, mwa, lv, sp; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp);
    game_debug_hurt(mhp / 3);
    game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp);
    const int c0 = game_debug_flask_charges(0);
    const bool drank = game_debug_use_flask(0) == 1;
    int hp2; game_debug_player_stats(&hp2, &mhp, &wa, &mwa, &lv, &sp);
    const bool healed = hp2 > hp;
    const bool spent = game_debug_flask_charges(0) == c0 - 3;   // kFlaskUseCost

    // (C) too few charges -> the sip is refused
    game_debug_set_flask_charges(0, 2);
    const bool empty_gate = game_debug_use_flask(0) == 0 && game_debug_flask_charges(0) == 2;

    // (D) the Will flask restores Will
    game_debug_set_will(0);
    game_debug_set_flask_charges(1, 6);
    const bool will_drank = game_debug_use_flask(1) == 1 && game_debug_will() > 0;

    // (E) the Utility flask flushes ailments (a Stun here) and wards
    game_debug_stun_player(10);
    game_debug_set_flask_charges(2, 6);
    const bool util_drank = game_debug_use_flask(2) == 1 && game_debug_player_stun_turns() == 0;

    // (F) charges refill (the on-kill / flask_gain mechanism)
    game_debug_set_flask_charges(1, 0);
    game_debug_flask_gain(2);
    const bool recharged = game_debug_flask_charges(1) == 2;

    // (G) a slow time trickle refills over turns
    game_debug_set_flask_charges(1, 0);
    game_debug_world_tick(9);   // crosses a kFlaskRechargeTurns boundary
    const bool trickle = game_debug_flask_charges(1) >= 1;

    const bool pass = full && drank && healed && spent && empty_gate && will_drank && util_drank &&
                      recharged && trickle;
    std::printf("flasktest %s (full %d life[drink %d heal %d spent %d] gate %d will %d util %d recharge %d trickle %d)\n",
                pass ? "PASS" : "FAIL", full, drank, healed, spent, empty_gate, will_drank,
                util_drank, recharged, trickle);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// Input wiring: the flask keys (1/2/3) reach use_flask and the ability keys (Q/E/F) reach
// cast_ability, so the C7 abilities + flasks are actually playable from the keyboard.
int run_inputtest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(123, services);
    InputState in;
    auto frame = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };

    // (A) a flask key heals and spends the flask
    int hp, mhp, wa, mwa, lv, sp; game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp);
    game_debug_hurt(mhp / 3);
    game_debug_player_stats(&hp, &mhp, &wa, &mwa, &lv, &sp);
    const int fc0 = game_debug_flask_charges(0);
    frame(Key::Flask1);
    int hp2; game_debug_player_stats(&hp2, &mhp, &wa, &mwa, &lv, &sp);
    const bool flask_key = hp2 > hp && game_debug_flask_charges(0) == fc0 - 3;

    // (B) an ability key casts (Q = the Voice stuns an adjacent foe)
    int shp; game_debug_spawn_shield_adjacent(&shp);
    game_debug_set_will(100);
    bool ability_key = false;
    for (int i = 0; i < 8 && !ability_key; ++i) {
        frame(Key::Ability1);
        if (game_debug_enemy_stun_turns() > 0) ability_key = true;
    }

    const bool pass = flask_key && ability_key;
    std::printf("inputtest %s (flask_key %d ability_key %d)\n", pass ? "PASS" : "FAIL",
                flask_key, ability_key);
    std::remove("meta.dat");
    return pass ? 0 : 1;
}

// Hub UIs: the War Council table opens the Jihad Table / Landsraad / crafting Bench menus, which
// drive commission_strike / assault_pinnacle / court_house / bench_* through the keyboard menu path.
int run_menutest(audio::Mixer& mixer, const Assets& assets, const Colormaps& cmaps) {
    std::remove("meta.dat");
    GameServices services{&mixer, &assets, &cmaps};
    game_init(21, services);
    game_debug_open_chosen_path();
    game_debug_add_spice(1000);
    InputState in;
    auto key = [&](Key k) { in.pressed[int(k)] = true; game_update(in); in.end_tick(); };

    // (A) the War Council routes to the Jihad Table
    game_debug_open_menu(8, 0);
    key(Key::Confirm);
    const bool to_table = game_debug_menu_open() == 9;

    // (B) confirming a faction row commissions a strike (enters the raid; menu closes)
    game_debug_open_menu(9, 0);
    key(Key::Confirm);
    const bool commissioned = game_debug_in_strike() == 1 && game_debug_menu_open() == 0;
    game_debug_abandon_strike();

    // (C) the Landsraad menu courts a neutral House into an alliance
    game_debug_open_menu(10, 1);
    key(Key::Confirm);
    const bool courted = game_debug_house_stance(1) == 1;  // ALLY

    // (D) the crafting bench opens from the council and Back returns (full currency UI lands in D3d)
    game_debug_open_menu(8, 2 /*Crafting Bench row*/);
    key(Key::Confirm);
    const bool to_bench = game_debug_menu_open() == 11;
    key(Key::Confirm);  // the stub's only row is Back
    const bool bench_back = game_debug_menu_open() == 8;

    const bool pass = to_table && commissioned && courted && to_bench && bench_back;
    std::printf("menutest %s (to_table %d strike %d court %d bench %d back %d)\n",
                pass ? "PASS" : "FAIL", to_table, commissioned, courted, to_bench, bench_back);
    std::remove("meta.dat");
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
         campaigntest = false, dettest = false, animdebug = false,
         circledemo = false, fightdemo = false, rpgtest = false, clicktest = false,
         reveltest = false, gauntlettest = false, extractiontest = false, metatest = false,
         statmodtest = false, affixtest = false, metamigratetest = false, ailmenttest = false,
         rttest = false, stuntest = false, classtest = false, guildtest = false,
         vendortest = false, patroltest = false, cavetest = false, trialtest = false,
         endgametest = false, striketest = false, pinnacletest = false, capstonetest = false,
         longridetest = false, choamtest = false, currencytest = false, currencymechtest = false,
         currencydroptest = false, currencyuitest = false, essencetest = false, gridtest = false, currencytabtest = false, vaulttest = false, abilitytest = false,
         flasktest = false, inputtest = false, menutest = false;
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
        if (SDL_strcmp(argv[i], "--stuntest") == 0) stuntest = true;
        if (SDL_strcmp(argv[i], "--classtest") == 0) classtest = true;
        if (SDL_strcmp(argv[i], "--guildtest") == 0) guildtest = true;
        if (SDL_strcmp(argv[i], "--vendortest") == 0) vendortest = true;
        if (SDL_strcmp(argv[i], "--patroltest") == 0) patroltest = true;
        if (SDL_strcmp(argv[i], "--cavetest") == 0) cavetest = true;
        if (SDL_strcmp(argv[i], "--trialtest") == 0) trialtest = true;
        if (SDL_strcmp(argv[i], "--endgametest") == 0) endgametest = true;
        if (SDL_strcmp(argv[i], "--striketest") == 0) striketest = true;
        if (SDL_strcmp(argv[i], "--pinnacletest") == 0) pinnacletest = true;
        if (SDL_strcmp(argv[i], "--capstonetest") == 0) capstonetest = true;
        if (SDL_strcmp(argv[i], "--longridetest") == 0) longridetest = true;
        if (SDL_strcmp(argv[i], "--choamtest") == 0) choamtest = true;
        if (SDL_strcmp(argv[i], "--currencytest") == 0) currencytest = true;
        if (SDL_strcmp(argv[i], "--currencymechtest") == 0) currencymechtest = true;
        if (SDL_strcmp(argv[i], "--currencydroptest") == 0) currencydroptest = true;
        if (SDL_strcmp(argv[i], "--currencyuitest") == 0) currencyuitest = true;
        if (SDL_strcmp(argv[i], "--essencetest") == 0) essencetest = true;
        if (SDL_strcmp(argv[i], "--gridtest") == 0) gridtest = true;
        if (SDL_strcmp(argv[i], "--currencytabtest") == 0) currencytabtest = true;
        if (SDL_strcmp(argv[i], "--vaulttest") == 0) vaulttest = true;
        if (SDL_strcmp(argv[i], "--abilitytest") == 0) abilitytest = true;
        if (SDL_strcmp(argv[i], "--flasktest") == 0) flasktest = true;
        if (SDL_strcmp(argv[i], "--inputtest") == 0) inputtest = true;
        if (SDL_strcmp(argv[i], "--menutest") == 0) menutest = true;
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
    if (stuntest) return run_stuntest(mixer, assets, cmaps);
    if (classtest) return run_classtest(mixer, assets, cmaps);
    if (guildtest) return run_guildtest(mixer, assets, cmaps);
    if (vendortest) return run_vendortest(mixer, assets, cmaps);
    if (patroltest) return run_patroltest(mixer, assets, cmaps);
    if (cavetest) return run_cavetest(mixer, assets, cmaps);
    if (trialtest) return run_trialtest(mixer, assets, cmaps);
    if (endgametest) return run_endgametest(mixer, assets, cmaps);
    if (striketest) return run_striketest(mixer, assets, cmaps);
    if (pinnacletest) return run_pinnacletest(mixer, assets, cmaps);
    if (capstonetest) return run_capstonetest(mixer, assets, cmaps);
    if (longridetest) return run_longridetest(mixer, assets, cmaps);
    if (choamtest) return run_choamtest(mixer, assets, cmaps);
    if (currencytest) return run_currencytest(mixer, assets, cmaps);
    if (currencymechtest) return run_currencymechtest(mixer, assets, cmaps);
    if (currencydroptest) return run_currencydroptest(mixer, assets, cmaps);
    if (currencyuitest) return run_currencyuitest(mixer, assets, cmaps);
    if (essencetest) return run_essencetest(mixer, assets, cmaps);
    if (gridtest) return run_gridtest(mixer, assets, cmaps);
    if (currencytabtest) return run_currencytabtest(mixer, assets, cmaps);
    if (vaulttest) return run_vaulttest(mixer, assets, cmaps);
    if (abilitytest) return run_abilitytest(mixer, assets, cmaps);
    if (flasktest) return run_flasktest(mixer, assets, cmaps);
    if (inputtest) return run_inputtest(mixer, assets, cmaps);
    if (menutest) return run_menutest(mixer, assets, cmaps);
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
