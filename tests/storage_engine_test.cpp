// Standalone tests for storage engine milestones 1–3 (no SQL stack required).
#include "storage_engine/disk_manager.h"
#include "storage_engine/heap_page.h"
#include "storage_engine/row_codec.h"
#include "storage_engine/freelist.h"
#include "storage_engine/btree.h"
#include "storage_engine/index_key.h"
#include "storage.h"
#include "catalog.h"
#include "value.h"
#include <cstring>
#include <sys/stat.h>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int testsRun = 0;
static int testsFailed = 0;

#define CHECK(cond) do { \
    ++testsRun; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
        ++testsFailed; \
    } \
} while (0)

static bool valueEq(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case Type::INT:   return std::get<long long>(a.data) == std::get<long long>(b.data);
        case Type::FLOAT: return std::get<double>(a.data) == std::get<double>(b.data);
        case Type::TEXT:  return std::get<std::string>(a.data) == std::get<std::string>(b.data);
        case Type::BOOL:  return std::get<bool>(a.data) == std::get<bool>(b.data);
        case Type::NUL:   return true;
        default:          return false;
    }
}

static bool rowEq(const Row& a, const Row& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!valueEq(a[i], b[i])) return false;
    return true;
}

static Row sampleRow() {
    return {
        Value::makeInt(42),
        Value::makeText("Aditya"),
        Value::makeNull(),
        Value::makeBool(true),
        Value::makeFloat(3.14),
    };
}

// --- Milestone 1: Page + DiskManager ---

static void testDiskManager() {
    std::string path = "/tmp/insien_disk_test.db";
    fs::remove(path);

    DiskManager dm;
    dm.create(path);
    CHECK(dm.pageCount() == 1);

    Page p1;
    p1.writeU32(0, 0xDEADBEEF);
    p1.writeU16(4, 12345);
    dm.writePage(0, p1);

    PageId newId = dm.allocatePage();
    CHECK(newId == 1);
    CHECK(dm.pageCount() == 2);

    Page pNew;
    pNew.writeU32(0, 0xCAFEBABE);
    dm.writePage(newId, pNew);

    DiskManager dm2;
    dm2.open(path);
    CHECK(dm2.pageCount() == 2);

    Page loaded0, loaded1;
    dm2.readPage(0, loaded0);
    dm2.readPage(1, loaded1);
    CHECK(loaded0.readU32(0) == 0xDEADBEEF);
    CHECK(loaded0.readU16(4) == 12345);
    CHECK(loaded1.readU32(0) == 0xCAFEBABE);

    fs::remove(path);
    std::cout << "  disk_manager: page I/O round-trip OK\n";
}

// --- Milestone 3: Row codec ---

static void testRowCodec() {
    Row original = sampleRow();

    std::vector<uint8_t> bytes;
    RowCodec::encodeRow(original, bytes);
    CHECK(bytes.size() > 0);

    Row decoded = RowCodec::decodeRow(bytes.data(), bytes.size());
    CHECK(rowEq(original, decoded));
    CHECK(RowCodec::encodedRowSize(original) == bytes.size());

    // Edge: empty-ish row with only NULL
    Row nullRow = { Value::makeNull() };
    std::vector<uint8_t> nullBytes;
    RowCodec::encodeRow(nullRow, nullBytes);
    Row decodedNull = RowCodec::decodeRow(nullBytes.data(), nullBytes.size());
    CHECK(rowEq(nullRow, decodedNull));

    std::cout << "  row_codec: serialize/deserialize round-trip OK\n";
}

// --- Milestone 2: Slotted heap page ---

static void testHeapPageInMemory() {
    Page page;
    HeapPage::init(page);
    CHECK(HeapPage::isHeapPage(page));
    CHECK(HeapPage::pageLsn(page) == 0);
    CHECK(HeapPage::slotCount(page) == 0);
    CHECK(HeapPage::liveRowCount(page) == 0);

    Row r1 = { Value::makeInt(1), Value::makeText("Alice") };
    Row r2 = { Value::makeInt(2), Value::makeText("Bob") };

    auto s0 = HeapPage::insertRow(page, r1);
    auto s1 = HeapPage::insertRow(page, r2);
    CHECK(s0.has_value() && s1.has_value());
    CHECK(HeapPage::slotCount(page) == 2);
    CHECK(HeapPage::liveRowCount(page) == 2);

    CHECK(rowEq(HeapPage::getRow(page, *s0), r1));
    CHECK(rowEq(HeapPage::getRow(page, *s1), r2));

    CHECK(HeapPage::remove(page, *s0));
    CHECK(HeapPage::liveRowCount(page) == 1);
    CHECK(HeapPage::slotCount(page) == 2); // tombstone keeps slot entry

    auto live = HeapPage::liveSlots(page);
    CHECK(live.size() == 1);
    CHECK(rowEq(RowCodec::decodeRow(live[0].bytes.data(), live[0].bytes.size()), r2));

    std::cout << "  heap_page: insert/iterate/delete OK\n";
}

static void testHeapPageOnDisk() {
    std::string path = "/tmp/insien_heap_test.db";
    fs::remove(path);

    DiskManager dm;
    dm.create(path);
    PageId heapId = dm.allocatePage(); // page 1

    Page page;
    HeapPage::init(page);
    HeapPage::insertRow(page, sampleRow());
    HeapPage::insertRow(page, Row{ Value::makeInt(99), Value::makeText("disk") });
    dm.writePage(heapId, page);

    // Re-open and read back
    DiskManager dm2;
    dm2.open(path);
    Page loaded;
    dm2.readPage(heapId, loaded);
    CHECK(HeapPage::isHeapPage(loaded));
    CHECK(HeapPage::liveRowCount(loaded) == 2);

    auto slots = HeapPage::liveSlots(loaded);
    CHECK(slots.size() == 2);
    CHECK(rowEq(RowCodec::decodeRow(slots[0].bytes.data(), slots[0].bytes.size()), sampleRow()));

    fs::remove(path);
    std::cout << "  heap_page: persist to disk and reload OK\n";
}

static void testHeapPageFill() {
    Page page;
    HeapPage::init(page);

    int inserted = 0;
    for (int i = 0; i < 500; i++) {
        Row r = { Value::makeInt(i), Value::makeText("row_" + std::to_string(i)) };
        if (!HeapPage::insertRow(page, r).has_value())
            break;
        inserted++;
    }
    CHECK(inserted > 10); // should fit many rows before page is full
    CHECK(static_cast<int>(HeapPage::liveRowCount(page)) == inserted);

    std::cout << "  heap_page: packed " << inserted << " rows before page full\n";
}

static void testFreelistPageFormat() {
    Page page;
    FreeListPage::init(page, 42);
    CHECK(FreeListPage::isFreePage(page));
    CHECK(FreeListPage::nextFree(page) == 42);

    Page page2;
    FreeListPage::init(page2, 0);
    CHECK(FreeListPage::nextFree(page2) == 0);
    std::cout << "  freelist: page format OK\n";
}

static void testFreelistReuse() {
    std::string path = "/tmp/insien_freelist_test.db";
    fs::remove(path);

    Storage storage;
    storage.createDatabase(path);

    TableSchema schema;
    schema.name = "items";
    schema.columns = {ColumnSchema{"id", Type::INT, 0}};
    storage.createTable(schema);
    storage.insertRow("items", {Value::makeInt(1)});
    storage.insertRow("items", {Value::makeInt(2)});

    PageId pagesBeforeRewrite = storage.pageCount();
    storage.rewriteTable("items", {Row{Value::makeInt(99)}});
    CHECK(storage.freelistHead() != 0);
    PageId pagesAfterRewrite = storage.pageCount();
    // New chain is allocated before the old chain is freed, so the file may grow by one.
    CHECK(pagesAfterRewrite <= pagesBeforeRewrite + 1);

    TableSchema schema2;
    schema2.name = "other";
    schema2.columns = {ColumnSchema{"n", Type::INT, 0}};
    storage.createTable(schema2);
    PageId pagesAfterNewTable = storage.pageCount();
    // Next allocation should pop from freelist — file size should not grow again.
    CHECK(pagesAfterNewTable == pagesAfterRewrite);

    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  freelist: rewrite returns pages, allocate reuses ("
              << pagesBeforeRewrite << " -> " << pagesAfterRewrite << " -> "
              << pagesAfterNewTable << " pages)\n";
}

static void testWalCrashRecovery() {
    std::string path = "/tmp/insien_wal_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    {
        Storage storage;
        storage.createDatabase(path);
        TableSchema schema;
        schema.name = "events";
        schema.columns = {ColumnSchema{"id", Type::INT, 0}, ColumnSchema{"msg", Type::TEXT, 1}};
        storage.createTable(schema);
        storage.insertRow("events", {Value::makeInt(1), Value::makeText("boot")});
        storage.abandonWithoutCheckpoint();
    }

    struct stat st {};
    CHECK(stat((path + "-wal").c_str(), &st) == 0);
    CHECK(st.st_size > 32);

    Storage storage2;
    storage2.openDatabase(path);
    auto rows = storage2.scanTable("events");
    CHECK(rows.size() == 1);
    CHECK(rows[0][0].type == Type::INT);
    CHECK(std::get<long long>(rows[0][0].data) == 1);

    storage2.closeDatabase();
    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  wal: crash recovery replayed page writes OK\n";
}

static TableSchema eventsSchema() {
    TableSchema schema;
    schema.name = "events";
    schema.columns = {ColumnSchema{"id", Type::INT, 0}, ColumnSchema{"msg", Type::TEXT, 1}};
    return schema;
}

static void testWalLsnSurvivesCheckpoint() {
    std::string path = "/tmp/insien_wal_lsn_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    LSN lsnAfterCreate = 0;
    {
        Storage storage;
        storage.createDatabase(path);
        storage.createTable(eventsSchema());
        storage.insertRow("events", {Value::makeInt(1), Value::makeText("boot")});
        lsnAfterCreate = storage.walNextLsn();
        CHECK(lsnAfterCreate > 1);
        storage.closeDatabase();
    }

    {
        Storage storage;
        storage.openDatabase(path);
        // close() wrote a CHECKPOINT at lsnAfterCreate; LSNs are not reset.
        CHECK(storage.walRedoLsn() == lsnAfterCreate);
        CHECK(storage.walNextLsn() == lsnAfterCreate + 1);
        storage.closeDatabase();
    }

    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  wal: LSN is monotonic across checkpoint OK\n";
}

static void testWalHeapFpiThenRedo() {
    std::string path = "/tmp/insien_wal_fpi_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    {
        Storage storage;
        storage.createDatabase(path);
        storage.createTable(eventsSchema());
        storage.insertRow("events", {Value::makeInt(1), Value::makeText("keep")});
        storage.closeDatabase();
    }

    {
        Storage storage;
        storage.openDatabase(path);
        storage.insertRow("events", {Value::makeInt(2), Value::makeText("second")});
        storage.insertRow("events", {Value::makeInt(3), Value::makeText("third")});
        storage.abandonWithoutCheckpoint();
    }

    {
        WalManager wal;
        wal.open(path + "-wal");
        auto records = wal.readAllRecords();
        std::vector<WalRecord> inserts;
        for (const auto& rec : records) {
            if (rec.type == WalRecordType::HEAP_INSERT)
                inserts.push_back(rec);
        }
        CHECK(inserts.size() == 2);
        CHECK(inserts[0].hasFpi());
        CHECK(!inserts[1].hasFpi());
        CHECK(inserts[1].payload.size() < PAGE_SIZE);
        wal.close();
    }

    {
        Storage storage;
        storage.openDatabase(path);
        auto rows = storage.scanTable("events");
        CHECK(rows.size() == 3);
        CHECK(std::get<long long>(rows[1][0].data) == 2);
        CHECK(std::get<long long>(rows[2][0].data) == 3);
        storage.closeDatabase();
    }

    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  wal: first heap touch after checkpoint logs FPI, later redo only OK\n";
}

static void testWalHeapUpdateDeleteReplay() {
    std::string path = "/tmp/insien_wal_dml_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    RowId firstRid{};
    {
        Storage storage;
        storage.createDatabase(path);
        storage.createTable(eventsSchema());
        storage.insertRow("events", {Value::makeInt(1), Value::makeText("old")});
        storage.insertRow("events", {Value::makeInt(2), Value::makeText("keep")});
        storage.scanTableWithRids("events", [&](RowId rid, const Row& row) {
            if (row[0].type == Type::INT && std::get<long long>(row[0].data) == 1)
                firstRid = rid;
        });
        storage.closeDatabase();
    }

    {
        Storage storage;
        storage.openDatabase(path);
        CHECK(storage.updateRow("events", firstRid,
                                {Value::makeInt(1), Value::makeText("new")}));
        storage.scanTableWithRids("events", [&](RowId rid, const Row& row) {
            if (row[0].type == Type::INT && std::get<long long>(row[0].data) == 2)
                CHECK(storage.deleteRow("events", rid));
        });
        storage.abandonWithoutCheckpoint();
    }

    {
        WalManager wal;
        wal.open(path + "-wal");
        bool sawUpdate = false, sawDelete = false;
        for (const auto& rec : wal.readAllRecords()) {
            if (rec.type == WalRecordType::HEAP_UPDATE) sawUpdate = true;
            if (rec.type == WalRecordType::HEAP_DELETE) sawDelete = true;
        }
        CHECK(sawUpdate);
        CHECK(sawDelete);
        wal.close();
    }

    {
        Storage storage;
        storage.openDatabase(path);
        auto rows = storage.scanTable("events");
        CHECK(rows.size() == 1);
        CHECK(std::get<long long>(rows[0][0].data) == 1);
        CHECK(std::get<std::string>(rows[0][1].data) == "new");
        storage.closeDatabase();
    }

    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  wal: heap UPDATE/DELETE redo recovered after crash OK\n";
}

static void testIndexKey() {
    uint8_t a[8], b[8];
    IndexKey::encodeInt(42, a);
    IndexKey::encodeInt(-1, b);
    CHECK(IndexKey::decodeInt(a) == 42);
    CHECK(IndexKey::decodeInt(b) == -1);
    CHECK(IndexKey::compareInt(1, 2) < 0);
    CHECK(IndexKey::compareEncoded(a, b) > 0);
    std::cout << "  index_key: INT encode/compare OK\n";
}

static void insertIntKey(BTree& tree, int64_t k, RowId rid) {
    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(k, key);
    tree.insert(key, rid);
}

static void testBTreeInsertLookup() {
    BTree tree(3);
    insertIntKey(tree, 10, {100, 0});
    insertIntKey(tree, 5, {101, 1});
    insertIntKey(tree, 15, {102, 2});

    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(10, key);
    auto r10 = tree.lookupEqual(key);
    CHECK(r10.size() == 1);
    CHECK(r10[0].pageId == 100 && r10[0].slotIndex == 0);

    IndexKey::encodeInt(99, key);
    CHECK(tree.lookupEqual(key).empty());
    CHECK(tree.pageCount() >= 1);
    std::cout << "  btree: basic insert/lookup OK\n";
}

static void testBTreeBulkAndRange() {
    BTree tree(3);
    std::vector<int> order = {10, 5, 15, 1, 20, 3, 7, 12, 18, 2,
                              4, 6, 8, 9, 11, 13, 14, 16, 17, 19};
    for (int k : order)
        insertIntKey(tree, k, {static_cast<PageId>(1000 + k), static_cast<uint16_t>(k)});

    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    for (int k = 1; k <= 20; k++) {
        IndexKey::encodeInt(k, key);
        auto found = tree.lookupEqual(key);
        CHECK(found.size() == 1);
        CHECK(found[0].pageId == static_cast<PageId>(1000 + k));
        CHECK(found[0].slotIndex == static_cast<uint16_t>(k));
    }

    BTreeBound lo;
    BTreeBound hi;
    lo.unbounded = false;
    IndexKey::encodeInt(6, lo.key.data());
    lo.inclusive = true;
    hi.unbounded = false;
    IndexKey::encodeInt(14, hi.key.data());
    hi.inclusive = true;
    auto range = tree.rangeScan(lo, hi);
    CHECK(range.size() == 9);
    for (size_t i = 0; i < range.size(); i++)
        CHECK(range[i].slotIndex == static_cast<uint16_t>(6 + i));

    CHECK(tree.pageCount() > 1);
    std::cout << "  btree: 20 keys out-of-order + range scan OK (" << tree.pageCount() << " pages)\n";
}

static void testBTreeDuplicateKeys() {
    BTree tree(3);
    insertIntKey(tree, 5, {1, 0});
    insertIntKey(tree, 5, {2, 1});
    insertIntKey(tree, 5, {3, 2});
    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(5, key);
    auto found = tree.lookupEqual(key);
    CHECK(found.size() == 3);
    std::cout << "  btree: duplicate keys OK\n";
}

static void testBTreeComparisonOps() {
    BTree tree(3);
    for (int k = 1; k <= 10; k++)
        insertIntKey(tree, k, {static_cast<PageId>(k), static_cast<uint16_t>(k)});

    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(5, key);

    BTreeBound lo;
    BTreeBound hi;
    lo.unbounded = true;
    hi.unbounded = false;
    std::memcpy(hi.key.data(), key, IndexKey::KEY_SIZE_NUMERIC);
    hi.inclusive = false;
    CHECK(tree.rangeScan(lo, hi).size() == 4);

    lo.unbounded = false;
    std::memcpy(lo.key.data(), key, IndexKey::KEY_SIZE_NUMERIC);
    lo.inclusive = false;
    hi.unbounded = true;
    CHECK(tree.rangeScan(lo, hi).size() == 5);

    std::cout << "  btree: comparison range bounds OK\n";
}

static void testIndexKeyTypes() {
    uint8_t key[IndexKey::MAX_KEY_SIZE];

    IndexKey::encode(Value::makeText("bob"), Type::TEXT, key, IndexKey::KEY_SIZE_TEXT);
    uint8_t key2[IndexKey::MAX_KEY_SIZE];
    IndexKey::encode(Value::makeText("carol"), Type::TEXT, key2, IndexKey::KEY_SIZE_TEXT);
    CHECK(IndexKey::compare(key, key2, IndexKey::KEY_SIZE_TEXT) < 0);

    IndexKey::encode(Value::makeFloat(3.5), Type::FLOAT, key, IndexKey::KEY_SIZE_NUMERIC);
    IndexKey::encode(Value::makeFloat(10.0), Type::FLOAT, key2, IndexKey::KEY_SIZE_NUMERIC);
    CHECK(IndexKey::compare(key, key2, IndexKey::KEY_SIZE_NUMERIC) < 0);

    IndexKey::encode(Value::makeBool(true), Type::BOOL, key, IndexKey::KEY_SIZE_NUMERIC);
    IndexKey::encode(Value::makeBool(false), Type::BOOL, key2, IndexKey::KEY_SIZE_NUMERIC);
    CHECK(IndexKey::compare(key, key2, IndexKey::KEY_SIZE_NUMERIC) > 0);

    std::cout << "  index_key: TEXT/FLOAT/BOOL encode/compare OK\n";
}

static void testBTreePageFormat() {
    Page page;
    BTreePage::initLeaf(page);
    CHECK(BTreePage::isBTreePage(page));
    CHECK(BTreePage::isLeaf(page));
    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(42, key);
    BTreePage::writeLeafEntry(page, 0, key, IndexKey::KEY_SIZE_NUMERIC, {7, 3});
    page.writeU16(BTreePage::OFF_NUM_KEYS, 1);
    uint8_t readKey[IndexKey::KEY_SIZE_NUMERIC];
    BTreePage::readLeafKey(page, 0, IndexKey::KEY_SIZE_NUMERIC, readKey);
    CHECK(IndexKey::decodeInt(readKey) == 42);
    CHECK(BTreePage::leafRidAt(page, 0, IndexKey::KEY_SIZE_NUMERIC).pageId == 7);
    CHECK(BTreePage::leafRidAt(page, 0, IndexKey::KEY_SIZE_NUMERIC).slotIndex == 3);
    std::cout << "  btree: page layout OK\n";
}

static void testStorageCreateIndex() {
    std::string path = "/tmp/insien_index_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    Storage storage;
    storage.createDatabase(path);

    TableSchema schema;
    schema.name = "users";
    schema.columns = {
        ColumnSchema{"id", Type::INT, 0},
        ColumnSchema{"name", Type::TEXT, 1},
    };
    storage.createTable(schema);
    storage.insertRow("users", {Value::makeInt(10), Value::makeText("alice")});
    storage.insertRow("users", {Value::makeInt(20), Value::makeText("bob")});
    storage.insertRow("users", {Value::makeInt(10), Value::makeText("alice2")});

    storage.createIndex("idx_users_id", "users", "id");
    CHECK(storage.findIndex("idx_users_id") != nullptr);

    auto rows = storage.indexSearchRows("idx_users_id", IndexCompareOp::Eq, Value::makeInt(10));
    CHECK(rows.size() == 2);
    CHECK(rows[0][1].type == Type::TEXT);

    storage.closeDatabase();

    Storage storage2;
    storage2.openDatabase(path);
    CHECK(storage2.findIndex("idx_users_id") != nullptr);
    auto rows2 = storage2.indexSearchRows("idx_users_id", IndexCompareOp::Eq, Value::makeInt(20));
    CHECK(rows2.size() == 1);
    CHECK(std::get<std::string>(rows2[0][1].data) == "bob");
    storage2.closeDatabase();

    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  storage: CREATE INDEX + lookup + reopen OK\n";
}

static void testStorageTextIndex() {
    std::string path = "/tmp/insien_text_index_test.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    Storage storage;
    storage.createDatabase(path);

    TableSchema schema;
    schema.name = "items";
    schema.columns = {
        ColumnSchema{"name", Type::TEXT, 0},
        ColumnSchema{"qty", Type::INT, 1},
    };
    storage.createTable(schema);
    storage.insertRow("items", {Value::makeText("apple"), Value::makeInt(1)});
    storage.insertRow("items", {Value::makeText("banana"), Value::makeInt(2)});
    storage.insertRow("items", {Value::makeText("cherry"), Value::makeInt(3)});

    storage.createIndex("idx_items_name", "items", "name");
    auto rows = storage.indexSearchRows("idx_items_name", IndexCompareOp::Lt, Value::makeText("cherry"));
    CHECK(rows.size() == 2);

    storage.closeDatabase();
    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  storage: TEXT index range search OK\n";
}

static void testBTreeRemove() {
    BTree tree(3);
    insertIntKey(tree, 5, {1, 0});
    insertIntKey(tree, 5, {2, 1});
    insertIntKey(tree, 5, {3, 2});

    uint8_t key[IndexKey::KEY_SIZE_NUMERIC];
    IndexKey::encodeInt(5, key);
    CHECK(tree.removeEqual(key, {2, 1}));
    auto found = tree.lookupEqual(key);
    CHECK(found.size() == 2);
    CHECK(tree.removeEqual(key, {1, 0}));
    CHECK(tree.removeEqual(key, {3, 2}));
    CHECK(tree.lookupEqual(key).empty());
    std::cout << "  btree: remove by key+rid OK\n";
}

static void testHeapPageUpdate() {
    Page page;
    HeapPage::init(page);
    Row r1 = {Value::makeInt(1), Value::makeText("short")};
    auto slot = HeapPage::insertRow(page, r1);
    CHECK(slot.has_value());

    Row r2 = {Value::makeInt(2), Value::makeText("x")};
    CHECK(HeapPage::updateRow(page, *slot, r2));
    CHECK(rowEq(HeapPage::getRow(page, *slot), r2));

    Row r3 = {Value::makeInt(3), Value::makeText("a much longer updated value")};
    CHECK(HeapPage::updateRow(page, *slot, r3));
    CHECK(rowEq(HeapPage::getRow(page, *slot), r3));

    CHECK(HeapPage::remove(page, *slot));
    CHECK(HeapPage::liveRowCount(page) == 0);
    std::cout << "  heap_page: in-place update + tombstone OK\n";
}

static void testIncrementalDmlWithIndex() {
    std::string path = "/tmp/insien_incremental_dml.db";
    fs::remove(path);
    fs::remove(path + "-wal");

    Storage storage;
    storage.createDatabase(path);

    TableSchema schema;
    schema.name = "scores";
    schema.columns = {
        ColumnSchema{"id", Type::INT, 0},
        ColumnSchema{"pts", Type::INT, 1},
    };
    storage.createTable(schema);
    storage.insertRow("scores", {Value::makeInt(1), Value::makeInt(10)});
    storage.insertRow("scores", {Value::makeInt(2), Value::makeInt(20)});
    storage.insertRow("scores", {Value::makeInt(3), Value::makeInt(30)});
    storage.createIndex("idx_scores_pts", "scores", "pts");

    storage.scanTableWithRids("scores", [&](RowId rid, const Row& row) {
        (void)rid;
        if (row[0].type == Type::INT && std::get<long long>(row[0].data) == 2) {
            storage.updateRow("scores", rid, {Value::makeInt(2), Value::makeInt(25)});
        }
    });

    auto byPts = storage.indexSearchRows("idx_scores_pts", IndexCompareOp::Eq, Value::makeInt(25));
    CHECK(byPts.size() == 1);
    CHECK(std::get<long long>(byPts[0][0].data) == 2);

    auto old20 = storage.indexSearchRows("idx_scores_pts", IndexCompareOp::Eq, Value::makeInt(20));
    CHECK(old20.empty());

    storage.scanTableWithRids("scores", [&](RowId rid, const Row& row) {
        if (row[0].type == Type::INT && std::get<long long>(row[0].data) == 1)
            storage.deleteRow("scores", rid);
    });

    CHECK(storage.scanTable("scores").size() == 2);
    auto byId = storage.indexSearchRows("idx_scores_pts", IndexCompareOp::Eq, Value::makeInt(10));
    CHECK(byId.empty());

    storage.closeDatabase();
    fs::remove(path);
    fs::remove(path + "-wal");
    std::cout << "  storage: incremental UPDATE/DELETE + index maintenance OK\n";
}

int main() {
    std::cout << "Storage engine tests (milestones 1–3 + freelist + wal + btree)\n";

    try {
        testDiskManager();
        testRowCodec();
        testHeapPageInMemory();
        testHeapPageUpdate();
        testHeapPageOnDisk();
        testHeapPageFill();
        testFreelistPageFormat();
        testFreelistReuse();
        testWalCrashRecovery();
        testWalLsnSurvivesCheckpoint();
        testWalHeapFpiThenRedo();
        testWalHeapUpdateDeleteReplay();
        testIndexKey();
        testIndexKeyTypes();
        testBTreePageFormat();
        testBTreeInsertLookup();
        testBTreeBulkAndRange();
        testBTreeDuplicateKeys();
        testBTreeRemove();
        testBTreeComparisonOps();
        testStorageCreateIndex();
        testStorageTextIndex();
        testIncrementalDmlWithIndex();
    } catch (const std::exception& e) {
        std::cerr << "UNHANDLED EXCEPTION: " << e.what() << "\n";
        return 1;
    }

    std::cout << testsRun << " checks, " << testsFailed << " failed\n";
    return testsFailed == 0 ? 0 : 1;
}
