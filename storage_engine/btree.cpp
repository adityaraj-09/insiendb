#include "btree.h"
#include "index_key.h"
#include <algorithm>
#include <stdexcept>

size_t BTreePage::leafEntryOffset(uint16_t index, size_t keySize) {
    return HEADER_SIZE + static_cast<size_t>(index) * leafEntrySize(keySize);
}

size_t BTreePage::internalEntryOffset(uint16_t index, size_t keySize) {
    return HEADER_SIZE + static_cast<size_t>(index) * internalEntrySize(keySize);
}

void BTreePage::initLeaf(Page& page) {
    page.zero();
    page.writeU32(OFF_MAGIC, BTREE_MAGIC);
    page.writeU16(OFF_FLAGS, FLAG_LEAF);
    page.writeU16(OFF_NUM_KEYS, 0);
    page.writeU32(OFF_PARENT, 0);
    page.writeU32(OFF_LINK, 0);
}

void BTreePage::initInternal(Page& page) {
    page.zero();
    page.writeU32(OFF_MAGIC, BTREE_MAGIC);
    page.writeU16(OFF_FLAGS, 0);
    page.writeU16(OFF_NUM_KEYS, 0);
    page.writeU32(OFF_PARENT, 0);
    page.writeU32(OFF_LINK, 0);
}

bool BTreePage::isBTreePage(const Page& page) {
    return page.readU32(OFF_MAGIC) == BTREE_MAGIC;
}

bool BTreePage::isLeaf(const Page& page) {
    return isBTreePage(page) && (page.readU16(OFF_FLAGS) & FLAG_LEAF) != 0;
}

uint16_t BTreePage::numKeys(const Page& page) {
    return page.readU16(OFF_NUM_KEYS);
}

PageId BTreePage::parent(const Page& page) {
    return page.readU32(OFF_PARENT);
}

void BTreePage::setParent(Page& page, PageId parentId) {
    page.writeU32(OFF_PARENT, parentId);
}

PageId BTreePage::nextLeaf(const Page& page) {
    return page.readU32(OFF_LINK);
}

void BTreePage::setNextLeaf(Page& page, PageId next) {
    page.writeU32(OFF_LINK, next);
}

PageId BTreePage::leftChild(const Page& page) {
    return page.readU32(OFF_LINK);
}

void BTreePage::setLeftChild(Page& page, PageId child) {
    page.writeU32(OFF_LINK, child);
}

void BTreePage::readLeafKey(const Page& page, uint16_t index, size_t keySize, uint8_t* out) {
    page.readBytes(leafEntryOffset(index, keySize), out, keySize);
}

RowId BTreePage::leafRidAt(const Page& page, uint16_t index, size_t keySize) {
    size_t off = leafEntryOffset(index, keySize) + keySize;
    RowId rid;
    rid.pageId = page.readU32(off);
    rid.slotIndex = page.readU16(off + 4);
    return rid;
}

void BTreePage::writeLeafEntry(Page& page, uint16_t index, const uint8_t* key, size_t keySize, RowId rid) {
    size_t off = leafEntryOffset(index, keySize);
    page.writeBytes(off, key, keySize);
    page.writeU32(off + keySize, rid.pageId);
    page.writeU16(off + keySize + 4, rid.slotIndex);
}

void BTreePage::readInternalKey(const Page& page, uint16_t index, size_t keySize, uint8_t* out) {
    page.readBytes(internalEntryOffset(index, keySize), out, keySize);
}

PageId BTreePage::internalChildAt(const Page& page, uint16_t index, size_t keySize) {
    return page.readU32(internalEntryOffset(index, keySize) + keySize);
}

size_t BTreePage::leafBytesUsed(uint16_t numKeys, size_t keySize) {
    return HEADER_SIZE + static_cast<size_t>(numKeys) * leafEntrySize(keySize);
}

size_t BTreePage::internalBytesUsed(uint16_t numKeys, size_t keySize) {
    return HEADER_SIZE + static_cast<size_t>(numKeys) * internalEntrySize(keySize);
}

bool BTreePage::leafHasRoom(const Page& page, uint16_t maxKeys, size_t keySize) {
    return numKeys(page) < maxKeys && leafBytesUsed(numKeys(page) + 1, keySize) <= PAGE_SIZE;
}

bool BTreePage::internalHasRoom(const Page& page, uint16_t maxKeys, size_t keySize) {
    return numKeys(page) < maxKeys && internalBytesUsed(numKeys(page) + 1, keySize) <= PAGE_SIZE;
}

uint16_t BTreePage::findLeafInsertPos(const Page& page, const uint8_t* key, size_t keySize) {
    uint16_t n = numKeys(page);
    uint16_t pos = 0;
    while (pos < n) {
        uint8_t existing[IndexKey::MAX_KEY_SIZE];
        readLeafKey(page, pos, keySize, existing);
        if (IndexKey::compare(existing, key, keySize) >= 0)
            break;
        pos++;
    }
    return pos;
}

uint16_t BTreePage::findChildIndex(const Page& page, const uint8_t* key, size_t keySize) {
    uint16_t n = numKeys(page);
    uint16_t idx = 0;
    while (idx < n) {
        uint8_t sep[IndexKey::MAX_KEY_SIZE];
        readInternalKey(page, idx, keySize, sep);
        if (IndexKey::compare(key, sep, keySize) < 0)
            break;
        idx++;
    }
    return idx;
}

void BTreePage::insertLeafEntry(Page& page, uint16_t pos, const uint8_t* key, size_t keySize, RowId rid) {
    auto entries = readAllLeafEntries(page, keySize);
    std::vector<uint8_t> keyBytes(key, key + keySize);
    entries.insert(entries.begin() + pos, {std::move(keyBytes), rid});
    writeAllLeafEntries(page, keySize, entries);
    page.writeU16(OFF_NUM_KEYS, static_cast<uint16_t>(entries.size()));
}

std::vector<std::pair<std::vector<uint8_t>, RowId>> BTreePage::readAllLeafEntries(const Page& page,
                                                                                    size_t keySize) {
    std::vector<std::pair<std::vector<uint8_t>, RowId>> entries;
    uint16_t n = numKeys(page);
    entries.reserve(n);
    for (uint16_t i = 0; i < n; i++) {
        std::vector<uint8_t> key(keySize);
        readLeafKey(page, i, keySize, key.data());
        entries.emplace_back(std::move(key), leafRidAt(page, i, keySize));
    }
    return entries;
}

void BTreePage::writeAllLeafEntries(Page& page, size_t keySize,
                                    const std::vector<std::pair<std::vector<uint8_t>, RowId>>& entries) {
    for (uint16_t i = 0; i < entries.size(); i++)
        writeLeafEntry(page, i, entries[i].first.data(), keySize, entries[i].second);
}

std::pair<std::vector<std::vector<uint8_t>>, std::vector<PageId>>
BTreePage::readAllInternalEntries(const Page& page, size_t keySize) {
    std::vector<std::vector<uint8_t>> keys;
    std::vector<PageId> children;
    uint16_t n = numKeys(page);
    keys.reserve(n);
    children.reserve(n);
    for (uint16_t i = 0; i < n; i++) {
        std::vector<uint8_t> key(keySize);
        readInternalKey(page, i, keySize, key.data());
        keys.push_back(std::move(key));
        children.push_back(internalChildAt(page, i, keySize));
    }
    return {keys, children};
}

void BTreePage::writeAllInternalEntries(Page& page, PageId leftChildId, size_t keySize,
                                        const std::vector<std::vector<uint8_t>>& keys,
                                        const std::vector<PageId>& children) {
    if (keys.size() != children.size())
        throw std::runtime_error("BTreePage: internal keys/children size mismatch");
    setLeftChild(page, leftChildId);
    for (uint16_t i = 0; i < keys.size(); i++) {
        size_t off = internalEntryOffset(i, keySize);
        page.writeBytes(off, keys[i].data(), keySize);
        page.writeU32(off + keySize, children[i]);
    }
    page.writeU16(OFF_NUM_KEYS, static_cast<uint16_t>(keys.size()));
}

Page MemoryBTreePageStore::load(PageId id) const {
    auto it = pages_.find(id);
    if (it == pages_.end())
        throw std::runtime_error("BTree: unknown page " + std::to_string(id));
    return it->second;
}

void MemoryBTreePageStore::save(PageId id, const Page& page) {
    pages_[id] = page;
}

PageId MemoryBTreePageStore::allocateLeaf() {
    PageId id = nextPageId_++;
    Page p;
    BTreePage::initLeaf(p);
    pages_.emplace(id, std::move(p));
    return id;
}

PageId MemoryBTreePageStore::allocateInternal() {
    PageId id = nextPageId_++;
    Page p;
    BTreePage::initInternal(p);
    pages_.emplace(id, std::move(p));
    return id;
}

std::vector<PageId> BTree::orderedChildren(const Page& internal, size_t keySize) {
    auto [keys, children] = BTreePage::readAllInternalEntries(internal, keySize);
    (void)keys;
    std::vector<PageId> order;
    order.push_back(BTreePage::leftChild(internal));
    order.insert(order.end(), children.begin(), children.end());
    return order;
}

void BTree::writeInternalFromChildren(Page& internal, size_t keySize,
                                      const std::vector<std::vector<uint8_t>>& keys,
                                      const std::vector<PageId>& childOrder) {
    if (childOrder.size() != keys.size() + 1)
        throw std::runtime_error("BTree: keys/children count mismatch");
    BTreePage::writeAllInternalEntries(internal, childOrder.front(), keySize, keys,
                                       std::vector<PageId>(childOrder.begin() + 1, childOrder.end()));
}

BTree::BTree(uint16_t maxKeysPerPage, size_t keySize)
    : maxKeys_(maxKeysPerPage), keySize_(keySize) {
    if (maxKeys_ < 2)
        throw std::invalid_argument("BTree: maxKeysPerPage must be >= 2");
    if (keySize_ == 0 || keySize_ > IndexKey::MAX_KEY_SIZE)
        throw std::invalid_argument("BTree: invalid key size");
    ownedMemoryStore_ = std::make_unique<MemoryBTreePageStore>();
    store_ = ownedMemoryStore_.get();
}

BTree::BTree(std::unique_ptr<BTreePageStore> store, PageId root, uint16_t maxKeysPerPage, size_t keySize)
    : root_(root), maxKeys_(maxKeysPerPage), keySize_(keySize),
      ownedExternalStore_(std::move(store)) {
    if (maxKeys_ < 2)
        throw std::invalid_argument("BTree: maxKeysPerPage must be >= 2");
    if (keySize_ == 0 || keySize_ > IndexKey::MAX_KEY_SIZE)
        throw std::invalid_argument("BTree: invalid key size");
    store_ = ownedExternalStore_.get();
}

size_t BTree::pageCount() const {
    if (auto* mem = dynamic_cast<const MemoryBTreePageStore*>(store_))
        return mem->pageCount();
    return 0;
}

Page BTree::page(PageId id) const {
    if (auto* mem = dynamic_cast<MemoryBTreePageStore*>(store_))
        return mem->load(id);
    throw std::runtime_error("BTree::page only supported for in-memory trees");
}

PageId BTree::leftmostLeaf() const {
    if (root_ == 0) return 0;
    PageId id = root_;
    while (true) {
        Page p = store_->load(id);
        if (BTreePage::isLeaf(p)) return id;
        id = BTreePage::leftChild(p);
    }
}

PageId BTree::findLeafPage(PageId pageId, const uint8_t* key) const {
    Page p = store_->load(pageId);
    if (BTreePage::isLeaf(p))
        return pageId;

    uint16_t idx = BTreePage::findChildIndex(p, key, keySize_);
    PageId child = (idx == 0) ? BTreePage::leftChild(p) : BTreePage::internalChildAt(p, idx - 1, keySize_);
    return findLeafPage(child, key);
}

void BTree::insertIntoLeaf(PageId leafId, const uint8_t* key, RowId rid) {
    Page leaf = store_->load(leafId);
    uint16_t pos = BTreePage::findLeafInsertPos(leaf, key, keySize_);
    BTreePage::insertLeafEntry(leaf, pos, key, keySize_, rid);
    store_->save(leafId, leaf);
}

void BTree::insertAfterChild(PageId parentId, PageId leftPage, const uint8_t* sepKey, PageId rightPage) {
    Page parent = store_->load(parentId);
    std::vector<PageId> childOrder = orderedChildren(parent, keySize_);

    auto it = std::find(childOrder.begin(), childOrder.end(), leftPage);
    if (it == childOrder.end())
        throw std::runtime_error("BTree: parent does not contain left page");
    size_t pos = static_cast<size_t>(it - childOrder.begin());

    auto [keys, children] = BTreePage::readAllInternalEntries(parent, keySize_);
    (void)children;
    keys.insert(keys.begin() + static_cast<long>(pos), std::vector<uint8_t>(sepKey, sepKey + keySize_));
    childOrder.insert(childOrder.begin() + static_cast<long>(pos + 1), rightPage);

    writeInternalFromChildren(parent, keySize_, keys, childOrder);
    store_->save(parentId, parent);

    Page right = store_->load(rightPage);
    BTreePage::setParent(right, parentId);
    store_->save(rightPage, right);
}

void BTree::splitLeaf(PageId leafId, const uint8_t* key, RowId rid) {
    Page leaf = store_->load(leafId);
    auto entries = BTreePage::readAllLeafEntries(leaf, keySize_);
    std::vector<uint8_t> keyBytes(key, key + keySize_);
    uint16_t pos = 0;
    while (pos < entries.size() &&
           IndexKey::compare(entries[pos].first.data(), key, keySize_) < 0)
        pos++;
    entries.insert(entries.begin() + pos, {keyBytes, rid});

    size_t splitAt = entries.size() / 2;
    std::vector<std::pair<std::vector<uint8_t>, RowId>> leftEntries(entries.begin(),
                                                                    entries.begin() + splitAt);
    std::vector<std::pair<std::vector<uint8_t>, RowId>> rightEntries(entries.begin() + splitAt,
                                                                     entries.end());

    const uint8_t* promoteKey = rightEntries.front().first.data();

    BTreePage::writeAllLeafEntries(leaf, keySize_, leftEntries);
    leaf.writeU16(BTreePage::OFF_NUM_KEYS, static_cast<uint16_t>(leftEntries.size()));
    store_->save(leafId, leaf);

    PageId rightId = store_->allocateLeaf();
    Page right = store_->load(rightId);
    BTreePage::writeAllLeafEntries(right, keySize_, rightEntries);
    right.writeU16(BTreePage::OFF_NUM_KEYS, static_cast<uint16_t>(rightEntries.size()));

    PageId oldNext = BTreePage::nextLeaf(leaf);
    BTreePage::setNextLeaf(leaf, rightId);
    BTreePage::setNextLeaf(right, oldNext);
    BTreePage::setParent(right, BTreePage::parent(leaf));
    store_->save(leafId, leaf);
    store_->save(rightId, right);

    PageId parentId = BTreePage::parent(leaf);
    if (parentId == 0) {
        PageId newRoot = store_->allocateInternal();
        Page root = store_->load(newRoot);
        writeInternalFromChildren(root, keySize_, {std::vector<uint8_t>(promoteKey, promoteKey + keySize_)},
                                  {leafId, rightId});
        store_->save(newRoot, root);

        leaf = store_->load(leafId);
        BTreePage::setParent(leaf, newRoot);
        store_->save(leafId, leaf);
        right = store_->load(rightId);
        BTreePage::setParent(right, newRoot);
        store_->save(rightId, right);
        root_ = newRoot;
        return;
    }

    Page parent = store_->load(parentId);
    if (BTreePage::internalHasRoom(parent, maxKeys_, keySize_)) {
        store_->save(parentId, parent);
        insertAfterChild(parentId, leafId, promoteKey, rightId);
        return;
    }
    store_->save(parentId, parent);
    splitInternal(parentId, leafId, promoteKey, rightId);
}

void BTree::splitInternal(PageId internalId, PageId leftPage, const uint8_t* sepKey, PageId rightPage) {
    Page node = store_->load(internalId);
    std::vector<PageId> childOrder = orderedChildren(node, keySize_);
    auto [keys, children] = BTreePage::readAllInternalEntries(node, keySize_);
    (void)children;

    auto it = std::find(childOrder.begin(), childOrder.end(), leftPage);
    if (it == childOrder.end())
        throw std::runtime_error("BTree: internal split missing left page");
    size_t pos = static_cast<size_t>(it - childOrder.begin());
    keys.insert(keys.begin() + static_cast<long>(pos), std::vector<uint8_t>(sepKey, sepKey + keySize_));
    childOrder.insert(childOrder.begin() + static_cast<long>(pos + 1), rightPage);

    size_t mid = keys.size() / 2;
    std::vector<uint8_t> promoteKey = keys[mid];

    std::vector<std::vector<uint8_t>> leftKeys(keys.begin(), keys.begin() + static_cast<long>(mid));
    std::vector<std::vector<uint8_t>> rightKeys(keys.begin() + static_cast<long>(mid + 1), keys.end());
    std::vector<PageId> leftChildren(childOrder.begin(), childOrder.begin() + static_cast<long>(mid + 1));
    std::vector<PageId> rightChildren(childOrder.begin() + static_cast<long>(mid + 1), childOrder.end());

    writeInternalFromChildren(node, keySize_, leftKeys, leftChildren);
    store_->save(internalId, node);
    for (PageId cid : leftChildren) {
        Page child = store_->load(cid);
        BTreePage::setParent(child, internalId);
        store_->save(cid, child);
    }

    PageId newRightId = store_->allocateInternal();
    Page newRight = store_->load(newRightId);
    writeInternalFromChildren(newRight, keySize_, rightKeys, rightChildren);
    store_->save(newRightId, newRight);
    for (PageId cid : rightChildren) {
        Page child = store_->load(cid);
        BTreePage::setParent(child, newRightId);
        store_->save(cid, child);
    }

    PageId parentId = BTreePage::parent(node);
    if (parentId == 0) {
        PageId newRoot = store_->allocateInternal();
        Page root = store_->load(newRoot);
        writeInternalFromChildren(root, keySize_, {promoteKey}, {internalId, newRightId});
        store_->save(newRoot, root);

        node = store_->load(internalId);
        BTreePage::setParent(node, newRoot);
        store_->save(internalId, node);
        newRight = store_->load(newRightId);
        BTreePage::setParent(newRight, newRoot);
        store_->save(newRightId, newRight);
        root_ = newRoot;
        return;
    }

    Page parent = store_->load(parentId);
    if (BTreePage::internalHasRoom(parent, maxKeys_, keySize_)) {
        store_->save(parentId, parent);
        insertAfterChild(parentId, internalId, promoteKey.data(), newRightId);
        return;
    }
    store_->save(parentId, parent);
    splitInternal(parentId, internalId, promoteKey.data(), newRightId);
}

void BTree::insert(const uint8_t* key, RowId rid) {
    if (root_ == 0) {
        root_ = store_->allocateLeaf();
        insertIntoLeaf(root_, key, rid);
        return;
    }

    PageId leafId = findLeafPage(root_, key);
    Page leaf = store_->load(leafId);
    if (BTreePage::leafHasRoom(leaf, maxKeys_, keySize_)) {
        insertIntoLeaf(leafId, key, rid);
        return;
    }

    splitLeaf(leafId, key, rid);
}

bool BTree::keyInRange(const uint8_t* key, const BTreeBound& lo, const BTreeBound& hi) const {
    if (!lo.unbounded) {
        int cmp = IndexKey::compare(key, lo.key.data(), keySize_);
        if (cmp < 0 || (cmp == 0 && !lo.inclusive))
            return false;
    }
    if (!hi.unbounded) {
        int cmp = IndexKey::compare(key, hi.key.data(), keySize_);
        if (cmp > 0 || (cmp == 0 && !hi.inclusive))
            return false;
    }
    return true;
}

std::vector<RowId> BTree::lookupEqual(const uint8_t* key) const {
    if (root_ == 0) return {};

    PageId leafId = findLeafPage(root_, key);
    Page leaf = store_->load(leafId);
    std::vector<RowId> result;
    uint16_t n = BTreePage::numKeys(leaf);
    for (uint16_t i = 0; i < n; i++) {
        uint8_t existing[IndexKey::MAX_KEY_SIZE];
        BTreePage::readLeafKey(leaf, i, keySize_, existing);
        if (IndexKey::equal(existing, key, keySize_))
            result.push_back(BTreePage::leafRidAt(leaf, i, keySize_));
    }
    return result;
}

bool BTree::removeFromLeaf(PageId leafId, const uint8_t* key, RowId rid) {
    Page leaf = store_->load(leafId);
    auto entries = BTreePage::readAllLeafEntries(leaf, keySize_);
    bool removed = false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (IndexKey::equal(it->first.data(), key, keySize_) &&
            it->second.pageId == rid.pageId && it->second.slotIndex == rid.slotIndex) {
            entries.erase(it);
            removed = true;
            break;
        }
    }
    if (!removed) return false;

    BTreePage::writeAllLeafEntries(leaf, keySize_, entries);
    leaf.writeU16(BTreePage::OFF_NUM_KEYS, static_cast<uint16_t>(entries.size()));
    store_->save(leafId, leaf);
    return true;
}

bool BTree::removeEqual(const uint8_t* key, RowId rid) {
    if (root_ == 0) return false;
    PageId leafId = findLeafPage(root_, key);
    return removeFromLeaf(leafId, key, rid);
}

std::vector<RowId> BTree::rangeScan(const BTreeBound& lo, const BTreeBound& hi) const {
    if (root_ == 0) return {};

    PageId leafId = lo.unbounded ? leftmostLeaf() : findLeafPage(root_, lo.key.data());
    std::vector<RowId> result;

    while (leafId != 0) {
        Page leaf = store_->load(leafId);
        uint16_t n = BTreePage::numKeys(leaf);
        for (uint16_t i = 0; i < n; i++) {
            uint8_t k[IndexKey::MAX_KEY_SIZE];
            BTreePage::readLeafKey(leaf, i, keySize_, k);
            if (!hi.unbounded) {
                int cmp = IndexKey::compare(k, hi.key.data(), keySize_);
                if (cmp > 0 || (cmp == 0 && !hi.inclusive))
                    return result;
            }
            if (keyInRange(k, lo, hi))
                result.push_back(BTreePage::leafRidAt(leaf, i, keySize_));
        }
        leafId = BTreePage::nextLeaf(leaf);
    }
    return result;
}
