#include "freelist.h"

void FreeListPage::init(Page& page, PageId nextFree) {
    page.zero();
    page.writeU32(OFF_MAGIC, FREE_PAGE_MAGIC);
    page.writeU32(OFF_NEXT, nextFree);
}

bool FreeListPage::isFreePage(const Page& page) {
    return page.readU32(OFF_MAGIC) == FREE_PAGE_MAGIC;
}

PageId FreeListPage::nextFree(const Page& page) {
    return page.readU32(OFF_NEXT);
}
