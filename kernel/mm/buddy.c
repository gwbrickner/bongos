/* Pure buddy-allocator core (D-081, ROADMAP M2.2): block state lives entirely in the `Page` array
 * (page.h) -- there is no separate bitmap. No klog, no panic, no arch calls -- host-tested
 * directly by tests/host/kernel_buddy_test.c against a reference bitmap model. */
#include "pmm-internal.h"

void buddyZoneInit(PmmZone *zone, const char *name, uint64_t startPfn, uint64_t endPfn) {
    for (uint32_t k = 0; k < PMM_ORDER_COUNT; k++) {
        listInit(&zone->freeList[k]);
        zone->freeBlocks[k] = 0;
    }
    zone->freePages = 0;
    zone->managedPages = 0;
    zone->startPfn = startPfn;
    zone->endPfn = endPfn;
    zone->name = name;
}

void buddyFreeBlock(PmmZone *zone, uint64_t pfn, uint32_t order) {
    zone->managedPages += (uint64_t)1 << order;
    zone->freePages += (uint64_t)1 << order;

    while (order < PMM_MAX_ORDER) {
        uint64_t buddyPfn = pfn ^ ((uint64_t)1 << order);
        Page *buddyPage = pageFromPfn(buddyPfn);
        if (buddyPage->state != PAGE_STATE_BUDDY || buddyPage->order != order) {
            break;
        }
        listRemove(&buddyPage->lru);
        zone->freeBlocks[order]--;
        buddyPage->state = PAGE_STATE_TAIL;
        buddyPage->order = 0;
        pfn &= ~((uint64_t)1 << order);
        order++;
    }

    Page *head = pageFromPfn(pfn);
    head->state = PAGE_STATE_BUDDY;
    head->order = (uint8_t)order;
    listPushHead(&zone->freeList[order], &head->lru);
    zone->freeBlocks[order]++;
}

bool buddyAllocBlock(PmmZone *zone, uint32_t order, uint64_t *outPfn) {
    uint32_t k = order;
    while (k < PMM_ORDER_COUNT && listEmpty(&zone->freeList[k])) {
        k++;
    }
    if (k >= PMM_ORDER_COUNT) {
        return false;
    }

    ListNode *node = listPopHead(&zone->freeList[k]);
    Page *head = LIST_CONTAINER(node, Page, lru);
    zone->freeBlocks[k]--;
    uint64_t pfn = pageToPfn(head);
    head->state = PAGE_STATE_TAIL;
    head->order = 0;

    while (k > order) {
        k--;
        uint64_t upperPfn = pfn + ((uint64_t)1 << k);
        Page *upper = pageFromPfn(upperPfn);
        upper->state = PAGE_STATE_BUDDY;
        upper->order = (uint8_t)k;
        listPushHead(&zone->freeList[k], &upper->lru);
        zone->freeBlocks[k]++;
    }

    zone->freePages -= (uint64_t)1 << order;
    *outPfn = pfn;
    return true;
}
