CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0
INCLUDES  = -I. -Iparser -Isql -Iclient -Istorage -Istorage_engine

PARSER_SRC = \
	parser/lexer.cpp \
	parser/parser.cpp \
	parser/ast.cpp

SQL_SRC = \
	sql/value.cpp \
	sql/catalog.cpp \
	sql/semantic_analyzer.cpp \
	sql/executor.cpp \
	sql/session.cpp

STORAGE_SRC = \
	storage/storage.cpp \
	storage_engine/disk_manager.cpp \
	storage_engine/file_header.cpp \
	storage_engine/system_catalog.cpp \
	storage_engine/freelist.cpp \
	storage_engine/wal.cpp \
	storage_engine/row_codec.cpp \
	storage_engine/heap_page.cpp \
	storage_engine/index_key.cpp \
	storage_engine/btree.cpp \
	storage_engine/index_catalog.cpp

CLIENT_SRC = \
	client/main.cpp \
	client/repl.cpp \
	wire/protocol.cpp

SERVER_SRC = \
	server/main.cpp \
	wire/protocol.cpp

COMMON_SRC = $(PARSER_SRC) $(SQL_SRC) $(STORAGE_SRC)

.PHONY: all clean test smoke

all: mini_sql minisql-server

mini_sql: $(CLIENT_SRC) $(COMMON_SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

minisql-server: $(SERVER_SRC) $(COMMON_SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

storage_engine_test: tests/storage_engine_test.cpp $(STORAGE_SRC) sql/value.cpp sql/catalog.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

test: storage_engine_test
	./storage_engine_test

# Quick SQL smoke test through the embedded REPL (non-interactive).
smoke: mini_sql
	rm -f /tmp/minisql_smoke.db /tmp/minisql_smoke.db-wal
	printf '%s\n' \
		'CREATE TABLE t (id INT, name TEXT);' \
		"INSERT INTO t VALUES (1, 'alice');" \
		"INSERT INTO t VALUES (2, 'bob');" \
		'CREATE INDEX idx_t_id ON t (id);' \
		'SELECT * FROM t WHERE id = 1;' \
		'.quit' \
		| ./mini_sql --new /tmp/minisql_smoke.db

clean:
	rm -f mini_sql minisql-server storage_engine_test
	rm -f *.db *.db-wal /tmp/minisql_*.db /tmp/minisql_*.db-wal
