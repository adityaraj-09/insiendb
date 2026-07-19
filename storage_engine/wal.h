#pragma once
#include "page.h"
#include "common.h"
#include <cstdint>
#include <string>
#include <vector>

using LSN = uint64_t;

enum class WalRecordType : uint8_t {
    PAGE_WRITE = 1,
    CHECKPOINT = 2,
};

struct WalRecord {
    LSN lsn = 0;
    WalRecordType type = WalRecordType::PAGE_WRITE;
    PageId pageId = 0;
    Page page; // valid for PAGE_WRITE
};

// Append-only write-ahead log (minisql.db-wal).
// Page-level records: log full page image BEFORE it hits the db file.
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
    LSN appendCheckpoint();
    void flush();

    std::vector<WalRecord> readAllRecords() const;

    // Remove all records after a clean checkpoint / successful recovery.
    void truncate();

    LSN nextLsn() const { return nextLsn_; }

private:
    int fd = -1;
    LSN nextLsn_ = 1;
    off_t writeOffset_ = 0;

    void ensureOpen() const;
    LSN appendRecord(WalRecordType type, PageId pageId, const Page* page);
};

inline std::string walPathForDb(const std::string& dbPath) {
    return dbPath + "-wal";
}
