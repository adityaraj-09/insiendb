#pragma once
#include <cstdint>

// Fixed page size for the whole engine. Every on-disk block is exactly this
// many bytes; page_id * PAGE_SIZE is the byte offset in the database file.
inline constexpr uint32_t PAGE_SIZE = 8192;

using PageId = uint32_t;
using LSN = uint64_t;

inline constexpr PageId INVALID_PAGE_ID = 0xFFFFFFFF;
inline constexpr LSN INVALID_LSN = 0;

// Magic numbers identify page kinds when we add catalog / free-list pages later.
inline constexpr uint32_t FILE_HEADER_MAGIC = 0x41444231; // "ADB1"
inline constexpr uint32_t HEAP_PAGE_MAGIC   = 0x48454150; // "HEAP"
inline constexpr uint32_t FREE_PAGE_MAGIC   = 0x46454550; // "FEEP"
inline constexpr uint32_t BTREE_PAGE_MAGIC  = 0x45525442; // "BTRE" (free page)
