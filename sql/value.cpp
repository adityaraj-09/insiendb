#include "value.h"
#include <string>
#include <sstream>
#include <iomanip>
using namespace std;

std::string typeToString(Type t) {
    switch (t) {
        case Type::INT: return "INT";
        case Type::FLOAT: return "FLOAT";
        case Type::TEXT: return "TEXT";
        case Type::BOOL: return "BOOL";
        case Type::NUL: return "NULL";
        case Type::UNKNOWN: return "UNKNOWN";
    }
    return "?";
}

bool isNumeric(Type t) { return t == Type::INT || t == Type::FLOAT; }

Value Value::makeInt(long long v) { Value val; val.type = Type::INT; val.data = v; return val; }
Value Value::makeFloat(double v) { Value val; val.type = Type::FLOAT; val.data = v; return val; }
Value Value::makeText(std::string v) { Value val; val.type = Type::TEXT; val.data = std::move(v); return val; }
Value Value::makeBool(bool v) { Value val; val.type = Type::BOOL; val.data = v; return val; }
Value Value::makeNull() { Value val; val.type = Type::NUL; val.data = std::monostate{}; return val; }

std::string Value::toString() const {
    switch (type) {
        case Type::INT: return std::to_string(std::get<long long>(data));
        case Type::FLOAT: return std::to_string(std::get<double>(data));
        case Type::TEXT: return std::get<std::string>(data);
        case Type::BOOL: return std::get<bool>(data) ? "true" : "false";
        case Type::NUL: return "NULL";
        case Type::UNKNOWN: return "?";
    }
    return "?";
}
