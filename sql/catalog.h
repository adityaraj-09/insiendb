#pragma once
#include "value.h"
#include <string>
#include <vector>
#include <unordered_map>

// A single column's shape: its name, type, and its fixed position within
// the row. `index` matters a lot later — it's how the executor will find
// a column's value in a row (row[index]) without ever comparing strings
// at execution time.
struct ColumnSchema {
    std::string name;
    Type type;
    int index;
};

// A table's full shape: name + ordered columns.
struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;

    // Linear search is fine — insiendb tables have a handful of columns.
    // Returns nullptr if not found.
    const ColumnSchema* findColumn(const std::string& colName) const;
};

struct IndexSchema {
    std::string name;
    std::string tableName;
    std::string columnName;
    int columnIndex = -1;
    Type columnType = Type::UNKNOWN;
};

// The catalog is the engine's single source of truth for "what exists."
// It gets mutated as CREATE TABLE statements are analyzed/executed, and
// read from constantly during analysis of every later statement.
class Catalog {
public:
    bool hasTable(const std::string& name) const;
    const TableSchema* getTable(const std::string& name) const;

    // Registers a new table. Caller (SemanticAnalyzer) is responsible for
    // checking hasTable() first — this will overwrite if called blindly.
    void createTable(const TableSchema& schema);
    void createIndex(const IndexSchema& index);

    const IndexSchema* getIndex(const std::string& name) const;
    const IndexSchema* findIndexOnColumn(const std::string& table,
                                         const std::string& column) const;

    // Restores catalog from disk after Storage::openDatabase.
    void loadFromStorage(const class Storage& storage);

private:
    std::unordered_map<std::string, TableSchema> tables;
    std::unordered_map<std::string, IndexSchema> indexes;
};
