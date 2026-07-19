#pragma once
#include "page.h"
#include "common.h"

// SQLite-style freelist: a singly-linked list of reusable pages.
//
// Each free page is marked with FREE_PAGE_MAGIC and stores the next free
// page id at offset 4. Page 0's bootstrap holds the list head (0 = empty).
//
//   free page layout:
//     [magic: u32 "FEEP"][next_free: u32]
//
// Push: new page -> old head -> ... 
// Pop:  take head, head = head.next
class FreeListPage {
public:
    static constexpr size_t OFF_MAGIC = 0;
    static constexpr size_t OFF_NEXT  = 4;

    static void init(Page& page, PageId nextFree);
    static bool isFreePage(const Page& page);

    static PageId nextFree(const Page& page);
};
