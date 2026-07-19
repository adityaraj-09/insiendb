#pragma once
#include "catalog.h"
#include "value.h"
#include "common.h"
#include "file_header.h"
#include "system_catalog.h"
#include "btree.h"
#include "index_key.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using Row = std::vector<Value>;

struct IndexEntry {
    std::string name;
    std::string tableName;
    std::string columnName;
    int columnIndex = -1;
    Type columnType = Type::UNKNOWN;
    PageId rootPage = 0;
};

class IndexCatalog {
public:
    static constexpr const char* INDEXES_NAME = "__indexes";

    using ScanChainFn  = std::function<void(PageId, const std::function<void(const Row&)>&)>;
    using AppendRowFn  = std::function<void(PageId&, const Row&)>;

    static std::unordered_map<std::string, IndexEntry> load(
        const CatalogBootstrap& boot, const ScanChainFn& scanChain,
        const std::unordered_map<std::string, TableDirectoryEntry>& tables);

    static void insertIndex(const IndexEntry& entry, CatalogBootstrap& boot,
                            const AppendRowFn& appendRow);

    static PageId rewriteIndexesCatalog(
        const std::unordered_map<std::string, IndexEntry>& indexes,
        const SystemCatalog::WriteChainFn& writeChain);

    static Row makeIndexRow(const IndexEntry& entry);
};

inline constexpr uint16_t BTREE_DEFAULT_MAX_KEYS = 200;

enum class IndexCompareOp { Eq, Ne, Lt, Le, Gt, Ge };

bool valueIsIndexable(const Value& v, Type columnType);
void encodeIndexKey(const Value& v, Type columnType, uint8_t* out, size_t keySize);
