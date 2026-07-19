#pragma once
#include "executor.h"
#include "catalog.h"
#include "semantic_analyzer.h"
#include "storage.h"
#include <string>
#include <vector>

enum class StatementKind {
    CreateTable,
    CreateIndex,
    Insert,
    Update,
    Delete,
    Select,
};

struct StatementResult {
    StatementKind kind = StatementKind::Select;
    QueryResult query;
    int rowsAffected = 0;
    std::string commandTag;
};

struct SessionResult {
    bool ok = true;
    std::vector<StatementResult> statements;
    std::string error;
    int errorLine = 0;
};

// One connection's SQL pipeline: catalog + analyzer + executor.
class Session {
public:
    explicit Session(Storage& storage);

    void refreshCatalog();
    SessionResult execute(const std::string& sql);

private:
    Storage& storage;
    Catalog catalog;
    SemanticAnalyzer analyzer;
    Executor executor;
};
