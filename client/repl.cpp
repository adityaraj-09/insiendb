#include "repl.h"
#include "session.h"
#include "wire/protocol.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

void printResult(const QueryResult& result) {
    if (result.columnNames.empty()) {
        std::cout << "(empty result set)\n";
        return;
    }

    std::vector<size_t> widths(result.columnNames.size(), 4);
    for (size_t i = 0; i < result.columnNames.size(); i++)
        widths[i] = std::max(widths[i], result.columnNames[i].size());

    for (const Row& row : result.rows) {
        for (size_t i = 0; i < row.size(); i++) {
            std::string cell = row[i].toString();
            if (i < widths.size())
                widths[i] = std::max(widths[i], cell.size());
        }
    }

    auto printRow = [&](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); i++) {
            if (i > 0) std::cout << "  ";
            std::cout << std::setw(static_cast<int>(widths[i])) << cells[i];
        }
        std::cout << "\n";
    };

    printRow(result.columnNames);
    for (const Row& row : result.rows) {
        std::vector<std::string> cells;
        cells.reserve(row.size());
        for (const Value& v : row) cells.push_back(v.toString());
        printRow(cells);
    }
    std::cout << "(" << result.rows.size() << " row" << (result.rows.size() == 1 ? "" : "s") << ")\n";
}

void printSessionResult(const SessionResult& result) {
    if (!result.ok) {
        std::cout << "error: " << result.error << "\n";
        return;
    }
    for (const StatementResult& sr : result.statements) {
        if (sr.kind == StatementKind::Select) {
            printResult(sr.query);
        } else {
            std::cout << sr.commandTag << "\n";
        }
    }
}

void printHelp(bool remote) {
    std::cout <<
        "SQL statements (end with ;):\n"
        "  CREATE TABLE ...;\n"
        "  CREATE INDEX idx ON t (col);\n"
        "  INSERT INTO ...;\n"
        "  SELECT ...;\n"
        "  UPDATE ...;\n"
        "  DELETE FROM ...;\n"
        "\n"
        "Dot-commands:\n"
        "  .help              show this message\n"
        "  .quit / .exit       exit\n";
    if (!remote) {
        std::cout <<
        "  .tables            list tables\n"
        "  .schema <table>    show column layout\n"
        "  .info               database file stats\n";
    }
}

void printTables(const Storage& storage) {
    auto schemas = storage.getAllSchemas();
    if (schemas.empty()) {
        std::cout << "(no tables)\n";
        return;
    }
    for (const TableSchema& schema : schemas)
        std::cout << schema.name << "\n";
}

void printSchema(const Catalog& catalog, const std::string& tableName) {
    const TableSchema* schema = catalog.getTable(tableName);
    if (!schema) {
        std::cout << "no such table: " << tableName << "\n";
        return;
    }
    std::cout << "CREATE TABLE " << schema->name << " (\n";
    for (size_t i = 0; i < schema->columns.size(); i++) {
        const ColumnSchema& col = schema->columns[i];
        std::cout << "  " << col.name << " " << typeToString(col.type);
        if (i + 1 < schema->columns.size()) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << ");\n";
}

void printInfo(const Storage& storage) {
    std::cout << "database: " << storage.path() << "\n";
    std::cout << "wal:      " << storage.walPath() << "\n";
    std::cout << "pages:    " << storage.pageCount() << "\n";
    std::cout << "freelist: " << storage.freelistHead() << "\n";
}

bool handleDotCommand(const std::string& line, bool remote,
                      const Catalog& catalog, const Storage& storage) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == ".quit" || cmd == ".exit") return false;
    if (cmd == ".help") { printHelp(remote); return true; }

    if (remote) {
        std::cout << "dot-command not available in remote mode: " << cmd << "\n";
        return true;
    }

    if (cmd == ".tables") { printTables(storage); return true; }
    if (cmd == ".info") { printInfo(storage); return true; }
    if (cmd == ".schema") {
        std::string table;
        iss >> table;
        if (table.empty()) std::cout << "usage: .schema <table>\n";
        else printSchema(catalog, table);
        return true;
    }

    std::cout << "unknown command: " << cmd << " (try .help)\n";
    return true;
}

bool readInputLine(std::string& buffer, std::string& line) {
    std::cout << (buffer.empty() ? "insiendb> " : "      ...> ");
    std::cout.flush();
    if (!std::getline(std::cin, line)) {
        std::cout << "\n";
        return false;
    }
    line = trim(line);
    return true;
}

} // namespace

void runRepl(Storage& storage, Catalog& catalog) {
    Session session(storage);

    std::cout << "insiendb — type .help for commands, .quit to exit\n";
    std::cout << "database: " << storage.path() << " (local)\n\n";

    std::string buffer;
    while (true) {
        std::string line;
        if (!readInputLine(buffer, line)) break;
        if (line.empty()) continue;

        if (buffer.empty() && line[0] == '.') {
            if (!handleDotCommand(line, false, catalog, storage)) break;
            continue;
        }

        if (!buffer.empty()) buffer += ' ';
        buffer += line;
        if (buffer.back() != ';') continue;

        session.refreshCatalog();
        printSessionResult(session.execute(buffer));
        buffer.clear();
    }
}

void runRemoteRepl(const std::string& host, uint16_t port) {
    Wire::Connection conn = Wire::connectTcp(host, port);
    conn.sendStartup();
    if (!conn.waitUntilReady(std::cerr)) {
        std::cerr << "handshake failed\n";
        return;
    }

    std::cout << "insiendb — connected to " << host << ":" << port << " (remote)\n";
    std::cout << "type .help for commands, .quit to exit\n\n";

    std::string buffer;
    while (true) {
        std::string line;
        if (!readInputLine(buffer, line)) break;
        if (line.empty()) continue;

        if (buffer.empty() && line[0] == '.') {
            if (line == ".quit" || line == ".exit") {
                conn.sendTerminate();
                break;
            }
            if (line == ".help") {
                printHelp(true);
                continue;
            }
            std::cout << "dot-command not available in remote mode (use .help)\n";
            continue;
        }

        if (!buffer.empty()) buffer += ' ';
        buffer += line;
        if (buffer.back() != ';') continue;

        conn.sendQuery(buffer);
        if (!conn.readQueryResponse(std::cout)) break;
        buffer.clear();
    }
}
