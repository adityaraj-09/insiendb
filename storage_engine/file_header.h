#pragma once
#include "page.h"
#include "common.h"
#include <cstdint>

// Page 0 bootstrap (format version 5).
//
//   [magic: u32][version: u32 (=5)]
//   [__tables root: u32][__columns root: u32][__indexes root: u32]
//   [freelist head: u32]
struct CatalogBootstrap {
    PageId tablesRoot = 0;
    PageId columnsRoot = 0;
    PageId indexesRoot = 0;
    PageId freelistHead = 0;
};

class FileHeader {
public:
    static constexpr uint32_t FORMAT_VERSION = 5;

    static constexpr size_t OFF_MAGIC         = 0;
    static constexpr size_t OFF_VERSION       = 4;
    static constexpr size_t OFF_TABLES_ROOT   = 8;
    static constexpr size_t OFF_COLUMNS_ROOT  = 12;
    static constexpr size_t OFF_INDEXES_ROOT  = 16;
    static constexpr size_t OFF_FREELIST_HEAD = 20;

    static void init(Page& page, const CatalogBootstrap& boot);
    static bool isValid(const Page& page);

    static CatalogBootstrap readBootstrap(const Page& page);
    static void writeBootstrap(Page& page, const CatalogBootstrap& boot);
};
