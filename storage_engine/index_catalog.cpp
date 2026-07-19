#include "index_catalog.h"
#include "system_catalog.h"
#include <stdexcept>

Row IndexCatalog::makeIndexRow(const IndexEntry& entry) {
    return {
        Value::makeText(entry.name),
        Value::makeText(entry.tableName),
        Value::makeText(entry.columnName),
        Value::makeInt(static_cast<long long>(entry.rootPage)),
    };
}

std::unordered_map<std::string, IndexEntry> IndexCatalog::load(
    const CatalogBootstrap& boot, const ScanChainFn& scanChain,
    const std::unordered_map<std::string, TableDirectoryEntry>& tables) {

    std::unordered_map<std::string, IndexEntry> result;
    if (boot.indexesRoot == 0) return result;

    scanChain(boot.indexesRoot, [&](const Row& row) {
        if (row.size() != 4 || row[0].type != Type::TEXT || row[1].type != Type::TEXT ||
            row[2].type != Type::TEXT || row[3].type != Type::INT)
            throw std::runtime_error("IndexCatalog: corrupt __indexes row");

        IndexEntry entry;
        entry.name = std::get<std::string>(row[0].data);
        entry.tableName = std::get<std::string>(row[1].data);
        entry.columnName = std::get<std::string>(row[2].data);
        entry.rootPage = static_cast<PageId>(std::get<long long>(row[3].data));

        auto tableIt = tables.find(entry.tableName);
        if (tableIt == tables.end())
            throw std::runtime_error("IndexCatalog: index '" + entry.name +
                                     "' references missing table '" + entry.tableName + "'");

        const ColumnSchema* col = tableIt->second.schema.findColumn(entry.columnName);
        if (!col)
            throw std::runtime_error("IndexCatalog: index '" + entry.name +
                                     "' references missing column '" + entry.columnName + "'");
        if (!IndexKey::isIndexableType(col->type))
            throw std::runtime_error("IndexCatalog: index '" + entry.name +
                                     "' column type is not indexable");

        entry.columnIndex = col->index;
        entry.columnType = col->type;
        result[entry.name] = std::move(entry);
    });

    return result;
}

void IndexCatalog::insertIndex(const IndexEntry& entry, CatalogBootstrap& boot,
                               const AppendRowFn& appendRow) {
    appendRow(boot.indexesRoot, makeIndexRow(entry));
}

PageId IndexCatalog::rewriteIndexesCatalog(
    const std::unordered_map<std::string, IndexEntry>& indexes,
    const SystemCatalog::WriteChainFn& writeChain) {

    std::vector<Row> rows;
    rows.reserve(indexes.size());
    for (const auto& [name, entry] : indexes) {
        (void)name;
        rows.push_back(makeIndexRow(entry));
    }
    return writeChain(rows);
}

bool valueIsIndexable(const Value& v, Type columnType) {
    return IndexKey::valueIsIndexable(v, columnType);
}

void encodeIndexKey(const Value& v, Type columnType, uint8_t* out, size_t keySize) {
    IndexKey::encode(v, columnType, out, keySize);
}
