#pragma once
// The audio mixer: 8 two-op FM voices (music + tonal cues), 8 sample channels
// (8-bit-era digitized one-shots), and one parameterized rumble channel
// (filtered noise + low sine) for the sandworm. Game thread talks to the
// audio thread only through push() (lock-free SPSC ring) — render() runs on
// the SDL audio callback.
#include <atomic>
#include <cstdint>

#include "engine/wav.h"

namespace audio {

inline constexpr int kRate = 44100;
inline constexpr int kFmVoices = 8;
inline constexpr int kSampleChans = 8;
inline constexpr int kMaxSamples = 32;
inline constexpr int kMaxInstruments = 16;

// ---- data definitions (authored in tables) ----------------------------------

struct Adsr {
    float attack, decay, sustain, release;  // seconds, seconds, level, seconds
};

struct FmInstrument {
    float car_ratio;   // carrier freq multiple of note freq
    float mod_ratio;   // modulator freq multiple
    float mod_index;   // modulation depth (radians-ish)
    float feedback;    // modulator self-feedback 0..1
    Adsr car_env, mod_env;
    float vol;         // 0..1
};

// A song is a flat, tick-sorted event list. tick 0xFFFF terminates.
struct SongEvent {
    uint16_t tick;
    uint8_t chan;   // FM voice 0..3 reserved for music
    uint8_t note;   // MIDI note; 0 = note off
    uint8_t inst;
    uint8_t vel;    // 0..127
};

struct Song {
    const SongEvent* events;    // tick-sorted, terminated by tick == 0xFFFF
    float ticks_per_second;
    uint16_t length_ticks;      // playhead wraps here...
    uint16_t loop_tick;         // ...back to here
};

// ---- commands (game thread -> audio thread) ---------------------------------

enum class Cmd : uint8_t { PlaySfx, PlaySong, StopSong, Rumble, StopAll };

struct Command {
    Cmd cmd;
    uint8_t a;    // sfx: sample id | song: song id
    uint8_t b;    // sfx: volume 0..255 | rumble: intensity 0..255
    uint8_t c;    // sfx: pitch (128 = 1.0) | rumble: pitch hint
};

// ---- mixer -------------------------------------------------------------------

class Mixer {
public:
    // Registration happens before the device starts; not thread-safe.
    int add_sample(Sample s);                    // returns sample id
    int add_instrument(const FmInstrument& in);  // returns instrument id
    int add_song(const Song& s);                 // returns song id

    bool push(const Command& c);  // any thread; false if ring full

    // Audio-thread only. Renders interleaved stereo s16.
    void render(int16_t* out, int frames);

private:
    struct FmVoice {
        bool active = false;
        bool released = false;
        const FmInstrument* inst = nullptr;
        float freq = 0, vel = 0;
        double car_phase = 0, mod_phase = 0;
        float mod_prev = 0;
        float t = 0, rel_t = 0, rel_level = 0;
    };
    struct SampleChan {
        const Sample* sample = nullptr;
        double pos = 0, step = 0;
        float vol = 0;
    };

    void start_note(int chan, int note, int inst, int vel);
    void stop_note(int chan);
    float fm_sample(FmVoice& v, float dt);

    Sample samples_[kMaxSamples];
    int sample_count_ = 0;
    FmInstrument instruments_[kMaxInstruments];
    int instrument_count_ = 0;
    Song songs_[8];
    int song_count_ = 0;

    // ring buffer
    Command ring_[64];
    std::atomic<uint32_t> head_{0}, tail_{0};

    // audio-thread state
    FmVoice voices_[kFmVoices];
    SampleChan chans_[kSampleChans];
    const Song* song_ = nullptr;
    double song_tick_pos_ = 0;
    int song_event_idx_ = 0;
    float rumble_target_ = 0, rumble_cur_ = 0;
    float rumble_lp_ = 0;
    double rumble_phase_ = 0;
    uint32_t noise_state_ = 0x1234567;
};

}  // namespace audio
