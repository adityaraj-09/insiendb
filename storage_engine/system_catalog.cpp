#include "system_catalog.h"
#include "row_codec.h"
#include <algorithm>
#include <stdexcept>

Row SystemCatalog::makeTablesRow(const std::string& name, PageId rootPage) {
    return {Value::makeText(name), Value::makeInt(static_cast<long long>(rootPage))};
}

Row SystemCatalog::makeColumnsRow(const std::string& tableName, int colIndex,
                                   const std::string& colName, Type type) {
    return {
        Value::makeText(tableName),
        Value::makeInt(colIndex),
        Value::makeText(colName),
        Value::makeInt(static_cast<long long>(RowCodec::typeToTag(type))),
    };
}

std::unordered_map<std::string, TableDirectoryEntry> SystemCatalog::load(
    const CatalogBootstrap& boot, const ScanChainFn& scanChain) {

    std::unordered_map<std::string, TableDirectoryEntry> result;
    std::unordered_map<std::string, PageId> roots;

    scanChain(boot.tablesRoot, [&](const Row& row) {
        if (row.size() != 2 || row[0].type != Type::TEXT || row[1].type != Type::INT)
            throw std::runtime_error("SystemCatalog: corrupt __tables row");
        std::string name = std::get<std::string>(row[0].data);
        PageId root = static_cast<PageId>(std::get<long long>(row[1].data));
        roots[name] = root;
    });

    scanChain(boot.columnsRoot, [&](const Row& row) {
        if (row.size() != 4 || row[0].type != Type::TEXT || row[1].type != Type::INT ||
            row[2].type != Type::TEXT || row[3].type != Type::INT)
            throw std::runtime_error("SystemCatalog: corrupt __columns row");

        std::string tableName = std::get<std::string>(row[0].data);
        int colIndex = static_cast<int>(std::get<long long>(row[1].data));
        std::string colName = std::get<std::string>(row[2].data);
        auto typeTag = static_cast<RowCodec::TypeTag>(std::get<long long>(row[3].data));
        Type colType = RowCodec::tagToType(typeTag);

        auto it = result.find(tableName);
        if (it == result.end()) {
            TableDirectoryEntry entry;
            entry.schema.name = tableName;
            entry.firstHeapPage = roots.count(tableName) ? roots.at(tableName) : 0;
            it = result.emplace(tableName, std::move(entry)).first;
        }
        it->second.schema.columns.push_back(ColumnSchema{colName, colType, colIndex});
    });

    for (auto& [name, entry] : result) {
        auto rootIt = roots.find(name);
        if (rootIt == roots.end())
            throw std::runtime_error("SystemCatalog: table '" + name + "' in __columns but not __tables");
        entry.firstHeapPage = rootIt->second;

        std::sort(entry.schema.columns.begin(), entry.schema.columns.end(),
                  [](const ColumnSchema& a, const ColumnSchema& b) { return a.index < b.index; });
    }

    for (auto& [name, root] : roots) {
        if (!result.count(name)) {
            TableDirectoryEntry entry;
            entry.schema.name = name;
            entry.firstHeapPage = root;
            result[name] = std::move(entry);
        }
    }

    return result;
}

void SystemCatalog::insertTable(const TableSchema& schema, PageId firstHeapPage,
                                 CatalogBootstrap& boot, const AppendRowFn& appendRow) {
    appendRow(boot.tablesRoot, makeTablesRow(schema.name, firstHeapPage));
    for (const ColumnSchema& col : schema.columns)
        appendRow(boot.columnsRoot, makeColumnsRow(schema.name, col.index, col.name, col.type));
}

PageId SystemCatalog::rewriteTablesCatalog(
    const std::unordered_map<std::string, TableDirectoryEntry>& tables,
    const WriteChainFn& writeChain) {

    std::vector<Row> rows;
    rows.reserve(tables.size());
    for (auto& [name, entry] : tables)
        rows.push_back(makeTablesRow(name, entry.firstHeapPage));
    return writeChain(rows);
}
