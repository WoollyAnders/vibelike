#pragma once
// Minimal PCM WAV load/save. Load accepts 8-bit unsigned or 16-bit signed,
// mono, any sample rate (playback resamples). Save writes 16-bit mono.
#include <cstdint>
#include <vector>

struct Sample {
    std::vector<int16_t> data;  // mono
    int rate = 0;
};

bool wav_load(const char* path, Sample& out);
bool wav_save(const char* path, const int16_t* interleaved_stereo, int frames, int rate);
