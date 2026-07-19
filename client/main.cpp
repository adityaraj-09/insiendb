#include "repl.h"
#include "storage.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options] [database-file]\n"
        "\n"
        "Options:\n"
        "  --host HOST    connect to insiendb-server (remote REPL)\n"
        "  --port PORT    server port (default 54321)\n"
        "  --new          create a fresh database (local mode only)\n"
        "  -h, --help     show this help\n"
        "\n"
        "Local mode (default): opens database file and runs embedded REPL.\n"
        "Remote mode (--host): sends SQL to insiendb-server over wire protocol.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "insien.db";
    std::string host;
    uint16_t port = 54321;
    bool forceNew = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--new") {
            forceNew = true;
            continue;
        }
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
            continue;
        }
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
        dbPath = arg;
    }

    if (!host.empty()) {
        try {
            runRemoteRepl(host, port);
        } catch (const std::exception& e) {
            std::cerr << "connection failed: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    Catalog catalog;
    Storage storage;

    try {
        if (forceNew) {
            if (std::filesystem::exists(dbPath))
                throw std::runtime_error("database already exists: " + dbPath + " (remove it or pick another path)");
            storage.createDatabase(dbPath);
        } else if (std::filesystem::exists(dbPath)) {
            storage.openDatabase(dbPath);
            catalog.loadFromStorage(storage);
        } else {
            storage.createDatabase(dbPath);
        }
    } catch (const std::exception& e) {
        std::cerr << "failed to open database: " << e.what() << "\n";
        return 1;
    }

    runRepl(storage, catalog);
    storage.closeDatabase();
    return 0;
}
