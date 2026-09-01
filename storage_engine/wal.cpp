#include "wal.h"
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

inline constexpr uint32_t WAL_MAGIC = 0x314C4157; // "WAL1" little-endian
inline constexpr uint32_t WAL_VERSION = 2;
inline constexpr size_t WAL_HEADER_SIZE = 32;
inline constexpr size_t WAL_RECORD_PREFIX = 1 + 8 + 1 + 4; // type + lsn + flags + page_id

inline void writeU32LE(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

inline void writeU64LE(uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; i++)
        dst[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint32_t readU32LE(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline uint64_t readU64LE(const uint8_t* src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= static_cast<uint64_t>(src[i]) << (8 * i);
    return v;
}

inline uint16_t readU16LE(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

inline void writeU16LE(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
}

void writeWalFileHeader(int fd, LSN nextLsn, LSN redoLsn) {
    uint8_t buf[WAL_HEADER_SIZE] = {};
    writeU32LE(buf + 0, WAL_MAGIC);
    writeU32LE(buf + 4, WAL_VERSION);
    writeU32LE(buf + 8, PAGE_SIZE);
    writeU32LE(buf + 12, 0);
    writeU64LE(buf + 16, nextLsn);
    writeU64LE(buf + 24, redoLsn);
    ssize_t n = ::pwrite(fd, buf, WAL_HEADER_SIZE, 0);
    if (n != static_cast<ssize_t>(WAL_HEADER_SIZE))
        throw std::runtime_error("WalManager: failed to write WAL header");
}

void readWalFileHeader(int fd, LSN& nextLsn, LSN& redoLsn) {
    uint8_t buf[WAL_HEADER_SIZE] = {};
    ssize_t n = ::pread(fd, buf, WAL_HEADER_SIZE, 0);
    if (n != static_cast<ssize_t>(WAL_HEADER_SIZE))
        throw std::runtime_error("WalManager: failed to read WAL header");
    uint32_t magic = readU32LE(buf + 0);
    uint32_t version = readU32LE(buf + 4);
    uint32_t pageSize = readU32LE(buf + 8);
    if (magic != WAL_MAGIC || version != WAL_VERSION || pageSize != PAGE_SIZE)
        throw std::runtime_error("WalManager: invalid WAL file header");
    nextLsn = readU64LE(buf + 16);
    redoLsn = readU64LE(buf + 24);
    if (nextLsn == 0)
        nextLsn = 1;
}

bool isHeapRecord(WalRecordType type) {
    return type == WalRecordType::HEAP_INIT ||
           type == WalRecordType::HEAP_INSERT ||
           type == WalRecordType::HEAP_DELETE ||
           type == WalRecordType::HEAP_UPDATE ||
           type == WalRecordType::HEAP_SET_NEXT;
}

} // namespace

std::vector<uint8_t> encodeWalSlotRow(uint16_t slot, const uint8_t* row, size_t len) {
    if (len > 0xFFFF)
        throw std::runtime_error("WalManager: row too large for heap WAL record");
    std::vector<uint8_t> out(4 + len);
    writeU16LE(out.data(), slot);
    writeU16LE(out.data() + 2, static_cast<uint16_t>(len));
    if (len > 0)
        std::memcpy(out.data() + 4, row, len);
    return out;
}

WalSlotRow decodeWalSlotRow(const std::vector<uint8_t>& payload) {
    if (payload.size() < 4)
        throw std::runtime_error("WalManager: truncated heap row payload");
    WalSlotRow out;
    out.slot = readU16LE(payload.data());
    uint16_t len = readU16LE(payload.data() + 2);
    if (payload.size() < static_cast<size_t>(4 + len))
        throw std::runtime_error("WalManager: truncated heap row bytes");
    out.row.assign(payload.begin() + 4, payload.begin() + 4 + len);
    return out;
}

std::vector<uint8_t> encodeWalSlot(uint16_t slot) {
    std::vector<uint8_t> out(2);
    writeU16LE(out.data(), slot);
    return out;
}

uint16_t decodeWalSlot(const std::vector<uint8_t>& payload) {
    if (payload.size() < 2)
        throw std::runtime_error("WalManager: truncated heap slot payload");
    return readU16LE(payload.data());
}

std::vector<uint8_t> encodeWalPageId(PageId id) {
    std::vector<uint8_t> out(4);
    writeU32LE(out.data(), id);
    return out;
}

PageId decodeWalPageId(const std::vector<uint8_t>& payload) {
    if (payload.size() < 4)
        throw std::runtime_error("WalManager: truncated page id payload");
    return readU32LE(payload.data());
}

WalManager::~WalManager() { close(); }

void WalManager::ensureOpen() const {
    if (fd < 0)
        throw std::runtime_error("WalManager: WAL file is not open");
}

void WalManager::persistHeader() {
    ensureOpen();
    writeWalFileHeader(fd, nextLsn_, redoLsn_);
}

void WalManager::create(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("WalManager: failed to create '" + path + "'");

    nextLsn_ = 1;
    redoLsn_ = 0;
    persistHeader();
    writeOffset_ = static_cast<off_t>(WAL_HEADER_SIZE);
}

void WalManager::open(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("WalManager: failed to open '" + path + "'");

    readWalFileHeader(fd, nextLsn_, redoLsn_);

    for (auto& rec : readAllRecords()) {
        if (rec.lsn >= nextLsn_)
            nextLsn_ = rec.lsn + 1;
        if (rec.type == WalRecordType::CHECKPOINT)
            redoLsn_ = rec.lsn;
    }

    struct stat st {};
    if (fstat(fd, &st) != 0)
        throw std::runtime_error("WalManager: fstat failed");
    writeOffset_ = st.st_size;
}

void WalManager::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    nextLsn_ = 1;
    redoLsn_ = 0;
    writeOffset_ = 0;
}

LSN WalManager::appendRecord(WalRecordType type, PageId pageId, uint8_t flags,
                             const Page* fpi, const uint8_t* payload, size_t payloadLen) {
    ensureOpen();

    if (fpi)
        flags |= WAL_FLAG_FPI;
    if ((flags & WAL_FLAG_FPI) && !fpi)
        throw std::runtime_error("WalManager: FPI flag set without page image");

    size_t extra = 0;
    if (flags & WAL_FLAG_FPI)
        extra += PAGE_SIZE;
    extra += payloadLen;

    size_t bodySize = WAL_RECORD_PREFIX + extra;
    std::vector<uint8_t> buf(4 + bodySize);

    writeU32LE(buf.data(), static_cast<uint32_t>(bodySize));
    buf[4] = static_cast<uint8_t>(type);
    writeU64LE(buf.data() + 5, nextLsn_);
    buf[13] = flags;
    writeU32LE(buf.data() + 14, pageId);

    size_t pos = 18;
    if (flags & WAL_FLAG_FPI) {
        std::memcpy(buf.data() + pos, fpi->data(), PAGE_SIZE);
        pos += PAGE_SIZE;
    }
    if (payloadLen > 0)
        std::memcpy(buf.data() + pos, payload, payloadLen);

    ssize_t n = ::pwrite(fd, buf.data(), buf.size(), writeOffset_);
    if (n != static_cast<ssize_t>(buf.size()))
        throw std::runtime_error("WalManager: write failed");
    writeOffset_ += n;

    return nextLsn_++;
}

LSN WalManager::appendPageWrite(PageId pageId, const Page& page) {
    return appendRecord(WalRecordType::PAGE_WRITE, pageId, WAL_FLAG_FPI, &page, nullptr, 0);
}

LSN WalManager::appendHeap(WalRecordType type, PageId pageId, const Page* fpi,
                           const std::vector<uint8_t>& payload) {
    if (!isHeapRecord(type))
        throw std::runtime_error("WalManager: appendHeap called with non-heap type");
    return appendRecord(type, pageId, 0, fpi, payload.empty() ? nullptr : payload.data(),
                        payload.size());
}

LSN WalManager::appendCheckpoint() {
    LSN lsn = appendRecord(WalRecordType::CHECKPOINT, 0, 0, nullptr, nullptr, 0);
    redoLsn_ = lsn;
    return lsn;
}

void WalManager::flush() {
    ensureOpen();
    if (::fsync(fd) != 0)
        throw std::runtime_error("WalManager: fsync failed");
}

std::vector<WalRecord> WalManager::readAllRecords() const {
    ensureOpen();

    struct stat st {};
    if (fstat(fd, &st) != 0)
        throw std::runtime_error("WalManager: fstat failed");

    std::vector<WalRecord> records;
    if (static_cast<size_t>(st.st_size) <= WAL_HEADER_SIZE)
        return records;

    std::vector<uint8_t> file(static_cast<size_t>(st.st_size));
    ssize_t n = ::pread(fd, file.data(), file.size(), 0);
    if (n != static_cast<ssize_t>(file.size()))
        throw std::runtime_error("WalManager: read failed");

    size_t pos = WAL_HEADER_SIZE;
    while (pos + 4 <= file.size()) {
        uint32_t bodySize = readU32LE(file.data() + pos);
        pos += 4;
        if (bodySize < WAL_RECORD_PREFIX || pos + bodySize > file.size())
            throw std::runtime_error("WalManager: corrupt WAL record");

        auto type = static_cast<WalRecordType>(file[pos]);
        LSN lsn = readU64LE(file.data() + pos + 1);
        uint8_t flags = file[pos + 9];
        PageId pageId = readU32LE(file.data() + pos + 10);

        WalRecord rec;
        rec.lsn = lsn;
        rec.type = type;
        rec.flags = flags;
        rec.pageId = pageId;

        size_t cursor = pos + WAL_RECORD_PREFIX;
        size_t end = pos + bodySize;
        if (flags & WAL_FLAG_FPI) {
            if (cursor + PAGE_SIZE > end)
                throw std::runtime_error("WalManager: truncated full-page image");
            std::memcpy(rec.page.data(), file.data() + cursor, PAGE_SIZE);
            cursor += PAGE_SIZE;
        }
        rec.payload.assign(file.begin() + static_cast<std::ptrdiff_t>(cursor),
                           file.begin() + static_cast<std::ptrdiff_t>(end));

        if (type == WalRecordType::PAGE_WRITE) {
            if (!(flags & WAL_FLAG_FPI))
                throw std::runtime_error("WalManager: PAGE_WRITE missing page image");
        } else if (type == WalRecordType::CHECKPOINT) {
            // no payload
        } else if (!isHeapRecord(type)) {
            throw std::runtime_error("WalManager: unknown record type");
        }

        records.push_back(std::move(rec));
        pos += bodySize;
    }
    return records;
}

void WalManager::truncate() {
    ensureOpen();
    if (ftruncate(fd, static_cast<off_t>(WAL_HEADER_SIZE)) != 0)
        throw std::runtime_error("WalManager: truncate failed");
    writeOffset_ = static_cast<off_t>(WAL_HEADER_SIZE);
    persistHeader();
}
