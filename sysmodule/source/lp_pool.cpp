/**
 * lp_pool.cpp — static memory pool implementation.
 *
 * 30 slots × 2100 bytes = 63 KB.  Statically allocated so the memory
 * is reserved at compile-time and doesn't depend on heap availability.
 */
#include "lp_pool.h"

/* ------------------------------------------------------------------ */
/*  Pool storage                                                        */
/* ------------------------------------------------------------------ */

static uint8_t s_pool_data[LP_POOL_SLOT_COUNT][LP_POOL_SLOT_SIZE]
    __attribute__((aligned(16)));

static bool s_slot_in_use[LP_POOL_SLOT_COUNT];
static Mutex s_pool_mutex;

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

void lp_pool_init(void)
{
    mutexInit(&s_pool_mutex);
    memset(s_slot_in_use, 0, sizeof(s_slot_in_use));
    LLOG(LLOG_INFO, "lp_pool: initialised (%d slots × %d bytes = %d KB)",
         LP_POOL_SLOT_COUNT, LP_POOL_SLOT_SIZE,
         (LP_POOL_SLOT_COUNT * LP_POOL_SLOT_SIZE) / 1024);
}

void *lp_pool_alloc(size_t size)
{
    /* Fast path: if the request fits in a slot, try the pool first. */
    if (size <= LP_POOL_SLOT_SIZE) {
        mutexLock(&s_pool_mutex);
        for (int i = 0; i < LP_POOL_SLOT_COUNT; i++) {
            if (!s_slot_in_use[i]) {
                s_slot_in_use[i] = true;
                mutexUnlock(&s_pool_mutex);
                return s_pool_data[i];
            }
        }
        mutexUnlock(&s_pool_mutex);
        /* Pool exhausted — fall through to malloc */
        static int pool_miss = 0;
        if (++pool_miss <= 5) {
            LLOG(LLOG_WARNING, "lp_pool: all %d slots busy, falling back to malloc (%zu bytes)",
                 LP_POOL_SLOT_COUNT, size);
        }
    }

    /* Oversize or pool full — use heap. */
    return malloc(size);
}

void lp_pool_free(void *ptr)
{
    if (!ptr) return;

    /* Check if the pointer belongs to our pool. */
    uint8_t *p = (uint8_t *)ptr;
    uint8_t *pool_start = &s_pool_data[0][0];
    uint8_t *pool_end   = &s_pool_data[LP_POOL_SLOT_COUNT - 1][0] + LP_POOL_SLOT_SIZE;

    if (p >= pool_start && p < pool_end) {
        /* Calculate slot index from pointer arithmetic. */
        int idx = (int)((p - pool_start) / LP_POOL_SLOT_SIZE);
        if (idx >= 0 && idx < LP_POOL_SLOT_COUNT &&
            p == &s_pool_data[idx][0]) {
            mutexLock(&s_pool_mutex);
            s_slot_in_use[idx] = false;
            mutexUnlock(&s_pool_mutex);
            return;
        }
    }

    /* Not a pool pointer — free from heap. */
    free(ptr);
}
