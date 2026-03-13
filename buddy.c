#include "buddy.h"
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096

// Free list structure
typedef struct free_block {
    struct free_block *next;
} free_block_t;

// Global state
static void *base_address;           // Start of memory pool
static int total_pages;              // Total number of 4K pages
static free_block_t *free_lists[MAX_RANK + 1];  // Free lists for each rank
static char *allocated_bitmap;       // Track allocated pages (1 bit per page)
static char *rank_map;               // Track rank of each page

// Helper functions
static inline int is_valid_rank(int rank) {
    return rank >= 1 && rank <= MAX_RANK;
}

static inline int pages_for_rank(int rank) {
    return 1 << (rank - 1);  // 2^(rank-1) pages
}

static inline long page_index(void *p) {
    return ((char *)p - (char *)base_address) / PAGE_SIZE;
}

static inline void *index_to_ptr(long idx) {
    return (char *)base_address + idx * PAGE_SIZE;
}

static inline int is_allocated(long idx) {
    return (allocated_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static inline void set_allocated(long idx, int val) {
    if (val) {
        allocated_bitmap[idx / 8] |= (1 << (idx % 8));
    } else {
        allocated_bitmap[idx / 8] &= ~(1 << (idx % 8));
    }
}

// Get buddy index
static inline long get_buddy_index(long idx, int rank) {
    long block_size = pages_for_rank(rank);
    return idx ^ block_size;
}

// Check if two blocks can be merged (are buddies)
static inline int are_buddies(long idx1, long idx2, int rank) {
    return get_buddy_index(idx1, rank) == idx2;
}

// Initialize the buddy system
int init_page(void *p, int pgcount) {
    base_address = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Allocate bitmap and rank map (we'll use the managed memory for this)
    // Actually, we should use static arrays or malloc separately
    // For simplicity, let's use static arrays with maximum possible size
    static char alloc_bmp[1 << 16];  // Max 2^16 pages
    static char rnk_map[1 << 16];
    allocated_bitmap = alloc_bmp;
    rank_map = rnk_map;

    // Clear bitmaps
    for (int i = 0; i < (total_pages + 7) / 8; i++) {
        allocated_bitmap[i] = 0;
    }
    for (int i = 0; i < total_pages; i++) {
        rank_map[i] = 0;
    }

    // Add blocks to free lists
    // We need to build the largest possible blocks from total_pages
    long idx = 0;
    while (idx < total_pages) {
        // Find largest rank that fits
        int rank = MAX_RANK;
        while (rank > 0 && pages_for_rank(rank) > (total_pages - idx)) {
            rank--;
        }

        if (rank == 0) break;

        // Add block to free list
        free_block_t *block = (free_block_t *)index_to_ptr(idx);
        block->next = free_lists[rank];
        free_lists[rank] = block;

        // Update rank map for all pages in this block
        int npages = pages_for_rank(rank);
        for (int i = 0; i < npages; i++) {
            rank_map[idx + i] = rank;
        }

        idx += npages;
    }

    return OK;
}

// Allocate pages
void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of at least this rank
    int current_rank = rank;
    while (current_rank <= MAX_RANK && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Split blocks until we get the desired rank
    while (current_rank > rank) {
        // Remove from current free list
        free_block_t *block = free_lists[current_rank];
        free_lists[current_rank] = block->next;

        // Split into two buddies
        current_rank--;
        int npages = pages_for_rank(current_rank);
        long idx = page_index(block);

        // Add both halves to lower rank
        free_block_t *left_buddy = (free_block_t *)index_to_ptr(idx);
        free_block_t *right_buddy = (free_block_t *)index_to_ptr(idx + npages);

        left_buddy->next = right_buddy;
        right_buddy->next = free_lists[current_rank];
        free_lists[current_rank] = left_buddy;

        // Update rank map
        for (int i = 0; i < npages; i++) {
            rank_map[idx + i] = current_rank;
            rank_map[idx + npages + i] = current_rank;
        }
    }

    // Remove from free list
    free_block_t *block = free_lists[rank];
    free_lists[rank] = block->next;

    // Mark as allocated
    long idx = page_index(block);
    int npages = pages_for_rank(rank);
    for (int i = 0; i < npages; i++) {
        set_allocated(idx + i, 1);
        rank_map[idx + i] = rank;
    }

    return block;
}

// Return pages
int return_pages(void *p) {
    if (p == NULL) {
        return -EINVAL;
    }

    long idx = page_index(p);

    // Check if valid address
    if (idx < 0 || idx >= total_pages) {
        return -EINVAL;
    }

    // Check if allocated
    if (!is_allocated(idx)) {
        return -EINVAL;
    }

    int rank = rank_map[idx];
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    // Check if p is the start of an allocated block
    int npages = pages_for_rank(rank);
    if (idx % npages != 0) {
        return -EINVAL;
    }

    // Mark as free
    for (int i = 0; i < npages; i++) {
        set_allocated(idx + i, 0);
    }

    // Try to merge with buddies
    while (rank < MAX_RANK) {
        long buddy_idx = get_buddy_index(idx, rank);

        // Check if buddy exists, is free, and has same rank
        if (buddy_idx >= total_pages || buddy_idx < 0) break;
        if (is_allocated(buddy_idx)) break;
        if (rank_map[buddy_idx] != rank) break;

        // Check if buddy is the start of a block of this rank
        int buddy_npages = pages_for_rank(rank);
        if (buddy_idx % buddy_npages != 0) break;

        // Remove buddy from free list
        free_block_t **prev = &free_lists[rank];
        free_block_t *curr = free_lists[rank];
        int found = 0;
        while (curr != NULL) {
            if (page_index(curr) == buddy_idx) {
                *prev = curr->next;
                found = 1;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        if (!found) break;

        // Merge: the lower index becomes the merged block
        if (buddy_idx < idx) {
            idx = buddy_idx;
        }
        rank++;

        // Update rank map
        npages = pages_for_rank(rank);
        for (int i = 0; i < npages; i++) {
            rank_map[idx + i] = rank;
        }
    }

    // Add to free list
    free_block_t *block = (free_block_t *)index_to_ptr(idx);
    block->next = free_lists[rank];
    free_lists[rank] = block;

    return OK;
}

// Query rank of a page
int query_ranks(void *p) {
    long idx = page_index(p);

    // Check if valid address
    if (idx < 0 || idx >= total_pages) {
        return -EINVAL;
    }

    return rank_map[idx];
}

// Query page counts for a rank
int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    int count = 0;
    free_block_t *curr = free_lists[rank];
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
