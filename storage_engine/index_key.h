#pragma once
#include "value.h"
#include <cstddef>
#include <cstdint>

// Sortable fixed-width index keys. Width depends on column type (see keySizeForType).
namespace IndexKey {

inline constexpr size_t KEY_SIZE_NUMERIC = 8;
inline constexpr size_t KEY_SIZE_TEXT = 64;
inline constexpr size_t MAX_KEY_SIZE = KEY_SIZE_TEXT;

size_t keySizeForType(Type columnType);
bool isIndexableType(Type columnType);
bool valueIsIndexable(const Value& v, Type columnType);

void encode(const Value& v, Type columnType, uint8_t* out, size_t keySize);
int compare(const uint8_t* a, const uint8_t* b, size_t keySize);
bool equal(const uint8_t* a, const uint8_t* b, size_t keySize);

void minKey(Type columnType, uint8_t* out, size_t keySize);
void maxKey(Type columnType, uint8_t* out, size_t keySize);

// Legacy INT helpers (tests / numeric paths).
inline constexpr size_t INT_KEY_SIZE = KEY_SIZE_NUMERIC;
int compareInt(int64_t a, int64_t b);
void encodeInt(int64_t key, uint8_t* out);
int64_t decodeInt(const uint8_t* data);
int compareEncoded(const uint8_t* a, const uint8_t* b);

} // namespace IndexKey
