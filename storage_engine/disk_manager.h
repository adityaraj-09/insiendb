#pragma once
#include "page.h"
#include <cstdint>
#include <string>

// Milestone 1: maps page IDs to file offsets and performs whole-page I/O.
// page_id N lives at byte offset N * PAGE_SIZE in the database file.
class DiskManager {
public:
    DiskManager() = default;
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Create a new database file (truncates if it exists) with page 0 reserved
    // as an (empty for now) file header.
    void create(const std::string& path);

    // Open an existing database file for read/write.
    void open(const std::string& path);

    void close();

    bool isOpen() const { return fd >= 0; }

    // Read/write one full page. Throws on I/O error or short read/write.
    void readPage(PageId pageId, Page& out) const;
    void writePage(PageId pageId, const Page& page);

    // Grow the file by one page and return the new page's id (contents undefined until written).
    PageId allocatePage();

    // Extend the file so pageId is valid for read/write.
    void growToInclude(PageId pageId);

    void fsync();

    // Number of pages currently in the file (including page 0).
    PageId pageCount() const { return numPages; }

private:
    int fd = -1;
    PageId numPages = 0;

    void ensureOpen() const;
    static off_t pageOffset(PageId pageId);
};
