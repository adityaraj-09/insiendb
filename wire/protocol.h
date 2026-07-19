#pragma once
#include "session.h"
#include "value.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// Postgres-inspired frontend/backend messages (protocol v1).
// Framing: [type: u8][length: i32 BE][payload...]
// length includes itself (4 bytes), excludes the type byte.

namespace Wire {

enum class TypeCode : int32_t {
    INT   = 1,
    FLOAT = 2,
    TEXT  = 3,
    BOOL  = 4,
    NUL   = 5,
};

TypeCode typeCodeFromValue(const Value& v);
TypeCode typeCodeForColumn(const QueryResult& result, size_t colIndex);

struct Message {
    char type = 0;
    std::vector<uint8_t> payload;
};

class Connection {
public:
    explicit Connection(int fd);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void sendReadyForQuery(char status = 'I');
    void sendError(const std::string& message);
    void sendCommandComplete(const std::string& tag);
    void sendRowDescription(const QueryResult& result);
    void sendDataRow(const Row& row);
    void sendSessionResult(const SessionResult& result);

    void sendAuthenticationOk();
    void sendQuery(const std::string& sql);
    void sendTerminate();
    void sendStartup();
    bool waitUntilReady(std::ostream& err);

    // Client: read response messages until ReadyForQuery. Returns false on disconnect.
    bool readQueryResponse(std::ostream& out);

    // Blocks until a typed message arrives. Returns false on EOF / disconnect.
    bool readMessage(Message& out);

    // Read startup packet (no type byte): int32 length + body.
    bool readStartup(std::vector<uint8_t>& body);

    int fd() const { return fd_; }

private:
    int fd_;

    void writeMessage(char type, const std::vector<uint8_t>& payload);
    void writeRaw(const uint8_t* data, size_t len);
    bool readExact(uint8_t* data, size_t len);
};

Connection connectTcp(const std::string& host, uint16_t port);

} // namespace Wire
