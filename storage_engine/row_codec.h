#pragma once
#include "value.h"
#include "storage.h"
#include <cstdint>
#include <vector>

// Milestone 3: converts between in-memory Row (vector<Value>) and bytes.
//
// On-disk row layout (little-endian):
//   [num_columns: u16]
//   repeated num_columns times:
//     [type_tag: u8]          -- see TypeTag below
//     [payload]               -- absent for NUL; variable for TEXT
//
// TypeTag values are stable format identifiers (not raw enum ordinals we
// might reorder later).
namespace RowCodec {

enum class TypeTag : uint8_t {
    INT   = 1,
    FLOAT = 2,
    TEXT  = 3,
    BOOL  = 4,
    NUL   = 5,
};

// Returns number of bytes written.
size_t encodeValue(const Value& v, std::vector<uint8_t>& out);

// Returns bytes consumed from data; throws on corrupt input.
size_t decodeValue(const uint8_t* data, size_t len, Value& out);

// Serialize a full row. Throws if row is too large for caller's limit.
void encodeRow(const Row& row, std::vector<uint8_t>& out);

// Deserialize a full row from a byte slice.
Row decodeRow(const uint8_t* data, size_t len);

// Size of encoded row without building the buffer (for space checks).
size_t encodedRowSize(const Row& row);

TypeTag typeToTag(Type t);
Type tagToType(TypeTag tag);

} // namespace RowCodec
