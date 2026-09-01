# insiendb

**insiendb** is a small SQL database engine written in C++17. It stores everything in a single page-oriented `insien.db` file, with a write-ahead log (`insien.db-wal`), slotted heap pages for rows, and B+ trees for secondary indexes.

```
SQL → Lexer → Parser → Semantic Analyzer → Executor → Storage
                                                      ├── Heap pages (rows)
                                                      ├── B+ tree pages (indexes)
                                                      ├── System catalog (__tables / __columns / __indexes)
                                                      └── WAL (crash recovery)
```

---

## Build & run

```bash
make all      # insiendb + insiendb-server
make test     # storage engine suite (155 checks)
make smoke    # quick CREATE / INSERT / INDEX / SELECT
make clean
```

**Local REPL**

```bash
./insiendb --new insien.db          # create fresh DB
./insiendb insien.db                # open existing
```

**Client / server**

```bash
./insiendb-server --new --port 54321 insien.db
./insiendb --host 127.0.0.1 --port 54321
```

Dot-commands in the REPL: `.help`, `.tables`, `.schema`, `.info`, `.quit`

---

## Repository layout

```
insiendb/
├── client/            # insiendb entry + interactive REPL
├── parser/            # lexer, tokens, AST, recursive-descent parser
├── sql/               # catalog, semantic analysis, executor, session, values
├── storage/           # Storage facade (SQL ↔ disk)
├── storage_engine/    # pages, disk I/O, heap, freelist, WAL, B-tree, catalogs
├── server/            # insiendb-server (TCP)
├── wire/              # Postgres-inspired wire protocol
├── tests/             # storage_engine_test
└── Makefile
```

---

## On-disk overview

Two files per database:

| File | Role |
|------|------|
| `insien.db` | Main database — fixed **8192-byte** pages |
| `insien.db-wal` | Append-only write-ahead log |

Everything is little-endian unless noted (wire protocol uses big-endian lengths).

```
┌─────────────────────────────────────────────────────────┐
│  insien.db                                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │
│  │ Page 0   │ │ Page 1   │ │ Page 2   │ │   ...    │   │
│  │ header   │ │ HEAP /   │ │ BTRE /   │ │          │   │
│  │ + boot   │ │ FEEP /…  │ │ HEAP /…  │ │          │   │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘   │
│  byte offset = page_id × 8192                           │
└─────────────────────────────────────────────────────────┘
```

Page kinds are identified by a 4-byte magic at offset 0:

| Magic | Hex | Meaning |
|-------|-----|---------|
| `ADB1` | `0x41444231` | File header (page 0) |
| `HEAP` | `0x48454150` | Slotted heap page (table rows) |
| `FEEP` | `0x46454550` | Free page (freelist) |
| `BTRE` | `0x45525442` | B+ tree index page |

---

## Page 0 — file header (format v5)

Page 0 is never a heap or btree page. It only bootstraps the catalog:

```
Offset  Size  Field
──────  ────  ─────────────────────────
0       u32   magic = ADB1
4       u32   version = 5
8       u32   __tables  heap root page id
12      u32   __columns heap root page id
16      u32   __indexes heap root page id
20      u32   freelist head (0 = empty)
```

Opening a DB reads this bootstrap, then loads the three system heaps into memory.

---

## System catalog

Catalog metadata is stored as **normal heap tables** (same row codec as user data):

### `__tables`
| Column | Type | Meaning |
|--------|------|---------|
| name | TEXT | Table name |
| root_page | INT | First heap page of the table |

### `__columns`
| Column | Type | Meaning |
|--------|------|---------|
| table_name | TEXT | Owning table |
| col_index | INT | 0-based column position |
| col_name | TEXT | Column name |
| type | INT | Encoded `Type` enum |

### `__indexes`
| Column | Meaning |
|--------|---------|
| index name, table, column, root btree page id, … |

A user table is a **singly-linked chain of HEAP pages**. The first page id lives in `__tables.root_page`; each heap page’s header stores `next_page` (0 = end).

```
__tables["users"].root_page
        │
        ▼
   [HEAP p3] ──next──► [HEAP p7] ──next──► [HEAP p12] ──► 0
      rows                rows                rows
```

---

## Heap pages (row storage)

Classic slotted-page layout inside 8 KiB:

```
┌──────────────────────────────────────────────────────────┐
│ Header (16 bytes)                                        │
│  magic u32 | num_slots u16 | data_end u16                │
│  next_page u32 | page_lsn u32                            │
├──────────────────────────────────────────────────────────┤
│ Row bytes grow UPWARD from offset 16                     │
│   [row0][row1][row2]...                                  │
│                                                          │
│                    ~~~~ free gap ~~~~                    │
│                                                          │
│ Slot directory grows DOWNWARD from PAGE_SIZE             │
│   ... [slot2][slot1][slot0]                              │
└──────────────────────────────────────────────────────────┘
```

Each slot is 4 bytes: `(offset: u16, length: u16)`.

- Slot `i` lives at `PAGE_SIZE - (i + 1) * 4`
- `offset == 0` means deleted (tombstone); the RID (page + slot index) stays stable
- Updates try in-place overwrite; if the new row is larger, bytes append at `data_end` when space allows

**RowId** = `(page_id: u32, slot_index: u16)` — the stable pointer indexes use to find a heap row.

### Row codec (bytes inside a slot)

```
[num_columns: u16]
  for each column:
    [type_tag: u8]   1=INT 2=FLOAT 3=TEXT 4=BOOL 5=NUL
    [payload]        absent for NUL; TEXT is length-prefixed
```

Types: `INT`, `FLOAT`, `TEXT`, `BOOL`, `NUL`.

---

## Freelist

When a heap chain is rewritten (or pages are abandoned), pages go on a freelist instead of leaking forever.

**Free page layout**

```
[magic: u32 "FEEP"][next_free: u32]
```

Page 0’s `freelist_head` points at the first free page (`0` = empty). Allocate pops the head; free pushes to the front (LIFO).

---

## Write-ahead log (WAL)

File: `insien.db-wal` (generally `<dbpath>-wal`), format **v2**.

Heap mutations log small **redo** records (insert / update / delete / init / set-next). A full 8 KiB page image is attached only on the **first modification of that heap page after the last checkpoint** (`page.lsn <= redoLsn`) — the same full-page-write rule as PostgreSQL. B-tree pages, freelist pages, and page 0 still go through `savePage()` as a full `PAGE_WRITE`.

Durable heap change:

1. Apply the mutation in memory
2. If `page.lsn <= redoLsn`, attach a **post-image** of the page
3. Set `page.lsn` to this record’s LSN, append the WAL record, fsync
4. Write the page into `insien.db`

On open, WAL records are replayed: an attached image replaces the page; otherwise redo is applied. Records whose page already has `page.lsn >= record.lsn` are skipped. Then a checkpoint truncates the WAL. LSNs keep increasing across checkpoints (`nextLsn` / `redoLsn` live in the WAL header).

**WAL file layout**

```
[32-byte header: magic "WAL1", version=2, page_size, reserved,
                 next_lsn u64, redo_lsn u64]
[records...]
  each record:
    [body_len: u32]
    [type: u8]          1=PAGE_WRITE 2=CHECKPOINT
                        3=HEAP_INIT 4=HEAP_INSERT 5=HEAP_DELETE
                        6=HEAP_UPDATE 7=HEAP_SET_NEXT
    [lsn: u64]
    [flags: u8]         bit 0 = full page image attached
    [page_id: u32]
    [optional 8192-byte page image]
    [redo payload]
      HEAP_INSERT / HEAP_UPDATE → slot u16 + len u16 + row bytes
      HEAP_DELETE               → slot u16
      HEAP_SET_NEXT             → next_page u32
      HEAP_INIT / CHECKPOINT / PAGE_WRITE → (empty)
```

`abandonWithoutCheckpoint()` closes without checkpointing — used in tests to simulate a crash.

---

## Indexes & B+ trees

`CREATE INDEX idx ON t (col)` builds a **B+ tree** whose leaves map encoded keys → RowIds.

```
         ┌─────────────┐
         │  INTERNAL   │  keys + child page ids
         └──┬─────┬────┘
            │     │
     ┌──────┘     └──────┐
     ▼                   ▼
 ┌────────┐         ┌────────┐
 │  LEAF  │──link──►│  LEAF  │──► …
 │ k→RID  │         │ k→RID  │
 └────────┘         └────────┘
```

- Internal nodes: separator keys + child pointers (leftmost child in header)
- Leaves: sorted `(key, RID)` entries + sibling `link` for range scans
- Duplicate keys allowed (same key, different RIDs)
- Default fanout: up to ~200 keys per page (numeric keys)

### B+ tree page layout (magic `BTRE`)

```
Header (16 bytes):
  magic u32 | flags u16 | num_keys u16 | parent u32 | link/left_child u32

flags bit 0 = LEAF

Leaf entries (packed after header):
  [key bytes][page_id u32][slot u16]   repeated num_keys times
  RID = 6 bytes

Internal entries:
  header.left_child, then [key][child_page_id u32] × num_keys
```

### Index key encoding

Fixed-width, byte-comparable keys:

| Column type | Key size | Encoding |
|-------------|----------|----------|
| INT / BOOL | 8 | little-endian int64 (`BOOL` as 0/1) |
| FLOAT | 8 | IEEE bits transformed so unsigned compare ≈ numeric order |
| TEXT | 64 | `[len_be u16][utf8 bytes…][zero pad]` (truncated) |

`NULL` values are not indexed.

### Query path

For `SELECT … WHERE col = / < / <= / > / >= …` when an index exists on `col`:

1. Encode the bound into an index key
2. B+ tree `lookupEqual` or `rangeScan` → list of RowIds
3. Fetch each row from its heap page/slot

Without a usable index, the executor falls back to a full heap scan.

INSERT / UPDATE / DELETE maintain every index on that table (remove old key, insert new key as needed).

---

## Query pipeline

```
"SELECT id FROM users WHERE id = 1;"
        │
        ▼
   Lexer → tokens
        │
        ▼
   Parser → AST (SelectStmt, …)
        │
        ▼
   SemanticAnalyzer → type-check, resolve columns, pick indexes
        │
        ▼
   Executor → Storage (scan / index lookup / mutate)
        │
        ▼
   SessionResult → REPL table  or  Wire messages
```

Supported surface area includes `CREATE TABLE`, `CREATE INDEX`, `INSERT`, `UPDATE`, `DELETE`, `SELECT` (with joins / WHERE as implemented in the executor).

---

## Wire protocol (client ↔ server)

Postgres-inspired framing over TCP (default port `54321`):

```
[type: u8][length: i32 big-endian][payload…]
```

length includes itself (4 bytes), excludes the type byte.

Typical flow: Startup → AuthenticationOk → ReadyForQuery → Query (`Q`) → RowDescription / DataRow / CommandComplete / Error → ReadyForQuery → Terminate (`X`).

The server holds one shared `Storage` (mutex around execute) and one `Session` per connection.

---

## Mental model: one INSERT

```
INSERT INTO users VALUES (42, 'Ada');
```

1. Encode row → `[2][INT][42][TEXT][Ada…]`
2. Find last HEAP page in the table chain (or allocate from freelist / grow file)
3. Insert into slotted page → get `RowId (page, slot)`
4. For each index on `users`, encode key and B+ tree `insert(key, rid)`
5. Heap mutation: redo WAL record (+ FPI if first touch since checkpoint), fsync, then write `.db` page. Other page kinds still log a full image.

---

## Testing

```bash
make test
# Storage engine tests (milestones 1–3 + freelist + WAL + btree + indexes)
# Expected: 173 checks, 0 failed
```

---

## Format version

Current on-disk format: **version 5** (`FileHeader::FORMAT_VERSION`). Older `.db` files from earlier milestones are not auto-migrated — recreate with `--new` if the version mismatches.
