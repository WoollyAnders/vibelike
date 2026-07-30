#pragma once
// Chunked binary save file: header {magic, version, crc32} + TLV chunks.
// Writes are atomic (temp file + rename). Version mismatch = refuse, no
// migration — roguelike saves are short-lived (save-on-quit, delete-on-death).
#include <cstdint>
#include <cstring>
#include <vector>

inline constexpr uint32_t kSaveMagic = 0x54535544;  // 'DUST'
inline constexpr uint16_t kSaveVersion = 21;  // v21: The Long Ride greed-gauntlet state (in_ride)

class SaveWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(uint16_t(v >> 16)); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    // Chunks may nest sequentially (not recursively).
    void begin_chunk(uint32_t tag) { u32(tag); len_pos_ = buf_.size(); u32(0); }
    void end_chunk() {
        const uint32_t len = uint32_t(buf_.size() - len_pos_ - 4);
        std::memcpy(&buf_[len_pos_], &len, 4);
    }

    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
    size_t len_pos_ = 0;
};

class SaveReader {
public:
    explicit SaveReader(std::vector<uint8_t> data) : buf_(std::move(data)) {}

    bool ok() const { return ok_; }
    uint8_t u8() { return pos_ < buf_.size() ? buf_[pos_++] : (ok_ = false, 0); }
    uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | (u8() << 8)); }
    uint32_t u32() { const uint32_t a = u16(); return a | (uint32_t(u16()) << 16); }
    uint64_t u64() { const uint64_t a = u32(); return a | (uint64_t(u32()) << 32); }
    int32_t i32() { return int32_t(u32()); }
    void bytes(void* p, size_t n) {
        if (pos_ + n > buf_.size()) { ok_ = false; return; }
        std::memcpy(p, &buf_[pos_], n);
        pos_ += n;
    }

    // Positions the reader inside the named top-level chunk. Linear scan.
    bool open_chunk(uint32_t tag) {
        size_t p = 0;
        while (p + 8 <= buf_.size()) {
            uint32_t t, len;
            std::memcpy(&t, &buf_[p], 4);
            std::memcpy(&len, &buf_[p + 4], 4);
            if (t == tag) { pos_ = p + 8; return true; }
            p += 8 + len;
        }
        return false;
    }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// File I/O with header + CRC + atomic replace.
bool save_write_file(const char* path, const std::vector<uint8_t>& payload);
bool save_read_file(const char* path, std::vector<uint8_t>& payload_out);

uint32_t crc32(const void* data, size_t n);
