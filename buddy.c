#include "buddy.h"

#include <stdint.h>
#include <stdlib.h>

#define PAGE_SIZE 4096U
#define MAX_RANK 16
#define NO_PAGE (-1)

enum block_state {
    BLOCK_INTERIOR = 0,
    BLOCK_FREE,
    BLOCK_ALLOCATED
};

static uintptr_t pool_base;
static int pool_page_count;
static unsigned char *block_states;
static unsigned char *block_ranks;
static int *next_free;
static int *prev_free;
static int free_heads[MAX_RANK + 1];
static int free_tails[MAX_RANK + 1];
static int free_counts[MAX_RANK + 1];

static unsigned int pages_in_rank(int rank)
{
    return 1U << (rank - 1);
}

static int valid_rank(int rank)
{
    return rank >= 1 && rank <= MAX_RANK;
}

static void reset_lists(void)
{
    int rank;

    for (rank = 0; rank <= MAX_RANK; ++rank) {
        free_heads[rank] = NO_PAGE;
        free_tails[rank] = NO_PAGE;
        free_counts[rank] = 0;
    }
}

/* Add at the head: allocation and coalescing are both constant-time. */
static void add_free_block(int page, int rank)
{
    int old_head = free_heads[rank];

    block_states[page] = BLOCK_FREE;
    block_ranks[page] = (unsigned char)rank;
    prev_free[page] = NO_PAGE;
    next_free[page] = old_head;
    if (old_head != NO_PAGE)
        prev_free[old_head] = page;
    else
        free_tails[rank] = page;
    free_heads[rank] = page;
    ++free_counts[rank];
}

/* Initial blocks are appended so the first allocation uses the lowest one. */
static void append_free_block(int page, int rank)
{
    int old_tail = free_tails[rank];

    block_states[page] = BLOCK_FREE;
    block_ranks[page] = (unsigned char)rank;
    next_free[page] = NO_PAGE;
    prev_free[page] = old_tail;
    if (old_tail != NO_PAGE)
        next_free[old_tail] = page;
    else
        free_heads[rank] = page;
    free_tails[rank] = page;
    ++free_counts[rank];
}

static void remove_free_block(int page, int rank)
{
    int previous = prev_free[page];
    int next = next_free[page];

    if (previous != NO_PAGE)
        next_free[previous] = next;
    else
        free_heads[rank] = next;
    if (next != NO_PAGE)
        prev_free[next] = previous;
    else
        free_tails[rank] = previous;
    block_states[page] = BLOCK_INTERIOR;
    --free_counts[rank];
}

static int page_index(const void *pointer)
{
    uintptr_t address;
    uintptr_t offset;

    if (pointer == NULL || block_states == NULL)
        return NO_PAGE;

    address = (uintptr_t)pointer;
    if (address < pool_base)
        return NO_PAGE;
    offset = address - pool_base;
    if (offset % PAGE_SIZE != 0 || offset / PAGE_SIZE >= (uintptr_t)pool_page_count)
        return NO_PAGE;
    return (int)(offset / PAGE_SIZE);
}

int init_page(void *p, int pgcount)
{
    unsigned char *new_states;
    unsigned char *new_ranks;
    int *new_next;
    int *new_previous;
    int page;

    if (p == NULL || pgcount <= 0)
        return -EINVAL;

    new_states = (unsigned char *)calloc((size_t)pgcount, sizeof(*new_states));
    new_ranks = (unsigned char *)malloc((size_t)pgcount * sizeof(*new_ranks));
    new_next = (int *)malloc((size_t)pgcount * sizeof(*new_next));
    new_previous = (int *)malloc((size_t)pgcount * sizeof(*new_previous));
    if (new_states == NULL || new_ranks == NULL || new_next == NULL ||
        new_previous == NULL) {
        free(new_states);
        free(new_ranks);
        free(new_next);
        free(new_previous);
        return -ENOSPC;
    }

    free(block_states);
    free(block_ranks);
    free(next_free);
    free(prev_free);
    block_states = new_states;
    block_ranks = new_ranks;
    next_free = new_next;
    prev_free = new_previous;
    pool_base = (uintptr_t)p;
    pool_page_count = pgcount;
    reset_lists();

    page = 0;
    while (page < pgcount) {
        int rank = MAX_RANK;

        while (rank > 1) {
            unsigned int size = pages_in_rank(rank);

            if (page % (int)size == 0 && size <= (unsigned int)(pgcount - page))
                break;
            --rank;
        }
        append_free_block(page, rank);
        page += (int)pages_in_rank(rank);
    }

    return OK;
}

void *alloc_pages(int rank)
{
    int available_rank;
    int page;

    if (!valid_rank(rank))
        return ERR_PTR(-EINVAL);
    if (block_states == NULL)
        return ERR_PTR(-ENOSPC);

    for (available_rank = rank; available_rank <= MAX_RANK;
         ++available_rank) {
        if (free_heads[available_rank] != NO_PAGE)
            break;
    }
    if (available_rank > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    page = free_heads[available_rank];
    remove_free_block(page, available_rank);
    while (available_rank > rank) {
        int buddy;

        --available_rank;
        buddy = page + (int)pages_in_rank(available_rank);
        add_free_block(buddy, available_rank);
    }

    block_states[page] = BLOCK_ALLOCATED;
    block_ranks[page] = (unsigned char)rank;
    return (void *)(pool_base + (uintptr_t)page * PAGE_SIZE);
}

int return_pages(void *p)
{
    int page = page_index(p);
    int rank;

    if (page == NO_PAGE || block_states[page] != BLOCK_ALLOCATED)
        return -EINVAL;

    rank = block_ranks[page];
    block_states[page] = BLOCK_INTERIOR;
    while (rank < MAX_RANK) {
        int size = (int)pages_in_rank(rank);
        int buddy = page ^ size;

        if (buddy < 0 || buddy + size > pool_page_count ||
            block_states[buddy] != BLOCK_FREE || block_ranks[buddy] != rank)
            break;
        remove_free_block(buddy, rank);
        if (buddy < page)
            page = buddy;
        ++rank;
    }
    add_free_block(page, rank);
    return OK;
}

int query_ranks(void *p)
{
    int page = page_index(p);
    int rank;

    if (page == NO_PAGE)
        return -EINVAL;

    for (rank = MAX_RANK; rank >= 1; --rank) {
        int size = (int)pages_in_rank(rank);
        int start = page & ~(size - 1);

        if (start < pool_page_count && block_states[start] != BLOCK_INTERIOR &&
            block_ranks[start] == rank && page < start + size)
            return rank;
    }
    return -EINVAL;
}

int query_page_counts(int rank)
{
    if (!valid_rank(rank))
        return -EINVAL;
    return free_counts[rank];
}
