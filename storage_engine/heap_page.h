#pragma once
#include "page.h"
#include "storage.h"
#include <cstdint>
#include <optional>
#include <vector>

// Milestone 2: slotted heap page — stores variable-length serialized rows.
//
// Physical layout inside one 8 KiB page:
//
//   [Header 16 bytes]
//   [Row bytes grow upward from offset 16 ...............]
//   [............... free gap ...........................]
//   [Slot directory grows downward from PAGE_SIZE]
//
// Each slot is 4 bytes: (offset: u16, length: u16). offset == 0 means deleted.
//
//   slot[i] lives at PAGE_SIZE - (i + 1) * 4
//
// This is the classic "slotted page" design: the slot directory and row data
// grow toward each other, so variable-length rows pack efficiently.
class HeapPage {
public:
    static constexpr size_t HEADER_SIZE = 16;
    static constexpr size_t SLOT_SIZE   = 4;

    // Header field offsets
    static constexpr size_t OFF_MAGIC         = 0;  // u32
    static constexpr size_t OFF_NUM_SLOTS     = 4;  // u16
    static constexpr size_t OFF_DATA_END      = 6;  // u16 — next free byte in data area
    static constexpr size_t OFF_NEXT_PAGE     = 8;  // u32 — 0 = end of table's heap chain
    static constexpr size_t OFF_PAGE_LSN      = 12; // u32 — last WAL LSN that modified this page

    // Initialize an empty heap page (call on a zeroed Page buffer).
    static void init(Page& page);

    static bool isHeapPage(const Page& page);

    static PageId nextPage(const Page& page);
    static void setNextPage(Page& page, PageId next);

    static LSN pageLsn(const Page& page);
    static void setPageLsn(Page& page, LSN lsn);

    // Redo helpers used during WAL recovery (idempotent where possible).
    static void applyLoggedInsert(Page& page, uint16_t slotIndex,
                                  const uint8_t* rowBytes, size_t rowLen);

    // Insert serialized row bytes. Returns slot index, or nullopt if full.
    static std::optional<uint16_t> insert(Page& page, const uint8_t* rowBytes, size_t rowLen);

    // Mark slot as deleted (tombstone). Returns false if index invalid or already dead.
    static bool remove(Page& page, uint16_t slotIndex);

    // Update a live slot in place. Same slot index (RID) is preserved.
    // If new bytes fit in the old space, overwrites in place; otherwise appends
    // at the page data end when there is room. Returns false if slot is dead or full.
    static bool updateRow(Page& page, uint16_t slotIndex, const Row& row);
    static bool updateBytes(Page& page, uint16_t slotIndex, const uint8_t* rowBytes, size_t rowLen);

    // Number of slot entries (includes deleted slots).
    static uint16_t slotCount(const Page& page);

    // Live (non-deleted) row count.
    static uint16_t liveRowCount(const Page& page);

    static bool isLiveSlot(const Page& page, uint16_t slotIndex);

    // Read row bytes for a live slot. Throws if slot is dead or out of range.
    static std::vector<uint8_t> getRowBytes(const Page& page, uint16_t slotIndex);

    // Deserialize slot to logical Row.
    static Row getRow(const Page& page, uint16_t slotIndex);

    // Insert a logical Row (encode + insert).
    static std::optional<uint16_t> insertRow(Page& page, const Row& row);

    // Iterate all live slots in index order.
    struct LiveSlot {
        uint16_t index;
        std::vector<uint8_t> bytes;
    };
    static std::vector<LiveSlot> liveSlots(const Page& page);

    // Bytes of free space remaining (conservative estimate).
    static size_t freeSpace(const Page& page);

private:
    static size_t slotOffset(uint16_t slotIndex);
    static void readSlot(const Page& page, uint16_t slotIndex, uint16_t& offset, uint16_t& length);
    static void writeSlot(Page& page, uint16_t slotIndex, uint16_t offset, uint16_t length);
    static size_t slotDirectoryBytes(uint16_t numSlots);
    static bool hasRoom(const Page& page, size_t rowLen, uint16_t numSlots);
};
