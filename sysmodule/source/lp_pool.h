/**
 * lp_pool.h — static memory pool for the sysmodule hot path.
 *
 * Replaces per-packet malloc/free in lan_client_send() with a fixed
 * array of reusable slots.  This eliminates heap fragmentation on the
 * Switch's constrained 2 MB sysmodule heap and guarantees allocation
 * never fails under normal traffic load.
 *
 * Thread-safe: guarded by a Mutex (3 threads call lan_client_send).
 * Falls back to malloc() if every slot is busy or the requested size
 * exceeds LP_POOL_SLOT_SIZE.
 */
#pragma once

#include "nx_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Slot sizing rationale:
 *   max payload = 1 (type byte) + LC_FRAG_HEADER_LEN (16) + ETHER_MTU (1500)
 *               = 1517 bytes.
 *   Non-fragmented max = 1 (type byte) + 4096 (relay_buf size) = 4097.
 *   We cap at 2100 which covers all fragmented packets comfortably.
 *   Non-fragmented giant packets (>2100) fall back to malloc — rare.
 */
#define LP_POOL_SLOT_SIZE   2100
#define LP_POOL_SLOT_COUNT  30

/** Initialise the pool.  Call once before any threads start. */
void  lp_pool_init(void);

/**
 * Allocate a buffer of at least `size` bytes.
 * Returns a pool slot if size <= LP_POOL_SLOT_SIZE and a slot is free.
 * Falls back to malloc() otherwise.
 * Returns NULL only if both the pool and malloc fail (out of memory).
 */
void *lp_pool_alloc(size_t size);

/**
 * Free a buffer previously returned by lp_pool_alloc().
 * If `ptr` belongs to the pool, the slot is released.
 * Otherwise, free() is called.
 */
void  lp_pool_free(void *ptr);

#ifdef __cplusplus
}
#endif
