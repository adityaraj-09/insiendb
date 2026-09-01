#pragma once
#include "value.h"
#include "catalog.h"
#include "storage_engine/disk_manager.h"
#include "storage_engine/file_header.h"
#include "storage_engine/system_catalog.h"
#include "storage_engine/index_catalog.h"
#include "storage_engine/btree.h"
#include "storage_engine/wal.h"
#include <string>
#include <unordered_map>
#include <vector>

using Row = std::vector<Value>;

class Storage {
public:
    Storage() = default;
    ~Storage();

    void createDatabase(const std::string& path);
    void openDatabase(const std::string& path);
    void closeDatabase();

    void abandonWithoutCheckpoint();

    bool isOpen() const { return disk.isOpen(); }
    const std::string& path() const { return dbPath; }
    const std::string& walPath() const { return walFilePath; }

    void createTable(const TableSchema& schema);
    void createIndex(const std::string& indexName, const std::string& tableName,
                     const std::string& columnName);

    void insertRow(const std::string& tableName, Row row);
    bool updateRow(const std::string& tableName, RowId rid, Row newRow);
    bool deleteRow(const std::string& tableName, RowId rid);
    std::vector<Row> scanTable(const std::string& tableName) const;
    void scanTableWithRids(const std::string& tableName,
                           const std::function<void(RowId, const Row&)>& fn) const;
    void rewriteTable(const std::string& tableName, std::vector<Row> rows);

    Row fetchRowByRid(PageId heapRoot, RowId rid) const;
    std::vector<Row> indexSearchRows(const std::string& indexName, IndexCompareOp op,
                                     const Value& bound) const;

    std::vector<TableSchema> getAllSchemas() const;
    const IndexEntry* findIndex(const std::string& name) const;
    const IndexEntry* findIndexOnColumn(const std::string& table,
                                        const std::string& column) const;
    const std::unordered_map<std::string, IndexEntry>& indexes() const { return indexes_; }

    PageId pageCount() const { return disk.pageCount(); }
    PageId freelistHead() const { return boot.freelistHead; }
    LSN walNextLsn() const { return wal.nextLsn(); }
    LSN walRedoLsn() const { return wal.redoLsn(); }

private:
    DiskManager disk;
    WalManager wal;
    std::string dbPath;
    std::string walFilePath;
    CatalogBootstrap boot;
    std::unordered_map<std::string, TableDirectoryEntry> tables;
    std::unordered_map<std::string, IndexEntry> indexes_;
    bool recovering = false;

    const TableDirectoryEntry& requireTable(const std::string& tableName) const;
    TableDirectoryEntry& requireTable(const std::string& tableName);
    IndexEntry& requireIndex(const std::string& indexName);

    Page loadPage(PageId id) const;
    void persistPage(PageId id, const Page& page);
    void savePage(PageId id, const Page& page);
    void commitHeapChange(PageId id, Page& page, WalRecordType type,
                          const std::vector<uint8_t>& payload, bool needFpi);
    bool heapNeedsFpi(const Page& page) const;
    bool heapAlreadyApplied(PageId id, LSN lsn) const;
    void applyWalRecord(const WalRecord& rec);
    void applyHeapRedo(const WalRecord& rec);
    void saveBootstrap();
    void recoverFromWal();
    void checkpoint();

    PageId allocateHeapPage();
    PageId allocateBTreePage(bool leaf);
    PageId allocatePage();
    PageId freelistPop();
    void freelistPush(PageId id);
    void freeHeapChain(PageId firstPage);
    void freeBTreePages(PageId root, size_t keySize);
    PageId findLastPage(PageId firstPage) const;
    bool appendRowToChain(PageId& firstPage, const Row& row, RowId* outRid = nullptr);
    PageId writeRowsToNewChain(const std::vector<Row>& rows);
    void writeRowsToChain(PageId& firstPage, const std::vector<Row>& rows);
    void scanHeapChain(PageId firstPage, const std::function<void(const Row&)>& fn) const;
    void scanHeapChainWithRids(PageId firstPage,
                               const std::function<void(PageId, uint16_t, const Row&)>& fn) const;

    BTree openIndexTree(const IndexEntry& entry);
    void persistIndexRoot(IndexEntry& entry, PageId root);
    void indexInsert(const IndexEntry& entry, const Value& keyVal, RowId rid);
    void indexRemove(const IndexEntry& entry, const Value& keyVal, RowId rid);
    void maintainIndexesOnInsert(const std::string& tableName, const Row& row, RowId rid);
    void maintainIndexesOnUpdate(const std::string& tableName, const Row& oldRow,
                                 const Row& newRow, RowId rid);
    void maintainIndexesOnDelete(const std::string& tableName, const Row& row, RowId rid);
    void rebuildIndexesForTable(const std::string& tableName);

    bool tryFetchRowByRid(RowId rid, Row& out) const;

    SystemCatalog::ScanChainFn scanFn() const;
    SystemCatalog::AppendRowFn appendFn();
    SystemCatalog::WriteChainFn writeChainFn();

    friend class StorageBTreePageStore;
};
