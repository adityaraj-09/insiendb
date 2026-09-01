#pragma once
#include "page.h"
#include "common.h"
#include <cstdint>
#include <string>
#include <vector>

inline constexpr uint8_t WAL_FLAG_FPI = 0x01;

enum class WalRecordType : uint8_t {
    PAGE_WRITE   = 1,
    CHECKPOINT   = 2,
    HEAP_INIT    = 3,
    HEAP_INSERT  = 4,
    HEAP_DELETE  = 5,
    HEAP_UPDATE  = 6,
    HEAP_SET_NEXT = 7,
};

struct WalSlotRow {
    uint16_t slot = 0;
    std::vector<uint8_t> row;
};

struct WalRecord {
    LSN lsn = 0;
    WalRecordType type = WalRecordType::PAGE_WRITE;
    uint8_t flags = 0;
    PageId pageId = 0;
    Page page; // valid when hasFpi()
    std::vector<uint8_t> payload;

    bool hasFpi() const { return (flags & WAL_FLAG_FPI) != 0; }
};

std::vector<uint8_t> encodeWalSlotRow(uint16_t slot, const uint8_t* row, size_t len);
WalSlotRow decodeWalSlotRow(const std::vector<uint8_t>& payload);
std::vector<uint8_t> encodeWalSlot(uint16_t slot);
uint16_t decodeWalSlot(const std::vector<uint8_t>& payload);
std::vector<uint8_t> encodeWalPageId(PageId id);
PageId decodeWalPageId(const std::vector<uint8_t>& payload);

// Append-only write-ahead log (insien.db-wal), format v2.
// Heap mutations log small redo records. A full page image is attached only
// on the first modification of a heap page after the last checkpoint
// (page.lsn <= redoLsn), matching PostgreSQL full_page_writes.
// B-tree / freelist / page-0 still use PAGE_WRITE (always a full image).
class WalManager {
public:
    WalManager() = default;
    ~WalManager();

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    void create(const std::string& path);
    void open(const std::string& path);
    void close();

    bool isOpen() const { return fd >= 0; }

    LSN appendPageWrite(PageId pageId, const Page& page);
    LSN appendHeap(WalRecordType type, PageId pageId, const Page* fpi,
                   const std::vector<uint8_t>& payload);
    LSN appendCheckpoint();
    void flush();

    std::vector<WalRecord> readAllRecords() const;

    // Remove all records after a clean checkpoint / successful recovery.
    // LSNs keep increasing; redoLsn is persisted in the file header.
    void truncate();

    LSN nextLsn() const { return nextLsn_; }
    LSN redoLsn() const { return redoLsn_; }

private:
    int fd = -1;
    LSN nextLsn_ = 1;
    LSN redoLsn_ = 0;
    off_t writeOffset_ = 0;

    void ensureOpen() const;
    void persistHeader();
    LSN appendRecord(WalRecordType type, PageId pageId, uint8_t flags,
                     const Page* fpi, const uint8_t* payload, size_t payloadLen);
};

inline std::string walPathForDb(const std::string& dbPath) {
    return dbPath + "-wal";
}
