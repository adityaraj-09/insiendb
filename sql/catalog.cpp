#include "catalog.h"
#include "storage.h"

const ColumnSchema* TableSchema::findColumn(const std::string& colName) const {
    for (auto& col : columns) {
        if (col.name == colName) return &col;
    }
    return nullptr;
}

bool Catalog::hasTable(const std::string& name) const {
    return tables.find(name) != tables.end();
}

const TableSchema* Catalog::getTable(const std::string& name) const {
    auto it = tables.find(name);
    if (it == tables.end()) return nullptr;
    return &it->second;
}

void Catalog::createTable(const TableSchema& schema) {
    tables[schema.name] = schema;
}

void Catalog::createIndex(const IndexSchema& index) {
    indexes[index.name] = index;
}

const IndexSchema* Catalog::getIndex(const std::string& name) const {
    auto it = indexes.find(name);
    if (it == indexes.end()) return nullptr;
    return &it->second;
}

const IndexSchema* Catalog::findIndexOnColumn(const std::string& table,
                                              const std::string& column) const {
    for (const auto& [name, idx] : indexes) {
        (void)name;
        if (idx.tableName == table && idx.columnName == column)
            return &idx;
    }
    return nullptr;
}

void Catalog::loadFromStorage(const Storage& storage) {
    tables.clear();
    indexes.clear();
    for (const TableSchema& schema : storage.getAllSchemas())
        tables[schema.name] = schema;
    for (const auto& [name, entry] : storage.indexes()) {
        IndexSchema idx;
        idx.name = entry.name;
        idx.tableName = entry.tableName;
        idx.columnName = entry.columnName;
        idx.columnIndex = entry.columnIndex;
        idx.columnType = entry.columnType;
        indexes[name] = idx;
    }
}
