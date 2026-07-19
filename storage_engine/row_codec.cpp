#include "row_codec.h"
#include <cstring>
#include <stdexcept>

namespace RowCodec {

TypeTag typeToTag(Type t) {
    switch (t) {
        case Type::INT:   return TypeTag::INT;
        case Type::FLOAT: return TypeTag::FLOAT;
        case Type::TEXT:  return TypeTag::TEXT;
        case Type::BOOL:  return TypeTag::BOOL;
        case Type::NUL:   return TypeTag::NUL;
        default:
            throw std::runtime_error("RowCodec: cannot encode type " + typeToString(t));
    }
}

Type tagToType(TypeTag tag) {
    switch (tag) {
        case TypeTag::INT:   return Type::INT;
        case TypeTag::FLOAT: return Type::FLOAT;
        case TypeTag::TEXT:  return Type::TEXT;
        case TypeTag::BOOL:  return Type::BOOL;
        case TypeTag::NUL:   return Type::NUL;
    }
    throw std::runtime_error("RowCodec: unknown type tag");
}

static void appendU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

static void appendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

static void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

static void appendI64(std::vector<uint8_t>& out, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; i++)
        out.push_back(static_cast<uint8_t>(u >> (8 * i)));
}

static void appendF64(std::vector<uint8_t>& out, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(v));
    for (int i = 0; i < 8; i++)
        out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

static uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static int64_t readI64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++)
        u |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(u);
}

static double readF64(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++)
        bits |= static_cast<uint64_t>(p[i]) << (8 * i);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

size_t encodeValue(const Value& v, std::vector<uint8_t>& out) {
    size_t start = out.size();
    appendU8(out, static_cast<uint8_t>(typeToTag(v.type)));

    switch (v.type) {
        case Type::INT:
            appendI64(out, std::get<long long>(v.data));
            break;
        case Type::FLOAT:
            appendF64(out, std::get<double>(v.data));
            break;
        case Type::TEXT: {
            const std::string& s = std::get<std::string>(v.data);
            appendU32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
        case Type::BOOL:
            appendU8(out, std::get<bool>(v.data) ? 1 : 0);
            break;
        case Type::NUL:
            break;
        default:
            throw std::runtime_error("RowCodec: cannot encode type " + typeToString(v.type));
    }
    return out.size() - start;
}

size_t decodeValue(const uint8_t* data, size_t len, Value& out) {
    if (len < 1)
        throw std::runtime_error("RowCodec: truncated value (missing type tag)");

    auto tag = static_cast<TypeTag>(data[0]);
    size_t pos = 1;

    switch (tag) {
        case TypeTag::INT:
            if (len < 1 + 8) throw std::runtime_error("RowCodec: truncated INT");
            out = Value::makeInt(readI64(data + pos));
            pos += 8;
            break;
        case TypeTag::FLOAT:
            if (len < 1 + 8) throw std::runtime_error("RowCodec: truncated FLOAT");
            out = Value::makeFloat(readF64(data + pos));
            pos += 8;
            break;
        case TypeTag::TEXT: {
            if (len < 1 + 4) throw std::runtime_error("RowCodec: truncated TEXT length");
            uint32_t slen = readU32(data + pos);
            pos += 4;
            if (len < pos + slen) throw std::runtime_error("RowCodec: truncated TEXT payload");
            out = Value::makeText(std::string(reinterpret_cast<const char*>(data + pos), slen));
            pos += slen;
            break;
        }
        case TypeTag::BOOL:
            if (len < 2) throw std::runtime_error("RowCodec: truncated BOOL");
            out = Value::makeBool(data[pos] != 0);
            pos += 1;
            break;
        case TypeTag::NUL:
            out = Value::makeNull();
            break;
        default:
            throw std::runtime_error("RowCodec: unknown type tag");
    }
    return pos;
}

size_t encodedRowSize(const Row& row) {
    std::vector<uint8_t> tmp;
    encodeRow(row, tmp);
    return tmp.size();
}

void encodeRow(const Row& row, std::vector<uint8_t>& out) {
    if (row.size() > 0xFFFF)
        throw std::runtime_error("RowCodec: too many columns");

    appendU16(out, static_cast<uint16_t>(row.size()));
    for (const Value& v : row)
        encodeValue(v, out);
}

Row decodeRow(const uint8_t* data, size_t len) {
    if (len < 2)
        throw std::runtime_error("RowCodec: truncated row header");

    uint16_t ncols = readU16(data);
    size_t pos = 2;
    Row row;
    row.reserve(ncols);

    for (uint16_t i = 0; i < ncols; i++) {
        if (pos >= len)
            throw std::runtime_error("RowCodec: truncated row body");
        Value v;
        size_t consumed = decodeValue(data + pos, len - pos, v);
        pos += consumed;
        row.push_back(std::move(v));
    }
    return row;
}

} // namespace RowCodec
