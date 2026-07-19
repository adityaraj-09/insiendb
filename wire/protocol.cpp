#include "protocol.h"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <netdb.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

inline void appendInt16BE(std::vector<uint8_t>& buf, int16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void appendInt32BE(std::vector<uint8_t>& buf, int32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline int32_t readInt32BE(const uint8_t* p) {
    return (static_cast<int32_t>(p[0]) << 24) |
           (static_cast<int32_t>(p[1]) << 16) |
           (static_cast<int32_t>(p[2]) << 8) |
           static_cast<int32_t>(p[3]);
}

inline int16_t readInt16BE(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void appendString(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

std::vector<uint8_t> encodeFieldText(const Value& v) {
    std::string text = v.toString();
    std::vector<uint8_t> out;
    appendInt32BE(out, static_cast<int32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

} // namespace

namespace Wire {

TypeCode typeCodeFromValue(const Value& v) {
    switch (v.type) {
        case Type::INT: return TypeCode::INT;
        case Type::FLOAT: return TypeCode::FLOAT;
        case Type::TEXT: return TypeCode::TEXT;
        case Type::BOOL: return TypeCode::BOOL;
        case Type::NUL: return TypeCode::NUL;
        default: return TypeCode::TEXT;
    }
}

TypeCode typeCodeForColumn(const QueryResult& result, size_t colIndex) {
    for (const Row& row : result.rows) {
        if (colIndex < row.size())
            return typeCodeFromValue(row[colIndex]);
    }
    return TypeCode::TEXT;
}

Connection::Connection(int fd) : fd_(fd) {}

bool Connection::readExact(uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd_, data + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

void Connection::writeRaw(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd_, data + sent, len - sent);
        if (n <= 0)
            throw std::runtime_error("Wire: write failed");
        sent += static_cast<size_t>(n);
    }
}

void Connection::writeMessage(char type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> header;
    header.push_back(static_cast<uint8_t>(type));
    appendInt32BE(header, static_cast<int32_t>(4 + payload.size()));
    writeRaw(header.data(), header.size());
    if (!payload.empty())
        writeRaw(payload.data(), payload.size());
}

bool Connection::readStartup(std::vector<uint8_t>& body) {
    uint8_t lenBuf[4];
    if (!readExact(lenBuf, 4)) return false;
    int32_t len = readInt32BE(lenBuf);
    if (len < 4) return false;
    body.assign(static_cast<size_t>(len - 4), 0);
    if (!body.empty() && !readExact(body.data(), body.size())) return false;
    return true;
}

bool Connection::readMessage(Message& out) {
    uint8_t typeByte;
    if (!readExact(&typeByte, 1)) return false;

    uint8_t lenBuf[4];
    if (!readExact(lenBuf, 4)) return false;
    int32_t len = readInt32BE(lenBuf);
    if (len < 4) return false;

    out.type = static_cast<char>(typeByte);
    out.payload.assign(static_cast<size_t>(len - 4), 0);
    if (!out.payload.empty() && !readExact(out.payload.data(), out.payload.size())) return false;
    return true;
}

void Connection::sendReadyForQuery(char status) {
    std::vector<uint8_t> payload = {static_cast<uint8_t>(status)};
    writeMessage('Z', payload);
}

void Connection::sendError(const std::string& message) {
    std::vector<uint8_t> payload;
    appendString(payload, message);
    writeMessage('E', payload);
}

void Connection::sendCommandComplete(const std::string& tag) {
    std::vector<uint8_t> payload;
    appendString(payload, tag);
    writeMessage('C', payload);
}

void Connection::sendRowDescription(const QueryResult& result) {
    std::vector<uint8_t> payload;
    appendInt16BE(payload, static_cast<int16_t>(result.columnNames.size()));
    for (size_t i = 0; i < result.columnNames.size(); i++) {
        const std::string& name = result.columnNames[i];
        appendInt16BE(payload, static_cast<int16_t>(name.size()));
        payload.insert(payload.end(), name.begin(), name.end());
        appendInt32BE(payload, static_cast<int32_t>(typeCodeForColumn(result, i)));
    }
    writeMessage('T', payload);
}

void Connection::sendDataRow(const Row& row) {
    std::vector<uint8_t> payload;
    appendInt16BE(payload, static_cast<int16_t>(row.size()));
    for (const Value& v : row) {
        if (v.type == Type::NUL) {
            appendInt32BE(payload, -1);
        } else {
            auto field = encodeFieldText(v);
            payload.insert(payload.end(), field.begin(), field.end());
        }
    }
    writeMessage('D', payload);
}

void Connection::sendAuthenticationOk() {
    std::vector<uint8_t> payload(4, 0);
    writeMessage('R', payload);
}

void Connection::sendStartup() {
    auto appendCString = [](std::vector<uint8_t>& buf, const std::string& s) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0);
    };

    std::vector<uint8_t> body;
    appendInt32BE(body, 196608);
    appendCString(body, "user");
    appendCString(body, "insiendb");
    body.push_back(0);

    std::vector<uint8_t> packet;
    appendInt32BE(packet, static_cast<int32_t>(4 + body.size()));
    writeRaw(packet.data(), packet.size());
    writeRaw(body.data(), body.size());
}

bool Connection::waitUntilReady(std::ostream& err) {
    while (true) {
        Message msg;
        if (!readMessage(msg)) return false;
        if (msg.type == 'Z') return true;
        if (msg.type == 'E') {
            size_t n = msg.payload.size();
            if (n > 0 && msg.payload.back() == 0) n--;
            err << "server error: "
                << std::string(reinterpret_cast<const char*>(msg.payload.data()), n) << "\n";
            return false;
        }
    }
}

void Connection::sendQuery(const std::string& sql) {
    std::vector<uint8_t> payload;
    appendString(payload, sql);
    writeMessage('Q', payload);
}

void Connection::sendTerminate() {
    writeMessage('X', {});
}

static void printSelectResult(std::ostream& out,
                              const std::vector<std::string>& columns,
                              const std::vector<std::vector<std::string>>& rows) {
    if (columns.empty()) {
        out << "(empty result set)\n";
        return;
    }

    std::vector<size_t> widths(columns.size(), 4);
    for (size_t i = 0; i < columns.size(); i++)
        widths[i] = std::max(widths[i], columns[i].size());
    for (const auto& row : rows)
        for (size_t i = 0; i < row.size() && i < widths.size(); i++)
            widths[i] = std::max(widths[i], row[i].size());

    auto printLine = [&](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); i++) {
            if (i > 0) out << "  ";
            out << std::setw(static_cast<int>(i < widths.size() ? widths[i] : cells[i].size()))
                << cells[i];
        }
        out << "\n";
    };

    printLine(columns);
    for (const auto& row : rows) printLine(row);
    out << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")\n";
}

bool Connection::readQueryResponse(std::ostream& out) {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;

    while (true) {
        Message msg;
        if (!readMessage(msg)) return false;

        const uint8_t* p = msg.payload.data();
        size_t n = msg.payload.size();

        if (msg.type == 'Z') {
            if (!columns.empty() || !rows.empty())
                printSelectResult(out, columns, rows);
            columns.clear();
            rows.clear();
            return true;
        }

        if (msg.type == 'E') {
            if (n > 0 && p[n - 1] == 0) n--;
            out << "error: " << std::string(reinterpret_cast<const char*>(p), n) << "\n";
            continue;
        }

        if (msg.type == 'C') {
            if (columns.empty() && rows.empty()) {
                if (n > 0 && p[n - 1] == 0) n--;
                std::string tag(reinterpret_cast<const char*>(p), n);
                if (tag.rfind("SELECT", 0) != 0)
                    out << tag << "\n";
            } else {
                printSelectResult(out, columns, rows);
                columns.clear();
                rows.clear();
            }
            continue;
        }

        if (msg.type == 'T') {
            if (n < 2) continue;
            int16_t numCols = readInt16BE(p);
            size_t pos = 2;
            columns.clear();
            for (int16_t i = 0; i < numCols; i++) {
                if (pos + 2 > n) break;
                int16_t nameLen = readInt16BE(p + pos);
                pos += 2;
                if (pos + static_cast<size_t>(nameLen) + 4 > n) break;
                columns.emplace_back(reinterpret_cast<const char*>(p + pos), nameLen);
                pos += static_cast<size_t>(nameLen) + 4;
            }
            rows.clear();
            continue;
        }

        if (msg.type == 'D') {
            if (n < 2) continue;
            int16_t numFields = readInt16BE(p);
            size_t pos = 2;
            std::vector<std::string> row;
            for (int16_t i = 0; i < numFields; i++) {
                if (pos + 4 > n) break;
                int32_t flen = readInt32BE(p + pos);
                pos += 4;
                if (flen < 0) {
                    row.push_back("NULL");
                } else {
                    if (pos + static_cast<size_t>(flen) > n) break;
                    row.emplace_back(reinterpret_cast<const char*>(p + pos), flen);
                    pos += static_cast<size_t>(flen);
                }
            }
            rows.push_back(std::move(row));
            continue;
        }
    }
}

void Connection::sendSessionResult(const SessionResult& result) {
    if (!result.ok) {
        sendError(result.error);
        return;
    }

    for (const StatementResult& sr : result.statements) {
        if (sr.kind == StatementKind::Select) {
            if (!sr.query.columnNames.empty())
                sendRowDescription(sr.query);
            for (const Row& row : sr.query.rows)
                sendDataRow(row);
            sendCommandComplete(sr.commandTag);
        } else {
            sendCommandComplete(sr.commandTag);
        }
    }
}

Connection connectTcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
        throw std::runtime_error("Wire: failed to resolve host '" + host + "'");

    int fd = -1;
    for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        throw std::runtime_error("Wire: connect failed to " + host + ":" + portStr);

    return Connection(fd);
}

} // namespace Wire
