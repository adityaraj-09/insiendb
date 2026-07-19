#pragma once
#include "common.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// Milestone 1: a page is a fixed-size byte buffer — the unit of disk I/O.
// Higher layers (heap pages, catalog) interpret bytes inside this buffer;
// DiskManager only moves whole 8 KiB chunks to/from the file.
class Page {
public:
    Page() { bytes.fill(0); }

    uint8_t* data() { return bytes.data(); }
    const uint8_t* data() const { return bytes.data(); }

    void zero() { bytes.fill(0); }

    // --- little-endian primitive helpers (on-disk format is LE everywhere) ---

    void writeU8(size_t offset, uint8_t v) {
        checkBounds(offset, 1);
        bytes[offset] = v;
    }

    void writeU16(size_t offset, uint16_t v) {
        checkBounds(offset, 2);
        bytes[offset]     = static_cast<uint8_t>(v);
        bytes[offset + 1] = static_cast<uint8_t>(v >> 8);
    }

    void writeU32(size_t offset, uint32_t v) {
        checkBounds(offset, 4);
        bytes[offset]     = static_cast<uint8_t>(v);
        bytes[offset + 1] = static_cast<uint8_t>(v >> 8);
        bytes[offset + 2] = static_cast<uint8_t>(v >> 16);
        bytes[offset + 3] = static_cast<uint8_t>(v >> 24);
    }

    void writeI64(size_t offset, int64_t v) {
        writeU64(offset, static_cast<uint64_t>(v));
    }

    void writeU64(size_t offset, uint64_t v) {
        checkBounds(offset, 8);
        for (int i = 0; i < 8; i++)
            bytes[offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }

    void writeF64(size_t offset, double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(v));
        writeU64(offset, bits);
    }

    uint8_t readU8(size_t offset) const {
        checkBounds(offset, 1);
        return bytes[offset];
    }

    uint16_t readU16(size_t offset) const {
        checkBounds(offset, 2);
        return static_cast<uint16_t>(bytes[offset]) |
               (static_cast<uint16_t>(bytes[offset + 1]) << 8);
    }

    uint32_t readU32(size_t offset) const {
        checkBounds(offset, 4);
        return static_cast<uint32_t>(bytes[offset]) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    }

    int64_t readI64(size_t offset) const {
        return static_cast<int64_t>(readU64(offset));
    }

    uint64_t readU64(size_t offset) const {
        checkBounds(offset, 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
        return v;
    }

    double readF64(size_t offset) const {
        uint64_t bits = readU64(offset);
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    void writeBytes(size_t offset, const uint8_t* src, size_t len) {
        checkBounds(offset, len);
        std::memcpy(bytes.data() + offset, src, len);
    }

    void readBytes(size_t offset, uint8_t* dst, size_t len) const {
        checkBounds(offset, len);
        std::memcpy(dst, bytes.data() + offset, len);
    }

private:
    std::array<uint8_t, PAGE_SIZE> bytes;

    void checkBounds(size_t offset, size_t len) const {
        if (offset + len > PAGE_SIZE)
            throw std::out_of_range("Page access out of bounds");
    }
};
