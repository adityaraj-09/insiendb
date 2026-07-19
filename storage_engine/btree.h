#pragma once
#include "common.h"
#include "index_key.h"
#include "page.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct RowId {
    PageId pageId = 0;
    uint16_t slotIndex = 0;

    bool operator==(const RowId& o) const {
        return pageId == o.pageId && slotIndex == o.slotIndex;
    }
};

class BTreePageStore {
public:
    virtual ~BTreePageStore() = default;
    virtual Page load(PageId id) const = 0;
    virtual void save(PageId id, const Page& page) = 0;
    virtual PageId allocateLeaf() = 0;
    virtual PageId allocateInternal() = 0;
};

class MemoryBTreePageStore : public BTreePageStore {
public:
    Page load(PageId id) const override;
    void save(PageId id, const Page& page) override;
    PageId allocateLeaf() override;
    PageId allocateInternal() override;

    size_t pageCount() const { return pages_.size(); }

private:
    mutable std::unordered_map<PageId, Page> pages_;
    PageId nextPageId_ = 1;
};

class BTreePage {
public:
    static constexpr uint32_t BTREE_MAGIC = BTREE_PAGE_MAGIC;
    static constexpr size_t HEADER_SIZE = 16;
    static constexpr size_t RID_SIZE = 6;

    static constexpr size_t OFF_MAGIC = 0;
    static constexpr size_t OFF_FLAGS = 4;
    static constexpr size_t OFF_NUM_KEYS = 6;
    static constexpr size_t OFF_PARENT = 8;
    static constexpr size_t OFF_LINK = 12;

    static constexpr uint16_t FLAG_LEAF = 0x0001;

    static size_t leafEntrySize(size_t keySize) { return keySize + RID_SIZE; }
    static size_t internalEntrySize(size_t keySize) { return keySize + 4; }

    static void initLeaf(Page& page);
    static void initInternal(Page& page);

    static bool isBTreePage(const Page& page);
    static bool isLeaf(const Page& page);
    static uint16_t numKeys(const Page& page);
    static PageId parent(const Page& page);
    static void setParent(Page& page, PageId parentId);

    static PageId nextLeaf(const Page& page);
    static void setNextLeaf(Page& page, PageId next);

    static PageId leftChild(const Page& page);
    static void setLeftChild(Page& page, PageId child);

    static void readLeafKey(const Page& page, uint16_t index, size_t keySize, uint8_t* out);
    static RowId leafRidAt(const Page& page, uint16_t index, size_t keySize);
    static void writeLeafEntry(Page& page, uint16_t index, const uint8_t* key, size_t keySize, RowId rid);

    static void readInternalKey(const Page& page, uint16_t index, size_t keySize, uint8_t* out);
    static PageId internalChildAt(const Page& page, uint16_t index, size_t keySize);

    static size_t leafBytesUsed(uint16_t numKeys, size_t keySize);
    static size_t internalBytesUsed(uint16_t numKeys, size_t keySize);
    static bool leafHasRoom(const Page& page, uint16_t maxKeys, size_t keySize);
    static bool internalHasRoom(const Page& page, uint16_t maxKeys, size_t keySize);

    static uint16_t findLeafInsertPos(const Page& page, const uint8_t* key, size_t keySize);
    static uint16_t findChildIndex(const Page& page, const uint8_t* key, size_t keySize);

    static void insertLeafEntry(Page& page, uint16_t pos, const uint8_t* key, size_t keySize, RowId rid);

    static std::vector<std::pair<std::vector<uint8_t>, RowId>> readAllLeafEntries(const Page& page,
                                                                                    size_t keySize);
    static void writeAllLeafEntries(Page& page, size_t keySize,
                                    const std::vector<std::pair<std::vector<uint8_t>, RowId>>& entries);

    static std::pair<std::vector<std::vector<uint8_t>>, std::vector<PageId>>
    readAllInternalEntries(const Page& page, size_t keySize);
    static void writeAllInternalEntries(Page& page, PageId leftChild, size_t keySize,
                                        const std::vector<std::vector<uint8_t>>& keys,
                                        const std::vector<PageId>& children);

private:
    static size_t leafEntryOffset(uint16_t index, size_t keySize);
    static size_t internalEntryOffset(uint16_t index, size_t keySize);
};

struct BTreeBound {
    bool unbounded = true;
    std::array<uint8_t, IndexKey::MAX_KEY_SIZE> key{};
    bool inclusive = true;
};

class BTree {
public:
    explicit BTree(uint16_t maxKeysPerPage = 3, size_t keySize = IndexKey::KEY_SIZE_NUMERIC);
    BTree(std::unique_ptr<BTreePageStore> store, PageId root, uint16_t maxKeysPerPage,
          size_t keySize = IndexKey::KEY_SIZE_NUMERIC);

    PageId root() const { return root_; }
    void setRoot(PageId root) { root_ = root; }
    uint16_t maxKeys() const { return maxKeys_; }
    size_t keySize() const { return keySize_; }

    size_t pageCount() const;
    Page page(PageId id) const;

    void insert(const uint8_t* key, RowId rid);
    bool removeEqual(const uint8_t* key, RowId rid);
    std::vector<RowId> lookupEqual(const uint8_t* key) const;
    std::vector<RowId> rangeScan(const BTreeBound& lo, const BTreeBound& hi) const;

private:
    PageId root_ = 0;
    uint16_t maxKeys_;
    size_t keySize_;
    BTreePageStore* store_ = nullptr;
    std::unique_ptr<BTreePageStore> ownedExternalStore_;
    std::unique_ptr<MemoryBTreePageStore> ownedMemoryStore_;

    PageId findLeafPage(PageId pageId, const uint8_t* key) const;
    PageId leftmostLeaf() const;
    void insertIntoLeaf(PageId leafId, const uint8_t* key, RowId rid);
    bool removeFromLeaf(PageId leafId, const uint8_t* key, RowId rid);
    void insertAfterChild(PageId parentId, PageId leftPage, const uint8_t* sepKey, PageId rightPage);
    void splitLeaf(PageId leafId, const uint8_t* key, RowId rid);
    void splitInternal(PageId internalId, PageId leftPage, const uint8_t* sepKey, PageId rightPage);

    bool keyInRange(const uint8_t* key, const BTreeBound& lo, const BTreeBound& hi) const;

    static std::vector<PageId> orderedChildren(const Page& internal, size_t keySize);
    static void writeInternalFromChildren(Page& internal, size_t keySize,
                                          const std::vector<std::vector<uint8_t>>& keys,
                                          const std::vector<PageId>& childOrder);
};

class Storage;

std::unique_ptr<BTreePageStore> makeStorageBTreePageStore(Storage& storage);
