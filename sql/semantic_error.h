#pragma once
#include <stdexcept>
#include <string>

// Thrown by SemanticAnalyzer for anything the parser couldn't catch:
// unknown tables/columns, type mismatches, ambiguous names, etc.
// Kept as its own type (distinct from ParseError) so callers can tell
// "malformed SQL" apart from "well-formed SQL that doesn't make sense."
struct SemanticError : std::runtime_error {
    int line;
    SemanticError(const std::string& msg, int line)
        : std::runtime_error(msg), line(line) {}
};
