#include "engine/save.h"

#include <cstdio>

uint32_t crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool save_write_file(const char* path, const std::vector<uint8_t>& payload) {
    char tmp[512];
    std::snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;

    const uint32_t crc = crc32(payload.data(), payload.size());
    uint8_t hdr[10];
    std::memcpy(hdr, &kSaveMagic, 4);
    std::memcpy(hdr + 4, &kSaveVersion, 2);
    std::memcpy(hdr + 6, &crc, 4);
    bool ok = std::fwrite(hdr, 1, 10, f) == 10 &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { std::remove(tmp); return false; }

    std::remove(path);  // Windows rename() fails onto an existing file
    if (std::rename(tmp, path) != 0) { std::remove(tmp); return false; }
    return true;
}

bool save_read_file(const char* path, std::vector<uint8_t>& payload_out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 10) { std::fclose(f); return false; }

    uint8_t hdr[10];
    if (std::fread(hdr, 1, 10, f) != 10) { std::fclose(f); return false; }
    uint32_t magic, crc;
    uint16_t version;
    std::memcpy(&magic, hdr, 4);
    std::memcpy(&version, hdr + 4, 2);
    std::memcpy(&crc, hdr + 6, 4);
    if (magic != kSaveMagic || version != kSaveVersion) { std::fclose(f); return false; }

    payload_out.resize(size_t(size) - 10);
    const bool read_ok =
        std::fread(payload_out.data(), 1, payload_out.size(), f) == payload_out.size();
    std::fclose(f);
    return read_ok && crc32(payload_out.data(), payload_out.size()) == crc;
}
