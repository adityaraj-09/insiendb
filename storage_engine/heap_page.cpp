#include "heap_page.h"
#include "row_codec.h"
#include <stdexcept>

void HeapPage::init(Page& page) {
    page.zero();
    page.writeU32(OFF_MAGIC, HEAP_PAGE_MAGIC);
    page.writeU16(OFF_NUM_SLOTS, 0);
    page.writeU16(OFF_DATA_END, static_cast<uint16_t>(HEADER_SIZE));
    page.writeU32(OFF_NEXT_PAGE, 0);
}

bool HeapPage::isHeapPage(const Page& page) {
    return page.readU32(OFF_MAGIC) == HEAP_PAGE_MAGIC;
}

PageId HeapPage::nextPage(const Page& page) {
    return page.readU32(OFF_NEXT_PAGE);
}

void HeapPage::setNextPage(Page& page, PageId next) {
    page.writeU32(OFF_NEXT_PAGE, next);
}

size_t HeapPage::slotOffset(uint16_t slotIndex) {
    if (slotIndex >= (PAGE_SIZE - HEADER_SIZE) / SLOT_SIZE)
        throw std::out_of_range("HeapPage: slot index out of range");
    return PAGE_SIZE - (static_cast<size_t>(slotIndex) + 1) * SLOT_SIZE;
}

void HeapPage::readSlot(const Page& page, uint16_t slotIndex, uint16_t& offset, uint16_t& length) {
    size_t off = slotOffset(slotIndex);
    offset = page.readU16(off);
    length = page.readU16(off + 2);
}

void HeapPage::writeSlot(Page& page, uint16_t slotIndex, uint16_t offset, uint16_t length) {
    size_t off = slotOffset(slotIndex);
    page.writeU16(off, offset);
    page.writeU16(off + 2, length);
}

size_t HeapPage::slotDirectoryBytes(uint16_t numSlots) {
    return static_cast<size_t>(numSlots) * SLOT_SIZE;
}

bool HeapPage::hasRoom(const Page& page, size_t rowLen, uint16_t numSlots) {
    uint16_t dataEnd = page.readU16(OFF_DATA_END);
    size_t usedByData = dataEnd - HEADER_SIZE;
    size_t usedBySlots = slotDirectoryBytes(numSlots + 1);
    size_t totalNeeded = usedByData + rowLen + usedBySlots;
    return totalNeeded <= PAGE_SIZE;
}

size_t HeapPage::freeSpace(const Page& page) {
    uint16_t dataEnd = page.readU16(OFF_DATA_END);
    uint16_t numSlots = page.readU16(OFF_NUM_SLOTS);
    size_t dataUsed = dataEnd - HEADER_SIZE;
    size_t slotUsed = slotDirectoryBytes(numSlots);
    if (HEADER_SIZE + dataUsed + slotUsed >= PAGE_SIZE) return 0;
    return PAGE_SIZE - HEADER_SIZE - dataUsed - slotUsed;
}

std::optional<uint16_t> HeapPage::insert(Page& page, const uint8_t* rowBytes, size_t rowLen) {
    if (rowLen > 0xFFFF)
        return std::nullopt;

    uint16_t numSlots = page.readU16(OFF_NUM_SLOTS);
    if (!hasRoom(page, rowLen, numSlots))
        return std::nullopt;

    uint16_t dataEnd = page.readU16(OFF_DATA_END);
    page.writeBytes(dataEnd, rowBytes, rowLen);

    uint16_t slotIndex = numSlots;
    writeSlot(page, slotIndex, dataEnd, static_cast<uint16_t>(rowLen));

    page.writeU16(OFF_DATA_END, static_cast<uint16_t>(dataEnd + rowLen));
    page.writeU16(OFF_NUM_SLOTS, static_cast<uint16_t>(numSlots + 1));
    return slotIndex;
}

bool HeapPage::remove(Page& page, uint16_t slotIndex) {
    uint16_t numSlots = page.readU16(OFF_NUM_SLOTS);
    if (slotIndex >= numSlots) return false;

    uint16_t offset, length;
    readSlot(page, slotIndex, offset, length);
    if (offset == 0) return false;

    writeSlot(page, slotIndex, 0, length);
    return true;
}

bool HeapPage::updateBytes(Page& page, uint16_t slotIndex, const uint8_t* rowBytes, size_t rowLen) {
    if (rowLen > 0xFFFF) return false;

    uint16_t numSlots = page.readU16(OFF_NUM_SLOTS);
    if (slotIndex >= numSlots) return false;

    uint16_t offset, oldLen;
    readSlot(page, slotIndex, offset, oldLen);
    if (offset == 0) return false;

    if (rowLen <= oldLen) {
        page.writeBytes(offset, rowBytes, rowLen);
        writeSlot(page, slotIndex, offset, static_cast<uint16_t>(rowLen));
        return true;
    }

    uint16_t dataEnd = page.readU16(OFF_DATA_END);
    size_t slotDir = slotDirectoryBytes(numSlots);
    if (static_cast<size_t>(dataEnd) + rowLen + slotDir > PAGE_SIZE)
        return false;

    page.writeBytes(dataEnd, rowBytes, rowLen);
    writeSlot(page, slotIndex, dataEnd, static_cast<uint16_t>(rowLen));
    page.writeU16(OFF_DATA_END, static_cast<uint16_t>(dataEnd + rowLen));
    return true;
}

bool HeapPage::updateRow(Page& page, uint16_t slotIndex, const Row& row) {
    std::vector<uint8_t> encoded;
    RowCodec::encodeRow(row, encoded);
    return updateBytes(page, slotIndex, encoded.data(), encoded.size());
}

uint16_t HeapPage::slotCount(const Page& page) {
    return page.readU16(OFF_NUM_SLOTS);
}

uint16_t HeapPage::liveRowCount(const Page& page) {
    uint16_t n = slotCount(page);
    uint16_t live = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (isLiveSlot(page, i)) live++;
    }
    return live;
}

bool HeapPage::isLiveSlot(const Page& page, uint16_t slotIndex) {
    if (slotIndex >= slotCount(page)) return false;
    uint16_t offset, length;
    readSlot(page, slotIndex, offset, length);
    return offset != 0;
}

std::vector<uint8_t> HeapPage::getRowBytes(const Page& page, uint16_t slotIndex) {
    uint16_t numSlots = page.readU16(OFF_NUM_SLOTS);
    if (slotIndex >= numSlots)
        throw std::out_of_range("HeapPage: slot index out of range");

    uint16_t offset, length;
    readSlot(page, slotIndex, offset, length);
    if (offset == 0)
        throw std::runtime_error("HeapPage: slot is deleted");

    std::vector<uint8_t> bytes(length);
    page.readBytes(offset, bytes.data(), length);
    return bytes;
}

Row HeapPage::getRow(const Page& page, uint16_t slotIndex) {
    auto bytes = getRowBytes(page, slotIndex);
    return RowCodec::decodeRow(bytes.data(), bytes.size());
}

std::optional<uint16_t> HeapPage::insertRow(Page& page, const Row& row) {
    std::vector<uint8_t> encoded;
    RowCodec::encodeRow(row, encoded);
    return insert(page, encoded.data(), encoded.size());
}

std::vector<HeapPage::LiveSlot> HeapPage::liveSlots(const Page& page) {
    std::vector<LiveSlot> result;
    uint16_t n = slotCount(page);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t offset, length;
        readSlot(page, i, offset, length);
        if (offset == 0) continue;
        LiveSlot slot;
        slot.index = i;
        slot.bytes.resize(length);
        page.readBytes(offset, slot.bytes.data(), length);
        result.push_back(std::move(slot));
    }
    return result;
}
