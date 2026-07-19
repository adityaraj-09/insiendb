#include "file_header.h"
#include <stdexcept>
#include <string>

void FileHeader::init(Page& page, const CatalogBootstrap& boot) {
    page.zero();
    page.writeU32(OFF_MAGIC, FILE_HEADER_MAGIC);
    page.writeU32(OFF_VERSION, FORMAT_VERSION);
    page.writeU32(OFF_TABLES_ROOT, boot.tablesRoot);
    page.writeU32(OFF_COLUMNS_ROOT, boot.columnsRoot);
    page.writeU32(OFF_INDEXES_ROOT, boot.indexesRoot);
    page.writeU32(OFF_FREELIST_HEAD, boot.freelistHead);
}

static bool supportedVersion(uint32_t version) {
    return version == 4 || version == FileHeader::FORMAT_VERSION;
}

bool FileHeader::isValid(const Page& page) {
    return page.readU32(OFF_MAGIC) == FILE_HEADER_MAGIC &&
           supportedVersion(page.readU32(OFF_VERSION));
}

CatalogBootstrap FileHeader::readBootstrap(const Page& page) {
    if (page.readU32(OFF_MAGIC) != FILE_HEADER_MAGIC)
        throw std::runtime_error("FileHeader: invalid file magic");

    uint32_t version = page.readU32(OFF_VERSION);
    if (!supportedVersion(version))
        throw std::runtime_error("FileHeader: unsupported format version " + std::to_string(version));

    CatalogBootstrap boot;
    boot.tablesRoot = page.readU32(OFF_TABLES_ROOT);
    boot.columnsRoot = page.readU32(OFF_COLUMNS_ROOT);
    if (version >= 5)
        boot.indexesRoot = page.readU32(OFF_INDEXES_ROOT);
    else
        boot.indexesRoot = 0;
    boot.freelistHead = page.readU32(version >= 5 ? OFF_FREELIST_HEAD : 16);
    return boot;
}

void FileHeader::writeBootstrap(Page& page, const CatalogBootstrap& boot) {
    init(page, boot);
}
