#pragma once
#include <string>
#include <variant>

// The engine's runtime type system. Deliberately small — you can add
// DATE, BLOB, etc. later without touching the analyzer's *logic*, only
// this enum and the compatibility rules in the analyzer.
enum class Type { INT, FLOAT, TEXT, BOOL, NUL, UNKNOWN };

std::string typeToString(Type t);

// True for INT/FLOAT — used everywhere we need "is this a number,
// regardless of which numeric kind" (arithmetic, numeric comparisons).
bool isNumeric(Type t);

// A single typed runtime value. This is what will eventually sit inside
// actual table rows — separate from Value's cousin, LiteralExpr, which is
// just the *syntax* for a literal before it's been evaluated.
struct Value {
    Type type = Type::NUL;
    std::variant<std::monostate, long long, double, std::string, bool> data;

    static Value makeInt(long long v);
    static Value makeFloat(double v);
    static Value makeText(std::string v);
    static Value makeBool(bool v);
    static Value makeNull();

    std::string toString() const;
};
