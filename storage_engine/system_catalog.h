#pragma once
#include "catalog.h"
#include "value.h"
#include "common.h"
#include "file_header.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using Row = std::vector<Value>;

// SQLite-style system catalog stored as normal heap-page tables on disk.
//
//   __tables  — one row per user table:  (name TEXT, root_page INT)
//   __columns — one row per column:      (table_name TEXT, col_index INT,
//                                         col_name TEXT, type INT)
//
// Both use the same slotted heap pages and RowCodec as user data.
// Page 0 only stores the root page ids for these two catalog heaps.

struct TableDirectoryEntry {
    TableSchema schema;
    PageId firstHeapPage = 0;
};

class SystemCatalog {
public:
    static constexpr const char* TABLES_NAME  = "__tables";
    static constexpr const char* COLUMNS_NAME = "__columns";

    using ScanChainFn  = std::function<void(PageId, const std::function<void(const Row&)>&)>;
    using AppendRowFn  = std::function<void(PageId&, const Row&)>;
    using WriteChainFn = std::function<PageId(const std::vector<Row>&)>;

    static std::unordered_map<std::string, TableDirectoryEntry> load(
        const CatalogBootstrap& boot, const ScanChainFn& scanChain);

    static void insertTable(const TableSchema& schema, PageId firstHeapPage,
                            CatalogBootstrap& boot, const AppendRowFn& appendRow);

    // Rebuilds __tables from the in-memory map. Returns the new root page id.
    static PageId rewriteTablesCatalog(
        const std::unordered_map<std::string, TableDirectoryEntry>& tables,
        const WriteChainFn& writeChain);

    static Row makeTablesRow(const std::string& name, PageId rootPage);
    static Row makeColumnsRow(const std::string& tableName, int colIndex,
                              const std::string& colName, Type type);
};
