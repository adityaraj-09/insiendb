#include "disk_manager.h"
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

DiskManager::~DiskManager() { close(); }

void DiskManager::ensureOpen() const {
    if (fd < 0)
        throw std::runtime_error("DiskManager: no database file is open");
}

off_t DiskManager::pageOffset(PageId pageId) {
    return static_cast<off_t>(pageId) * static_cast<off_t>(PAGE_SIZE);
}

void DiskManager::create(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("DiskManager: failed to create '" + path + "'");

    // Start with one page (page 0). Later this becomes the file header.
    if (ftruncate(fd, static_cast<off_t>(PAGE_SIZE)) != 0) {
        close();
        throw std::runtime_error("DiskManager: ftruncate failed");
    }
    numPages = 1;
}

void DiskManager::open(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("DiskManager: failed to open '" + path + "'");

    struct stat st {};
    if (fstat(fd, &st) != 0) {
        close();
        throw std::runtime_error("DiskManager: fstat failed");
    }
    if (st.st_size % PAGE_SIZE != 0) {
        close();
        throw std::runtime_error("DiskManager: file size is not a multiple of page size");
    }
    numPages = static_cast<PageId>(st.st_size / PAGE_SIZE);
}

void DiskManager::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    numPages = 0;
}

void DiskManager::readPage(PageId pageId, Page& out) const {
    ensureOpen();
    if (pageId >= numPages)
        throw std::out_of_range("DiskManager: readPage page id out of range");

    out.zero();
    ssize_t n = ::pread(fd, out.data(), PAGE_SIZE, pageOffset(pageId));
    if (n < 0)
        throw std::runtime_error("DiskManager: pread failed");
    if (static_cast<size_t>(n) != PAGE_SIZE)
        throw std::runtime_error("DiskManager: short read");
}

void DiskManager::writePage(PageId pageId, const Page& page) {
    ensureOpen();
    if (pageId >= numPages)
        throw std::out_of_range("DiskManager: writePage page id out of range");

    ssize_t n = ::pwrite(fd, page.data(), PAGE_SIZE, pageOffset(pageId));
    if (n < 0)
        throw std::runtime_error("DiskManager: pwrite failed");
    if (static_cast<size_t>(n) != PAGE_SIZE)
        throw std::runtime_error("DiskManager: short write");
}

PageId DiskManager::allocatePage() {
    ensureOpen();
    PageId id = numPages;
    off_t newSize = pageOffset(numPages + 1);
    if (ftruncate(fd, newSize) != 0)
        throw std::runtime_error("DiskManager: ftruncate failed during allocatePage");

    numPages++;
    return id;
}

void DiskManager::growToInclude(PageId pageId) {
    ensureOpen();
    if (pageId < numPages) return;
    off_t newSize = pageOffset(pageId + 1);
    if (ftruncate(fd, newSize) != 0)
        throw std::runtime_error("DiskManager: ftruncate failed during growToInclude");
    numPages = pageId + 1;
}

void DiskManager::fsync() {
    ensureOpen();
    if (::fsync(fd) != 0)
        throw std::runtime_error("DiskManager: fsync failed");
}
