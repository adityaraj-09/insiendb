#include "session.h"
#include "storage.h"
#include "wire/protocol.h"
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

Storage g_storage;
std::mutex g_storageMutex;
volatile sig_atomic_t g_running = 1;

void onSignal(int) { g_running = 0; }

void printUsage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options] [database-file]\n"
        "\n"
        "Options:\n"
        "  --host ADDR    bind address (default 127.0.0.1)\n"
        "  --port PORT    listen port (default 54321)\n"
        "  --new          create a fresh database (fails if file exists)\n"
        "  -h, --help     show this help\n";
}

void handleClient(int clientFd) {
    try {
        Wire::Connection conn(clientFd);

        std::vector<uint8_t> startupBody;
        if (!conn.readStartup(startupBody))
            return;

        conn.sendAuthenticationOk();
        conn.sendReadyForQuery('I');

        Session session(g_storage);
        Wire::Message msg;

        while (g_running && conn.readMessage(msg)) {
            if (msg.type == 'X')
                break;

            if (msg.type != 'Q')
                continue;

            std::string sql;
            if (!msg.payload.empty()) {
                size_t len = msg.payload.size();
                if (msg.payload.back() == 0) len--;
                sql.assign(reinterpret_cast<const char*>(msg.payload.data()), len);
            }

            SessionResult result;
            {
                std::lock_guard<std::mutex> lock(g_storageMutex);
                session.refreshCatalog();
                result = session.execute(sql);
            }

            conn.sendSessionResult(result);
            conn.sendReadyForQuery('I');
        }
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n";
    }

    ::close(clientFd);
}

} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "minisql.db";
    std::string bindHost = "127.0.0.1";
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
            bindHost = argv[++i];
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

    try {
        if (forceNew) {
            if (std::filesystem::exists(dbPath))
                throw std::runtime_error("database already exists: " + dbPath);
            g_storage.createDatabase(dbPath);
        } else if (std::filesystem::exists(dbPath)) {
            g_storage.openDatabase(dbPath);
        } else {
            g_storage.createDatabase(dbPath);
        }
    } catch (const std::exception& e) {
        std::cerr << "failed to open database: " << e.what() << "\n";
        return 1;
    }

    int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bindHost.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind address: " << bindHost << "\n";
        return 1;
    }

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind failed on " << bindHost << ":" << port << "\n";
        return 1;
    }

    if (listen(serverFd, 16) != 0) {
        std::cerr << "listen failed\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "minisql-server listening on " << bindHost << ":" << port
              << "  database: " << g_storage.path() << "\n";

    while (g_running) {
        int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (g_running) std::cerr << "accept failed\n";
            break;
        }
        std::thread(handleClient, clientFd).detach();
    }

    ::close(serverFd);
    g_storage.closeDatabase();
    std::cout << "server stopped\n";
    return 0;
}
