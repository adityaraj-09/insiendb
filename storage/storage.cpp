#include "storage.h"
#include "storage_engine/heap_page.h"
#include "storage_engine/freelist.h"
#include "storage_engine/row_codec.h"
#include "storage_engine/index_catalog.h"
#include "storage_engine/btree.h"
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unistd.h>

Storage::~Storage() {
    if (disk.isOpen()) {
        try { closeDatabase(); } catch (...) {}
    }
}

void Storage::createDatabase(const std::string& path) {
    disk.create(path);
    dbPath = path;
    walFilePath = walPathForDb(path);
    wal.create(walFilePath);
    tables.clear();
    indexes_.clear();
    recovering = false;

    boot.tablesRoot = allocateHeapPage();
    boot.columnsRoot = allocateHeapPage();
    boot.indexesRoot = allocateHeapPage();

    Page header;
    FileHeader::writeBootstrap(header, boot);
    savePage(0, header);
}

void Storage::recoverFromWal() {
    auto records = wal.readAllRecords();
    if (records.empty()) return;

    recovering = true;
    for (auto& rec : records) {
        if (rec.type == WalRecordType::PAGE_WRITE) {
            disk.growToInclude(rec.pageId);
            disk.writePage(rec.pageId, rec.page);
        }
    }
    disk.fsync();
    recovering = false;

    wal.appendCheckpoint();
    wal.flush();
    wal.truncate();
}

void Storage::openDatabase(const std::string& path) {
    disk.open(path);
    dbPath = path;
    walFilePath = walPathForDb(path);

    if (access(walFilePath.c_str(), F_OK) == 0)
        wal.open(walFilePath);
    else
        wal.create(walFilePath);

    recoverFromWal();

    Page header;
    disk.readPage(0, header);
    boot = FileHeader::readBootstrap(header);
    tables = SystemCatalog::load(boot, scanFn());
    indexes_ = IndexCatalog::load(boot, scanFn(), tables);
}

void Storage::checkpoint() {
    if (!disk.isOpen()) return;
    disk.fsync();
    wal.appendCheckpoint();
    wal.flush();
    wal.truncate();
}

void Storage::closeDatabase() {
    if (!disk.isOpen()) return;
    checkpoint();
    wal.close();
    disk.close();
    tables.clear();
    indexes_.clear();
}

void Storage::abandonWithoutCheckpoint() {
    wal.close();
    disk.close();
}

void Storage::saveBootstrap() {
    Page header;
    FileHeader::writeBootstrap(header, boot);
    savePage(0, header);
}

std::vector<TableSchema> Storage::getAllSchemas() const {
    std::vector<TableSchema> schemas;
    schemas.reserve(tables.size());
    for (auto& [name, entry] : tables)
        schemas.push_back(entry.schema);
    return schemas;
}

const TableDirectoryEntry& Storage::requireTable(const std::string& tableName) const {
    auto it = tables.find(tableName);
    if (it == tables.end())
        throw std::runtime_error("Storage: table '" + tableName + "' has no backing store "
                                  "(executeCreateTable was probably skipped)");
    return it->second;
}

TableDirectoryEntry& Storage::requireTable(const std::string& tableName) {
    return const_cast<TableDirectoryEntry&>(
        static_cast<const Storage*>(this)->requireTable(tableName));
}

Page Storage::loadPage(PageId id) const {
    Page page;
    disk.readPage(id, page);
    return page;
}

void Storage::savePage(PageId id, const Page& page) {
    if (!recovering) {
        wal.appendPageWrite(id, page);
        wal.flush();
    }
    if (id >= disk.pageCount())
        disk.growToInclude(id);
    disk.writePage(id, page);
}

PageId Storage::freelistPop() {
    if (boot.freelistHead == 0) return 0;

    PageId id = boot.freelistHead;
    Page page = loadPage(id);
    if (!FreeListPage::isFreePage(page))
        throw std::runtime_error("Storage: freelist head page " + std::to_string(id) + " is corrupt");

    boot.freelistHead = FreeListPage::nextFree(page);
    return id;
}

void Storage::freelistPush(PageId id) {
    Page page;
    FreeListPage::init(page, boot.freelistHead);
    savePage(id, page);
    boot.freelistHead = id;
}

PageId Storage::allocatePage() {
    PageId id = freelistPop();
    if (id != 0) return id;
    return disk.allocatePage();
}

PageId Storage::allocateBTreePage(bool leaf) {
    PageId id = allocatePage();
    Page page;
    if (leaf)
        BTreePage::initLeaf(page);
    else
        BTreePage::initInternal(page);
    savePage(id, page);
    return id;
}

class StorageBTreePageStore : public BTreePageStore {
public:
    explicit StorageBTreePageStore(Storage& storage) : storage_(storage) {}

    Page load(PageId id) const override { return storage_.loadPage(id); }
    void save(PageId id, const Page& page) override { storage_.savePage(id, page); }
    PageId allocateLeaf() override { return storage_.allocateBTreePage(true); }
    PageId allocateInternal() override { return storage_.allocateBTreePage(false); }

private:
    Storage& storage_;
};

std::unique_ptr<BTreePageStore> makeStorageBTreePageStore(Storage& storage) {
    return std::make_unique<StorageBTreePageStore>(storage);
}

PageId Storage::allocateHeapPage() {
    PageId id = allocatePage();
    Page page;
    HeapPage::init(page);
    savePage(id, page);
    return id;
}

void Storage::freeHeapChain(PageId firstPage) {
    while (firstPage != 0) {
        Page page = loadPage(firstPage);
        if (!HeapPage::isHeapPage(page))
            throw std::runtime_error("Storage: cannot free non-heap page " + std::to_string(firstPage));
        PageId next = HeapPage::nextPage(page);
        freelistPush(firstPage);
        firstPage = next;
    }
    saveBootstrap();
}

SystemCatalog::ScanChainFn Storage::scanFn() const {
    return [this](PageId first, const std::function<void(const Row&)>& fn) {
        scanHeapChain(first, fn);
    };
}

SystemCatalog::AppendRowFn Storage::appendFn() {
    return [this](PageId& firstPage, const Row& row) {
        if (!appendRowToChain(firstPage, row))
            throw std::runtime_error("Storage: failed to append catalog row");
    };
}

SystemCatalog::WriteChainFn Storage::writeChainFn() {
    return [this](const std::vector<Row>& rows) { return writeRowsToNewChain(rows); };
}

void Storage::createTable(const TableSchema& schema) {
    if (!disk.isOpen())
        throw std::runtime_error("Storage: no database is open");
    if (tables.count(schema.name))
        throw std::runtime_error("Storage: table '" + schema.name + "' already has a heap");

    PageId firstPage = allocateHeapPage();

    TableDirectoryEntry entry;
    entry.schema = schema;
    entry.firstHeapPage = firstPage;
    tables[schema.name] = entry;

    SystemCatalog::insertTable(schema, firstPage, boot, appendFn());
}

PageId Storage::findLastPage(PageId firstPage) const {
    PageId current = firstPage;
    while (true) {
        Page page = loadPage(current);
        if (!HeapPage::isHeapPage(page))
            throw std::runtime_error("Storage: page " + std::to_string(current) + " is not a heap page");
        PageId next = HeapPage::nextPage(page);
        if (next == 0) return current;
        current = next;
    }
}

bool Storage::appendRowToChain(PageId& firstPage, const Row& row, RowId* outRid) {
    if (firstPage == 0) return false;

    PageId lastId = findLastPage(firstPage);
    Page page = loadPage(lastId);

    if (auto slot = HeapPage::insertRow(page, row)) {
        savePage(lastId, page);
        if (outRid) {
            outRid->pageId = lastId;
            outRid->slotIndex = *slot;
        }
        return true;
    }

    PageId newId = allocateHeapPage();
    Page newPage = loadPage(newId);
    auto slot = HeapPage::insertRow(newPage, row);
    if (!slot.has_value())
        throw std::runtime_error("Storage: row too large to fit on an empty heap page");

    HeapPage::setNextPage(page, newId);
    savePage(lastId, page);
    savePage(newId, newPage);
    if (outRid) {
        outRid->pageId = newId;
        outRid->slotIndex = *slot;
    }
    return true;
}

PageId Storage::writeRowsToNewChain(const std::vector<Row>& rows) {
    PageId firstPage = allocateHeapPage();
    PageId currentId = firstPage;
    Page page = loadPage(currentId);

    for (const Row& row : rows) {
        if (!HeapPage::insertRow(page, row).has_value()) {
            savePage(currentId, page);
            PageId newId = allocateHeapPage();
            HeapPage::setNextPage(page, newId);
            savePage(currentId, page);
            currentId = newId;
            page = loadPage(currentId);
            if (!HeapPage::insertRow(page, row).has_value())
                throw std::runtime_error("Storage: row too large to fit on an empty heap page");
        }
    }
    savePage(currentId, page);
    return firstPage;
}

void Storage::writeRowsToChain(PageId& firstPage, const std::vector<Row>& rows) {
    PageId oldFirst = firstPage;
    firstPage = writeRowsToNewChain(rows);
    if (oldFirst != 0)
        freeHeapChain(oldFirst);
}

void Storage::scanHeapChain(PageId firstPage, const std::function<void(const Row&)>& fn) const {
    PageId current = firstPage;
    while (current != 0) {
        Page page = loadPage(current);
        if (!HeapPage::isHeapPage(page))
            throw std::runtime_error("Storage: page " + std::to_string(current) + " is not a heap page");

        for (auto& slot : HeapPage::liveSlots(page)) {
            Row row = RowCodec::decodeRow(slot.bytes.data(), slot.bytes.size());
            fn(row);
        }
        current = HeapPage::nextPage(page);
    }
}

void Storage::scanHeapChainWithRids(
    PageId firstPage, const std::function<void(PageId, uint16_t, const Row&)>& fn) const {
    PageId current = firstPage;
    while (current != 0) {
        Page page = loadPage(current);
        if (!HeapPage::isHeapPage(page))
            throw std::runtime_error("Storage: page " + std::to_string(current) + " is not a heap page");

        for (auto& slot : HeapPage::liveSlots(page)) {
            Row row = RowCodec::decodeRow(slot.bytes.data(), slot.bytes.size());
            fn(current, slot.index, row);
        }
        current = HeapPage::nextPage(page);
    }
}

void Storage::insertRow(const std::string& tableName, Row row) {
    TableDirectoryEntry& entry = requireTable(tableName);
    RowId rid;
    if (!appendRowToChain(entry.firstHeapPage, row, &rid))
        throw std::runtime_error("Storage: insert failed for table '" + tableName + "'");
    maintainIndexesOnInsert(tableName, row, rid);
}

std::vector<Row> Storage::scanTable(const std::string& tableName) const {
    const TableDirectoryEntry& entry = requireTable(tableName);
    std::vector<Row> rows;
    scanHeapChain(entry.firstHeapPage, [&](const Row& row) { rows.push_back(row); });
    return rows;
}

void Storage::scanTableWithRids(const std::string& tableName,
                                const std::function<void(RowId, const Row&)>& fn) const {
    const TableDirectoryEntry& entry = requireTable(tableName);
    scanHeapChainWithRids(entry.firstHeapPage, [&](PageId pageId, uint16_t slot, const Row& row) {
        fn(RowId{pageId, slot}, row);
    });
}

bool Storage::tryFetchRowByRid(RowId rid, Row& out) const {
    Page page = loadPage(rid.pageId);
    if (!HeapPage::isHeapPage(page) || !HeapPage::isLiveSlot(page, rid.slotIndex))
        return false;
    out = HeapPage::getRow(page, rid.slotIndex);
    return true;
}

bool Storage::updateRow(const std::string& tableName, RowId rid, Row newRow) {
    requireTable(tableName);
    Row oldRow;
    if (!tryFetchRowByRid(rid, oldRow))
        return false;

    Page page = loadPage(rid.pageId);
    if (!HeapPage::updateRow(page, rid.slotIndex, newRow))
        throw std::runtime_error("Storage: updated row too large to fit on heap page");

    savePage(rid.pageId, page);
    maintainIndexesOnUpdate(tableName, oldRow, newRow, rid);
    return true;
}

bool Storage::deleteRow(const std::string& tableName, RowId rid) {
    requireTable(tableName);
    Row oldRow;
    if (!tryFetchRowByRid(rid, oldRow))
        return false;

    Page page = loadPage(rid.pageId);
    if (!HeapPage::remove(page, rid.slotIndex))
        return false;

    savePage(rid.pageId, page);
    maintainIndexesOnDelete(tableName, oldRow, rid);
    return true;
}

void Storage::rewriteTable(const std::string& tableName, std::vector<Row> rows) {
    TableDirectoryEntry& entry = requireTable(tableName);
    writeRowsToChain(entry.firstHeapPage, rows);

    PageId oldCatalogRoot = boot.tablesRoot;
    boot.tablesRoot = SystemCatalog::rewriteTablesCatalog(tables, writeChainFn());
    if (oldCatalogRoot != 0)
        freeHeapChain(oldCatalogRoot);
    saveBootstrap();
    rebuildIndexesForTable(tableName);
}

const IndexEntry* Storage::findIndex(const std::string& name) const {
    auto it = indexes_.find(name);
    if (it == indexes_.end()) return nullptr;
    return &it->second;
}

const IndexEntry* Storage::findIndexOnColumn(const std::string& table,
                                             const std::string& column) const {
    for (const auto& [name, entry] : indexes_) {
        (void)name;
        if (entry.tableName == table && entry.columnName == column)
            return &entry;
    }
    return nullptr;
}

IndexEntry& Storage::requireIndex(const std::string& indexName) {
    auto it = indexes_.find(indexName);
    if (it == indexes_.end())
        throw std::runtime_error("Storage: index '" + indexName + "' not found");
    return it->second;
}

BTree Storage::openIndexTree(const IndexEntry& entry) {
    size_t keySize = IndexKey::keySizeForType(entry.columnType);
    return BTree(makeStorageBTreePageStore(*this), entry.rootPage, BTREE_DEFAULT_MAX_KEYS, keySize);
}

void Storage::persistIndexRoot(IndexEntry& entry, PageId root) {
    entry.rootPage = root;
    PageId oldRoot = boot.indexesRoot;
    boot.indexesRoot = IndexCatalog::rewriteIndexesCatalog(indexes_, writeChainFn());
    if (oldRoot != 0 && oldRoot != boot.indexesRoot)
        freeHeapChain(oldRoot);
    saveBootstrap();
}

void Storage::indexInsert(const IndexEntry& entry, const Value& keyVal, RowId rid) {
    if (!valueIsIndexable(keyVal, entry.columnType)) return;

    IndexEntry& mutableEntry = requireIndex(entry.name);
    size_t keySize = IndexKey::keySizeForType(entry.columnType);
    uint8_t key[IndexKey::MAX_KEY_SIZE];
    encodeIndexKey(keyVal, entry.columnType, key, keySize);

    BTree tree = openIndexTree(mutableEntry);
    tree.insert(key, rid);
    if (tree.root() != mutableEntry.rootPage)
        persistIndexRoot(mutableEntry, tree.root());
}

void Storage::indexRemove(const IndexEntry& entry, const Value& keyVal, RowId rid) {
    if (!valueIsIndexable(keyVal, entry.columnType)) return;

    IndexEntry& mutableEntry = requireIndex(entry.name);
    size_t keySize = IndexKey::keySizeForType(entry.columnType);
    uint8_t key[IndexKey::MAX_KEY_SIZE];
    encodeIndexKey(keyVal, entry.columnType, key, keySize);

    BTree tree = openIndexTree(mutableEntry);
    tree.removeEqual(key, rid);
    if (tree.root() != mutableEntry.rootPage)
        persistIndexRoot(mutableEntry, tree.root());
}

void Storage::maintainIndexesOnInsert(const std::string& tableName, const Row& row, RowId rid) {
    for (auto& [name, entry] : indexes_) {
        (void)name;
        if (entry.tableName != tableName) continue;
        if (entry.columnIndex < 0 || static_cast<size_t>(entry.columnIndex) >= row.size()) continue;
        indexInsert(entry, row[entry.columnIndex], rid);
    }
}

static bool indexKeyValueEqual(const Value& a, const Value& b, Type columnType) {
    if (a.type == Type::NUL || b.type == Type::NUL) return a.type == b.type;
    if (columnType == Type::INT) {
        long long av = (a.type == Type::INT) ? std::get<long long>(a.data) : 0;
        long long bv = (b.type == Type::INT) ? std::get<long long>(b.data) : 0;
        return av == bv;
    }
    if (columnType == Type::BOOL) {
        return a.type == Type::BOOL && b.type == Type::BOOL &&
               std::get<bool>(a.data) == std::get<bool>(b.data);
    }
    if (columnType == Type::FLOAT) {
        double av = (a.type == Type::FLOAT) ? std::get<double>(a.data)
                                            : static_cast<double>(std::get<long long>(a.data));
        double bv = (b.type == Type::FLOAT) ? std::get<double>(b.data)
                                            : static_cast<double>(std::get<long long>(b.data));
        return av == bv;
    }
    if (columnType == Type::TEXT) {
        return a.type == Type::TEXT && b.type == Type::TEXT &&
               std::get<std::string>(a.data) == std::get<std::string>(b.data);
    }
    return false;
}

void Storage::maintainIndexesOnUpdate(const std::string& tableName, const Row& oldRow,
                                      const Row& newRow, RowId rid) {
    for (auto& [name, entry] : indexes_) {
        (void)name;
        if (entry.tableName != tableName) continue;
        if (entry.columnIndex < 0) continue;
        if (static_cast<size_t>(entry.columnIndex) >= oldRow.size() ||
            static_cast<size_t>(entry.columnIndex) >= newRow.size())
            continue;

        const Value& oldVal = oldRow[entry.columnIndex];
        const Value& newVal = newRow[entry.columnIndex];
        if (indexKeyValueEqual(oldVal, newVal, entry.columnType))
            continue;

        if (valueIsIndexable(oldVal, entry.columnType))
            indexRemove(entry, oldVal, rid);
        if (valueIsIndexable(newVal, entry.columnType))
            indexInsert(entry, newVal, rid);
    }
}

void Storage::maintainIndexesOnDelete(const std::string& tableName, const Row& row, RowId rid) {
    for (auto& [name, entry] : indexes_) {
        (void)name;
        if (entry.tableName != tableName) continue;
        if (entry.columnIndex < 0 || static_cast<size_t>(entry.columnIndex) >= row.size()) continue;
        const Value& keyVal = row[entry.columnIndex];
        if (!valueIsIndexable(keyVal, entry.columnType)) continue;
        indexRemove(entry, keyVal, rid);
    }
}

void Storage::freeBTreePages(PageId root, size_t keySize) {
    if (root == 0) return;
    Page page = loadPage(root);
    if (!BTreePage::isBTreePage(page)) return;

    if (BTreePage::isLeaf(page)) {
        freelistPush(root);
        return;
    }

    PageId left = BTreePage::leftChild(page);
    auto [keys, children] = BTreePage::readAllInternalEntries(page, keySize);
    (void)keys;
    freeBTreePages(left, keySize);
    for (PageId child : children)
        freeBTreePages(child, keySize);
    freelistPush(root);
}

void Storage::rebuildIndexesForTable(const std::string& tableName) {
    const TableDirectoryEntry& table = requireTable(tableName);

    for (auto& [name, entry] : indexes_) {
        (void)name;
        if (entry.tableName != tableName) continue;

        PageId oldRoot = entry.rootPage;
        size_t keySize = IndexKey::keySizeForType(entry.columnType);
        BTree tree(makeStorageBTreePageStore(*this), 0, BTREE_DEFAULT_MAX_KEYS, keySize);
        const int columnIndex = entry.columnIndex;
        const Type columnType = entry.columnType;

        scanHeapChainWithRids(table.firstHeapPage, [&](PageId pageId, uint16_t slot, const Row& row) {
            if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= row.size()) return;
            const Value& keyVal = row[columnIndex];
            if (!valueIsIndexable(keyVal, columnType)) return;
            uint8_t key[IndexKey::MAX_KEY_SIZE];
            encodeIndexKey(keyVal, columnType, key, keySize);
            tree.insert(key, {pageId, slot});
        });

        PageId newRoot = tree.root();
        if (oldRoot != 0 && oldRoot != newRoot)
            freeBTreePages(oldRoot, keySize);
        entry.rootPage = newRoot;
    }

    PageId oldCatalogRoot = boot.indexesRoot;
    boot.indexesRoot = IndexCatalog::rewriteIndexesCatalog(indexes_, writeChainFn());
    if (oldCatalogRoot != 0 && oldCatalogRoot != boot.indexesRoot)
        freeHeapChain(oldCatalogRoot);
    saveBootstrap();
}

void Storage::createIndex(const std::string& indexName, const std::string& tableName,
                          const std::string& columnName) {
    if (!disk.isOpen())
        throw std::runtime_error("Storage: no database is open");
    if (indexes_.count(indexName))
        throw std::runtime_error("Storage: index '" + indexName + "' already exists");

    const TableDirectoryEntry& table = requireTable(tableName);
    const ColumnSchema* col = table.schema.findColumn(columnName);
    if (!col)
        throw std::runtime_error("Storage: column '" + columnName + "' not found on '" + tableName + "'");
    if (!IndexKey::isIndexableType(col->type))
        throw std::runtime_error("Storage: column type cannot be indexed");
    if (findIndexOnColumn(tableName, columnName))
        throw std::runtime_error("Storage: index on " + tableName + "." + columnName + " already exists");

    IndexEntry entry;
    entry.name = indexName;
    entry.tableName = tableName;
    entry.columnName = columnName;
    entry.columnIndex = col->index;
    entry.columnType = col->type;
    entry.rootPage = 0;

    size_t keySize = IndexKey::keySizeForType(col->type);
    BTree tree(makeStorageBTreePageStore(*this), 0, BTREE_DEFAULT_MAX_KEYS, keySize);
    scanHeapChainWithRids(table.firstHeapPage, [&](PageId pageId, uint16_t slot, const Row& row) {
        if (static_cast<size_t>(col->index) >= row.size()) return;
        const Value& keyVal = row[col->index];
        if (!valueIsIndexable(keyVal, col->type)) return;
        uint8_t key[IndexKey::MAX_KEY_SIZE];
        encodeIndexKey(keyVal, col->type, key, keySize);
        tree.insert(key, {pageId, slot});
    });
    entry.rootPage = tree.root();

    indexes_[indexName] = entry;
    PageId oldRoot = boot.indexesRoot;
    boot.indexesRoot = IndexCatalog::rewriteIndexesCatalog(indexes_, writeChainFn());
    if (oldRoot != 0 && oldRoot != boot.indexesRoot)
        freeHeapChain(oldRoot);
    saveBootstrap();
}

Row Storage::fetchRowByRid(PageId heapRoot, RowId rid) const {
    (void)heapRoot;
    Page page = loadPage(rid.pageId);
    if (!HeapPage::isHeapPage(page))
        throw std::runtime_error("Storage: RID points to non-heap page");
    return HeapPage::getRow(page, rid.slotIndex);
}

std::vector<Row> Storage::indexSearchRows(const std::string& indexName, IndexCompareOp op,
                                            const Value& bound) const {
    const IndexEntry* entry = findIndex(indexName);
    if (!entry)
        throw std::runtime_error("Storage: index '" + indexName + "' not found");
    if (!valueIsIndexable(bound, entry->columnType))
        throw std::runtime_error("Storage: search bound is not indexable for this column type");

    size_t keySize = IndexKey::keySizeForType(entry->columnType);
    uint8_t key[IndexKey::MAX_KEY_SIZE];
    encodeIndexKey(bound, entry->columnType, key, keySize);

    BTree tree(makeStorageBTreePageStore(const_cast<Storage&>(*this)),
               entry->rootPage, BTREE_DEFAULT_MAX_KEYS, keySize);

    std::vector<RowId> rids;
    BTreeBound lo;
    BTreeBound hi;

    switch (op) {
        case IndexCompareOp::Eq:
            rids = tree.lookupEqual(key);
            break;
        case IndexCompareOp::Lt:
            lo.unbounded = true;
            hi.unbounded = false;
            std::memcpy(hi.key.data(), key, keySize);
            hi.inclusive = false;
            rids = tree.rangeScan(lo, hi);
            break;
        case IndexCompareOp::Le:
            lo.unbounded = true;
            hi.unbounded = false;
            std::memcpy(hi.key.data(), key, keySize);
            hi.inclusive = true;
            rids = tree.rangeScan(lo, hi);
            break;
        case IndexCompareOp::Gt:
            lo.unbounded = false;
            std::memcpy(lo.key.data(), key, keySize);
            lo.inclusive = false;
            hi.unbounded = true;
            rids = tree.rangeScan(lo, hi);
            break;
        case IndexCompareOp::Ge:
            lo.unbounded = false;
            std::memcpy(lo.key.data(), key, keySize);
            lo.inclusive = true;
            hi.unbounded = true;
            rids = tree.rangeScan(lo, hi);
            break;
        case IndexCompareOp::Ne: {
            lo.unbounded = true;
            hi.unbounded = false;
            std::memcpy(hi.key.data(), key, keySize);
            hi.inclusive = false;
            rids = tree.rangeScan(lo, hi);
            lo.unbounded = false;
            std::memcpy(lo.key.data(), key, keySize);
            lo.inclusive = false;
            hi.unbounded = true;
            auto upper = tree.rangeScan(lo, hi);
            rids.insert(rids.end(), upper.begin(), upper.end());
            break;
        }
    }

    std::vector<Row> rows;
    rows.reserve(rids.size());
    for (const RowId& rid : rids) {
        Row row;
        if (tryFetchRowByRid(rid, row))
            rows.push_back(std::move(row));
    }
    return rows;
}
