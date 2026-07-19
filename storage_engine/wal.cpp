#include "wal.h"
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

inline constexpr uint32_t WAL_MAGIC = 0x314C4157; // "WAL1" little-endian
inline constexpr uint32_t WAL_VERSION = 1;
inline constexpr size_t WAL_HEADER_SIZE = 32;

#pragma pack(push, 1)
struct WalFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pageSize;
    uint32_t reserved;
};
#pragma pack(pop)

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

void writeWalFileHeader(int fd) {
    std::vector<uint8_t> buf(WAL_HEADER_SIZE, 0);
    WalFileHeader hdr{WAL_MAGIC, WAL_VERSION, PAGE_SIZE, 0};
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    ssize_t n = ::pwrite(fd, buf.data(), buf.size(), 0);
    if (n != static_cast<ssize_t>(buf.size()))
        throw std::runtime_error("WalManager: failed to write WAL header");
}

void readWalFileHeader(int fd) {
    WalFileHeader hdr{};
    ssize_t n = ::pread(fd, &hdr, sizeof(hdr), 0);
    if (n != static_cast<ssize_t>(sizeof(hdr)))
        throw std::runtime_error("WalManager: failed to read WAL header");
    if (hdr.magic != WAL_MAGIC || hdr.version != WAL_VERSION || hdr.pageSize != PAGE_SIZE)
        throw std::runtime_error("WalManager: invalid WAL file header");
}

} // namespace

WalManager::~WalManager() { close(); }

void WalManager::ensureOpen() const {
    if (fd < 0)
        throw std::runtime_error("WalManager: WAL file is not open");
}

void WalManager::create(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("WalManager: failed to create '" + path + "'");

    writeWalFileHeader(fd);
    writeOffset_ = static_cast<off_t>(WAL_HEADER_SIZE);
    nextLsn_ = 1;
}

void WalManager::open(const std::string& path) {
    close();
    fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("WalManager: failed to open '" + path + "'");

    readWalFileHeader(fd);

    // Recover next LSN from existing records.
    nextLsn_ = 1;
    for (auto& rec : readAllRecords())
        if (rec.lsn >= nextLsn_) nextLsn_ = rec.lsn + 1;

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
    writeOffset_ = 0;
}

LSN WalManager::appendRecord(WalRecordType type, PageId pageId, const Page* page) {
    ensureOpen();

    size_t payloadSize = (type == WalRecordType::PAGE_WRITE) ? (4 + PAGE_SIZE) : 0;
    size_t bodySize = 1 + 8 + payloadSize; // type + lsn + payload
    std::vector<uint8_t> buf(4 + bodySize);

    writeU32LE(buf.data(), static_cast<uint32_t>(bodySize));
    buf[4] = static_cast<uint8_t>(type);
    writeU64LE(buf.data() + 5, nextLsn_);

    if (type == WalRecordType::PAGE_WRITE) {
        writeU32LE(buf.data() + 13, pageId);
        std::memcpy(buf.data() + 17, page->data(), PAGE_SIZE);
    }

    ssize_t n = ::pwrite(fd, buf.data(), buf.size(), writeOffset_);
    if (n != static_cast<ssize_t>(buf.size()))
        throw std::runtime_error("WalManager: write failed");
    writeOffset_ += n;

    return nextLsn_++;
}

LSN WalManager::appendPageWrite(PageId pageId, const Page& page) {
    return appendRecord(WalRecordType::PAGE_WRITE, pageId, &page);
}

LSN WalManager::appendCheckpoint() {
    return appendRecord(WalRecordType::CHECKPOINT, 0, nullptr);
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
        if (bodySize == 0 || pos + bodySize > file.size())
            throw std::runtime_error("WalManager: corrupt WAL record");

        auto type = static_cast<WalRecordType>(file[pos]);
        LSN lsn = readU64LE(file.data() + pos + 1);

        WalRecord rec;
        rec.lsn = lsn;
        rec.type = type;

        if (type == WalRecordType::PAGE_WRITE) {
            if (bodySize < 1 + 8 + 4 + PAGE_SIZE)
                throw std::runtime_error("WalManager: truncated PAGE_WRITE record");
            rec.pageId = readU32LE(file.data() + pos + 9);
            std::memcpy(rec.page.data(), file.data() + pos + 13, PAGE_SIZE);
        } else if (type != WalRecordType::CHECKPOINT) {
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
    nextLsn_ = 1;
    writeOffset_ = static_cast<off_t>(WAL_HEADER_SIZE);
}
