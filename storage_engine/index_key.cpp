#include "index_key.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace IndexKey {

size_t keySizeForType(Type columnType) {
    switch (columnType) {
        case Type::INT:
        case Type::FLOAT:
        case Type::BOOL:
            return KEY_SIZE_NUMERIC;
        case Type::TEXT:
            return KEY_SIZE_TEXT;
        default:
            throw std::runtime_error("IndexKey: type is not indexable");
    }
}

bool isIndexableType(Type columnType) {
    return columnType == Type::INT || columnType == Type::FLOAT ||
           columnType == Type::BOOL || columnType == Type::TEXT;
}

bool valueIsIndexable(const Value& v, Type columnType) {
    if (v.type == Type::NUL) return false;
    if (columnType == Type::INT) return v.type == Type::INT;
    if (columnType == Type::FLOAT) return v.type == Type::FLOAT || v.type == Type::INT;
    if (columnType == Type::BOOL) return v.type == Type::BOOL;
    if (columnType == Type::TEXT) return v.type == Type::TEXT;
    return false;
}

int compareInt(int64_t a, int64_t b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

void encodeInt(int64_t key, uint8_t* out) {
    uint64_t v = static_cast<uint64_t>(key);
    for (int i = 0; i < 8; i++)
        out[i] = static_cast<uint8_t>(v >> (8 * i));
}

int64_t decodeInt(const uint8_t* data) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= static_cast<uint64_t>(data[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

int compareEncoded(const uint8_t* a, const uint8_t* b) {
    return compareInt(decodeInt(a), decodeInt(b));
}

static uint64_t encodeFloatSortable(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(double));
    if (bits & (1ULL << 63))
        bits = ~bits;
    else
        bits |= (1ULL << 63);
    return bits;
}

static void writeU64Le(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; i++)
        out[i] = static_cast<uint8_t>(v >> (8 * i));
}

static uint64_t readU64Le(const uint8_t* data) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= static_cast<uint64_t>(data[i]) << (8 * i);
    return v;
}

void encode(const Value& v, Type columnType, uint8_t* out, size_t keySize) {
    std::memset(out, 0, keySize);

    if (columnType == Type::INT) {
        if (v.type != Type::INT)
            throw std::runtime_error("IndexKey::encode: expected INT value");
        encodeInt(std::get<long long>(v.data), out);
        return;
    }

    if (columnType == Type::BOOL) {
        if (v.type != Type::BOOL)
            throw std::runtime_error("IndexKey::encode: expected BOOL value");
        encodeInt(std::get<bool>(v.data) ? 1 : 0, out);
        return;
    }

    if (columnType == Type::FLOAT) {
        double d = (v.type == Type::FLOAT) ? std::get<double>(v.data)
                                             : static_cast<double>(std::get<long long>(v.data));
        writeU64Le(out, encodeFloatSortable(d));
        return;
    }

    if (columnType == Type::TEXT) {
        if (v.type != Type::TEXT)
            throw std::runtime_error("IndexKey::encode: expected TEXT value");
        const std::string& s = std::get<std::string>(v.data);
        if (keySize < 2)
            throw std::runtime_error("IndexKey::encode: TEXT key too small");
        size_t maxPayload = keySize - 2;
        size_t n = std::min(s.size(), maxPayload);
        out[0] = static_cast<uint8_t>((n >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(n & 0xFF);
        if (n > 0)
            std::memcpy(out + 2, s.data(), n);
        return;
    }

    throw std::runtime_error("IndexKey::encode: unsupported column type");
}

int compare(const uint8_t* a, const uint8_t* b, size_t keySize) {
    for (size_t i = 0; i < keySize; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

bool equal(const uint8_t* a, const uint8_t* b, size_t keySize) {
    return compare(a, b, keySize) == 0;
}

void minKey(Type columnType, uint8_t* out, size_t keySize) {
    std::memset(out, 0, keySize);
    (void)columnType;
}

void maxKey(Type columnType, uint8_t* out, size_t keySize) {
    std::memset(out, 0xFF, keySize);
    if (columnType == Type::TEXT && keySize >= 2) {
        out[0] = 0xFF;
        out[1] = static_cast<uint8_t>(std::min<size_t>(keySize - 2, 0xFF));
    }
}

} // namespace IndexKey
