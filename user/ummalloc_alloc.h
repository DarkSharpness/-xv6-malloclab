#pragma once
#include "ummalloc_data.h"

/**
 * @brief Allocate memory from a pack.
 * @param pack Pointer to the pack.
 * @param size Size of the pack.
 * @return Data pointer. Never return NULL.
 */
static inline void *
pack_allocate(struct pack *pack, size_t size) {
    struct pack *next = pack_next(pack);
    pack_set_prev(next, size);
    pack_set_info(next, 0, PREV_INUSE);
    return pack->data;
}

/**
 * @brief Split the pack and allocate memory.
 * @param pack Pointer to the pack.
 * @param size Size of the pack.
 * @param need Required size.
 * @return Data pointer. nullptr if failed.
*/
static inline void *
split_allocate(struct pack *pack, size_t size, size_t need) {
    /// TODO: Implement the split allocation.

    return pack_allocate(pack, size);
}

/** Just a wrapper function. */
static inline void *
try_split_allocate(struct pack *pack, size_t size, size_t need) {
    if (size > need * 2)
        return split_allocate(pack, size, need);
    else
        return pack_allocate(pack, size);
}

static inline uint64_t
next_free(size_t size) {
    uint64_t mask = bitmap;
    uint64_t temp = -1;
    mask &= temp << (size + 1);
    return mask & (-mask);
}

static inline void
bitmap_set(size_t index) { bitmap |= 1 << index; }

static inline void
bitmap_clr(size_t index) { bitmap &= ~(1 << index); }

/* Safely remove the first node from the list. */
static inline struct node *
list_extract(size_t index) {
    struct node *list = &slots[index];
    struct node *node = list_pop(list);
    if (list_empty(list)) bitmap_clr(index);
    return node;
}

/**
 * @brief Allocate memory from a larger-indexed slot.
 * @param index Index of current size's slot.
 * @param size Allocated size.
 * @return Data pointer. Never return NULL.
 */
static inline void *
next_allocate(size_t index, size_t size) {
    uint64_t lowbit = next_free(index);
    if (lowbit == 0) return malloc_brk(size);

    size_t position = log2_ceil64(lowbit);

    struct node *node = list_extract(position);
    struct pack *pack = list_pack(node);

    return try_split_allocate(pack, pack_size(pack), size);
}

/**
 * @brief Allocate memory from the fast slot for
 * extremely small memory, with fixed size = 32 bytes.
 * @return Data pointer. nullptr if failed.
 */
static inline void *
fast_allocate(void) {
    /// TODO: Implement the fast allocation.
    return 0;
}

/**
 * @brief Allocate memory from a tiny slot.
 * Each node is fixed, aligned to 16 bytes, and the
 * size ranges in [48, 512] bytes, step by 16.
 * @param index Index of the slot. Range: [2, 32)
 * @param need  Required size. need = (index + 1) * 16
 * @return Data pointer. nullptr if failed.
 */
static inline void *
tiny_allocate(size_t index, size_t need) {
    if (list_empty(slots + index)) return (void *)0;
    struct node *node = list_extract(index);
    struct pack *pack = list_pack(node);
    return pack_allocate(pack, need);
}

/**
 * @brief Allocate memory from a middle slot.
 * Each node is dynamic-sized. The size ranges in
 * [512, 768] and (768, 4096), step by 64 and 256. 
 * @param index Index of the slot. Range: [32, 48)
 * @param need  Required size.
 * @param iteration Iteration of attempts.
 * @return Data pointer. nullptr if failed.
 * @note Since the slot might be dynamic-sized, node size
 * may be smaller than the required size. So, we may need
 * to traverse the list to find a good enough fit.
 * 
 * Sadly, traversing the list might be expensive, so we
 * fix the iteration count to a small value, so that we
 * can quickly return if we can't find a good fit.
 */
static inline void *
middle_allocate(size_t index, size_t need, size_t iteration) {
    struct node *list = slots + index;
    struct node *head = list->next;
    while (iteration-- != 0 && head != list) {
        struct pack *pack = list_pack(head);
        size_t size = pack_size(pack);

        if (size >= need) {
            struct node *prev = head->prev;
            struct node *next = head->next;
            node_link(prev, next);
            if (prev == next) bitmap_clr(index);
            return pack_allocate(pack, size);
        }

        head = head->next;
    }

    return (void *)0;
}

/**
 * @brief Allocate memory from corresponding slot.
 * Each node is dynamic-sized. The size ranges in
 * [4096, 8192] and (8192, 65536], step by 2048 and 4096.
 * @param index Index of the slot.
 * @param size Required size.
 * @param iteration Iteration of attempes
 * @return Data pointer. nullptr if failed.
 * @note Since the slot might be dynamic-sized, node size
 * may be smaller than the required size. So, we may need
 * to traverse the list to find a good enough fit.
 * 
 * Sadly, traversing the list might be expensive, so we
 * fix the iteration count to a small value, so that we
 * can quickly return if we can't find a good fit.
 */
static inline void *
huge_allocate(size_t index, size_t size, size_t iteration) {
    return malloc_brk(size);
}

/**
 * @brief There is no more fast bin to allocate.
 * So, we need a new page to be the new fast bin.
 * @return 
 */
static inline void *fast_bin_allocate(void) {
    return 0;
}



/* Initialize all the lists. */
static inline void mm_list_init(void) {
    for (size_t i = 0; i < 64; i++) list_init(&slots[i]);
}

/* Align the top to 4096 and align the base to 8. */
static inline void mm_align_init(void) {
    size_t heap = (size_t)sbrk(PAGE_SIZE);
    size_t temp = heap % PAGE_SIZE;
    size_t size = PAGE_SIZE;

    if (temp != 0) {
        size += PAGE_SIZE - temp;
        sbrk(PAGE_SIZE - temp);
    }

    size_t top = heap + size;
    size_t low = ALIGN(heap) + sizeof(struct pack);

    base = (struct node *)low;
    size = top - low;

    /* Regard previous memory of first part. as unreachable. */
    struct node *head = (struct node *)low;
    pack_set_info(list_pack(head), size, PREV_INUSE);

    /* Regard the last part of memory as inuse. */
    struct node *tail = (struct node *)top;
    pack_set_info(list_pack(tail), 0, THIS_INUSE);
}

/* Allocate memory from the brk */
static inline void *malloc_brk(size_t need) {
    struct pack *pack = list_pack(base);

    size_t page = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t size = page * PAGE_SIZE;

    size_t heap = (size_t)(base);
    base = (struct node *)(heap + size);

    if (sbrk(size) != (char *)heap) return 0; // Out of memory.

    /* split and allocate */
    return try_split_allocate(pack, size, need);
}

/** Input wrapper of different size. */


static inline void *malloc_fast(void) {
    void *data = fast_allocate();
    if (data != (void *)0) return data;
    return fast_bin_allocate();
}

static inline void *
malloc_tiny(size_t size) {
    if (size <= 32) return malloc_fast();
    size_t index = (size - 1) / 16;
    size = (index + 1) * 16; // Align to 16 bytes.

    void *data = tiny_allocate(index, size);
    if (data != (void *)0) return data;

    return next_allocate(index, size);
}

static inline void *
malloc_middle(size_t size) {
    size_t index = size <= 640 ? (size + 1535) / 64 : 34 + (size - 513) / 256;

    void *data = middle_allocate(index, size, 4);
    if (data != (void *)0) return data;

    return next_allocate(index, size);
}

void *malloc_huge(size_t size) {
    size_t index = size < 6144 ? 48 : (size - 1) / 4096 + 48;

    void *data = huge_allocate(index, size, 8);
    if (data != (void *)0) return data;

    return next_allocate(index, size);
}
