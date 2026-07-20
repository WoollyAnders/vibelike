#include "engine/audio.h"

#include <cmath>

namespace audio {

namespace {
constexpr float kTau = 6.28318530718f;

float midi_freq(int note) { return 440.0f * std::pow(2.0f, (note - 69) / 12.0f); }

// Envelope value at time t (attack/decay/sustain); release handled separately.
float env_at(const Adsr& e, float t) {
    if (t < e.attack) return e.attack > 0 ? t / e.attack : 1.0f;
    t -= e.attack;
    if (t < e.decay) return 1.0f + (e.sustain - 1.0f) * (e.decay > 0 ? t / e.decay : 1.0f);
    return e.sustain;
}
}  // namespace

int Mixer::add_sample(Sample s) {
    if (sample_count_ >= kMaxSamples) return -1;
    samples_[sample_count_] = std::move(s);
    return sample_count_++;
}

int Mixer::add_instrument(const FmInstrument& in) {
    if (instrument_count_ >= kMaxInstruments) return -1;
    instruments_[instrument_count_] = in;
    return instrument_count_++;
}

int Mixer::add_song(const Song& s) {
    if (song_count_ >= 8) return -1;
    songs_[song_count_] = s;
    return song_count_++;
}

bool Mixer::push(const Command& c) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) & 63;
    if (next == tail_.load(std::memory_order_acquire)) return false;
    ring_[h] = c;
    head_.store(next, std::memory_order_release);
    return true;
}

void Mixer::start_note(int chan, int note, int inst, int vel) {
    if (chan < 0 || chan >= kFmVoices || inst < 0 || inst >= instrument_count_) return;
    FmVoice& v = voices_[chan];
    v.active = true;
    v.released = false;
    v.inst = &instruments_[inst];
    v.freq = midi_freq(note);
    v.vel = vel / 127.0f;
    v.car_phase = v.mod_phase = 0;
    v.mod_prev = 0;
    v.t = v.rel_t = 0;
}

void Mixer::stop_note(int chan) {
    if (chan < 0 || chan >= kFmVoices) return;
    FmVoice& v = voices_[chan];
    if (v.active && !v.released) {
        v.released = true;
        v.rel_t = 0;
        v.rel_level = env_at(v.inst->car_env, v.t);
    }
}

float Mixer::fm_sample(FmVoice& v, float dt) {
    const FmInstrument& in = *v.inst;

    float car_env, mod_env;
    if (!v.released) {
        car_env = env_at(in.car_env, v.t);
        mod_env = env_at(in.mod_env, v.t);
    } else {
        const float k = in.car_env.release > 0 ? 1.0f - v.rel_t / in.car_env.release : 0.0f;
        if (k <= 0) { v.active = false; return 0; }
        car_env = v.rel_level * k;
        mod_env = env_at(in.mod_env, v.t) * k;
        v.rel_t += dt;
    }
    v.t += dt;

    const float mod = std::sin(float(v.mod_phase) * kTau + in.feedback * v.mod_prev);
    v.mod_prev = mod;
    const float out = std::sin(float(v.car_phase) * kTau + in.mod_index * mod_env * mod);

    v.car_phase += double(v.freq) * in.car_ratio * dt;
    v.mod_phase += double(v.freq) * in.mod_ratio * dt;
    if (v.car_phase > 1e6) v.car_phase -= 1e6;  // keep precision
    if (v.mod_phase > 1e6) v.mod_phase -= 1e6;

    return out * car_env * v.vel * in.vol;
}

void Mixer::render(int16_t* out, int frames) {
    // Drain commands.
    uint32_t t = tail_.load(std::memory_order_relaxed);
    while (t != head_.load(std::memory_order_acquire)) {
        const Command& c = ring_[t];
        switch (c.cmd) {
            case Cmd::PlaySfx: {
                if (c.a < sample_count_) {
                    // pick a free channel (or steal the oldest-position one)
                    int best = 0;
                    double best_pos = -1;
                    for (int i = 0; i < kSampleChans; ++i) {
                        if (!chans_[i].sample) { best = i; best_pos = 1e18; break; }
                        if (chans_[i].pos > best_pos) { best_pos = chans_[i].pos; best = i; }
                    }
                    SampleChan& ch = chans_[best];
                    ch.sample = &samples_[c.a];
                    ch.pos = 0;
                    ch.step = double(ch.sample->rate) / kRate * (c.c ? c.c / 128.0 : 1.0);
                    ch.vol = c.b / 255.0f;
                }
                break;
            }
            case Cmd::PlaySong:
                if (c.a < song_count_) {
                    song_ = &songs_[c.a];
                    song_tick_pos_ = 0;
                    song_event_idx_ = 0;
                }
                break;
            case Cmd::StopSong:
                song_ = nullptr;
                for (int i = 0; i < 4; ++i) stop_note(i);
                break;
            case Cmd::Rumble:
                rumble_target_ = c.b / 255.0f;
                break;
            case Cmd::StopAll:
                song_ = nullptr;
                for (FmVoice& v : voices_) v.active = false;
                for (SampleChan& ch : chans_) ch.sample = nullptr;
                rumble_target_ = 0;
                break;
        }
        t = (t + 1) & 63;
        tail_.store(t, std::memory_order_release);
    }

    const float dt = 1.0f / kRate;
    for (int i = 0; i < frames; ++i) {
        // Song sequencing: fire due events, wrap at length_ticks -> loop_tick.
        if (song_) {
            song_tick_pos_ += song_->ticks_per_second * dt;
            while (song_->events[song_event_idx_].tick != 0xFFFF &&
                   double(song_->events[song_event_idx_].tick) <= song_tick_pos_) {
                const SongEvent& ev = song_->events[song_event_idx_++];
                if (ev.note == 0) stop_note(ev.chan);
                else start_note(ev.chan, ev.note, ev.inst, ev.vel);
            }
            if (song_tick_pos_ >= song_->length_ticks) {
                song_tick_pos_ -= song_->length_ticks - song_->loop_tick;
                song_event_idx_ = 0;
                while (song_->events[song_event_idx_].tick != 0xFFFF &&
                       song_->events[song_event_idx_].tick < song_->loop_tick)
                    ++song_event_idx_;
            }
        }

        float acc = 0;

        for (FmVoice& v : voices_)
            if (v.active) acc += fm_sample(v, dt);

        for (SampleChan& ch : chans_) {
            if (!ch.sample) continue;
            const size_t idx = size_t(ch.pos);
            if (idx >= ch.sample->data.size()) { ch.sample = nullptr; continue; }
            acc += ch.sample->data[idx] / 32768.0f * ch.vol;
            ch.pos += ch.step;
        }

        // Rumble: smoothed intensity driving lowpassed noise + 38Hz sine.
        rumble_cur_ += (rumble_target_ - rumble_cur_) * 0.00008f;
        if (rumble_cur_ > 0.001f) {
            noise_state_ = noise_state_ * 1664525u + 1013904223u;
            const float white = int32_t(noise_state_) / 2147483648.0f;
            rumble_lp_ += (white - rumble_lp_) * 0.02f;  // ~140Hz lowpass
            const float sine = std::sin(float(rumble_phase_) * kTau);
            rumble_phase_ += 38.0 * dt;
            acc += (rumble_lp_ * 1.2f + sine * 0.5f) * rumble_cur_;
        }

        // Soft clip and write stereo.
        const float clipped = std::tanh(acc * 0.8f);
        const int16_t s = int16_t(clipped * 32000.0f);
        out[i * 2] = s;
        out[i * 2 + 1] = s;
    }
}

}  // namespace audio
