#include "session.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_error.h"
#include <sstream>

Session::Session(Storage& storage)
    : storage(storage), analyzer(catalog), executor(catalog, storage) {
    refreshCatalog();
}

void Session::refreshCatalog() {
    catalog.loadFromStorage(storage);
}

SessionResult Session::execute(const std::string& sql) {
    SessionResult out;
    try {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        auto statements = parser.parseProgram();

        for (auto& stmt : statements) {
            analyzer.analyze(stmt.get());

            StatementResult sr;
            if (auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get())) {
                executor.executeCreateTable(ct);
                sr.kind = StatementKind::CreateTable;
                sr.commandTag = "CREATE TABLE";
            } else if (auto* ci = dynamic_cast<CreateIndexStmt*>(stmt.get())) {
                executor.executeCreateIndex(ci);
                sr.kind = StatementKind::CreateIndex;
                sr.commandTag = "CREATE INDEX";
            } else if (auto* ins = dynamic_cast<InsertStmt*>(stmt.get())) {
                executor.executeInsert(ins);
                sr.kind = StatementKind::Insert;
                sr.rowsAffected = 1;
                sr.commandTag = "INSERT 0 1";
            } else if (auto* sel = dynamic_cast<SelectStmt*>(stmt.get())) {
                sr.kind = StatementKind::Select;
                sr.query = executor.executeSelect(sel);
                sr.commandTag = "SELECT " + std::to_string(sr.query.rows.size());
            } else if (auto* upd = dynamic_cast<UpdateStmt*>(stmt.get())) {
                sr.rowsAffected = executor.executeUpdate(upd);
                sr.kind = StatementKind::Update;
                sr.commandTag = "UPDATE " + std::to_string(sr.rowsAffected);
            } else if (auto* del = dynamic_cast<DeleteStmt*>(stmt.get())) {
                sr.rowsAffected = executor.executeDelete(del);
                sr.kind = StatementKind::Delete;
                sr.commandTag = "DELETE " + std::to_string(sr.rowsAffected);
            }
            out.statements.push_back(std::move(sr));
        }
    } catch (const ParseError& e) {
        out.ok = false;
        out.error = std::string("parse error: ") + e.what();
        out.errorLine = e.line;
    } catch (const SemanticError& e) {
        out.ok = false;
        out.error = std::string("semantic error: ") + e.what();
        out.errorLine = e.line;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
    }
    return out;
}
