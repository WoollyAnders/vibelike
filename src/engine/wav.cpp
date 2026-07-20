#include "engine/wav.h"

#include <cstdio>
#include <cstring>

namespace {
uint32_t rd32(const uint8_t* p) {
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
}  // namespace

bool wav_load(const char* path, Sample& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    if (std::fread(buf.data(), 1, size_t(size), f) != size_t(size)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    if (size < 44 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4))
        return false;

    int channels = 0, bits = 0;
    out.rate = 0;
    size_t pos = 12;
    while (pos + 8 <= size_t(size)) {
        const uint32_t chunk_size = rd32(&buf[pos + 4]);
        if (!std::memcmp(&buf[pos], "fmt ", 4)) {
            channels = rd16(&buf[pos + 10]);
            out.rate = int(rd32(&buf[pos + 12]));
            bits = rd16(&buf[pos + 22]);
        } else if (!std::memcmp(&buf[pos], "data", 4)) {
            if (channels != 1 || out.rate == 0) return false;
            const uint8_t* d = &buf[pos + 8];
            const size_t n = chunk_size;
            if (bits == 8) {
                out.data.resize(n);
                for (size_t i = 0; i < n; ++i)
                    out.data[i] = int16_t((int(d[i]) - 128) << 8);
            } else if (bits == 16) {
                out.data.resize(n / 2);
                for (size_t i = 0; i < n / 2; ++i)
                    out.data[i] = int16_t(rd16(d + i * 2));
            } else {
                return false;
            }
            return true;
        }
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    return false;
}

bool wav_save(const char* path, const int16_t* interleaved_stereo, int frames, int rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t data_bytes = uint32_t(frames) * 4;
    const uint32_t riff = 36 + data_bytes;
    uint8_t hdr[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                       16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0,
                       'd','a','t','a', 0,0,0,0};
    auto wr32 = [&](int off, uint32_t v) {
        hdr[off] = v & 255; hdr[off+1] = (v >> 8) & 255;
        hdr[off+2] = (v >> 16) & 255; hdr[off+3] = (v >> 24) & 255;
    };
    wr32(4, riff);
    wr32(24, uint32_t(rate));
    wr32(28, uint32_t(rate) * 4);
    wr32(40, data_bytes);
    std::fwrite(hdr, 1, 44, f);
    std::fwrite(interleaved_stereo, 2, size_t(frames) * 2, f);
    std::fclose(f);
    return true;
}
