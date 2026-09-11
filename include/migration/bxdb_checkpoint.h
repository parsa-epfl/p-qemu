/*
 * BSD 3-Clause License
 *
 * Copyright (c) 2026, Parallel Systems Architecture Laboratory (PARSA), EPFL.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the PARSA, EPFL
 *    nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * bxdb-backed incremental checkpoint wrapper
 *
 * Thin QEMU-facing interface over the bxdb C API (see bxdb/c-test/bxdb.h).
 * savevm.c talks to this header; it never includes bxdb.h directly.
 *
 * Each QEMU snapshot is paired with a small companion file
 * <name>.bxdb-meta containing:
 *     db_path=<path to the .bxdb directory>
 *     snap_id=<u32>
 * which tells the loader which bxdb DB and snapshot id to target.
 */

#ifndef QEMU_MIGRATION_BXDB_CHECKPOINT_H
#define QEMU_MIGRATION_BXDB_CHECKPOINT_H

#include "qapi/error.h"

struct DirtyBitmapSnapshot;

#ifdef CONFIG_BXDB

/*
 * Save paths. `memory` points at the first byte of the guest RAM region to
 * snapshot; `memory_size` is its size in bytes and must be a multiple of 4096.
 *
 * save_base writes bxdb snap_id 0 (no shadow, 16 workers) and closes the DB.
 * save_delta requires that a prior load_bulk has opened the DB with shadow
 * enabled, and appends a new snapshot to it using `dirty->dirty` as the
 * per-page bitmap (same memory/size must be passed).
 */
int bxdb_ckpt_save_base(const char *name,
                        const void *memory, uint64_t memory_size,
                        Error **errp);
int bxdb_ckpt_save_delta(const char *name,
                         struct DirtyBitmapSnapshot *dirty,
                         const void *memory, uint64_t memory_size,
                         Error **errp);

/* True iff <name>.bxdb-meta exists. Used by savevm.c for format detection. */
bool bxdb_ckpt_snapshot_exists(const char *name);

/*
 * Bulk load (on_demand == 0). Reads the meta file to discover the target DB
 * and snap_id, opens a fw handle with shadow ON, and materialises the entire
 * guest RAM via bxdb_load_all_pages. Keeps the handle open so that subsequent
 * save_delta calls can append to the same DB.
 */
int bxdb_ckpt_load_bulk(const char *name,
                        void *memory, uint64_t memory_size,
                        Error **errp);

/*
 * On-demand load (on_demand != 0). Opens a timing DB; the uffd handler then
 * calls bxdb_ckpt_fetch_page for each page fault. Must be paired with
 * bxdb_ckpt_ondemand_close once the VM is shut down.
 *
 * memory_size is the size of the guest RAM region the timing DB will serve.
 * It is required so that the implementation can size its test-mode reference
 * buffer; in non-test builds it is otherwise unused.
 */
int  bxdb_ckpt_ondemand_open(const char *name, uint64_t memory_size,
                             Error **errp);
bool bxdb_ckpt_fetch_page(uint64_t offset, void *buffer);

/*
 * Test-mode verification hook. Called by the uffd handler after each page is
 * materialised; in test mode it compares the page against a zstd-decompressed
 * reference and aborts on mismatch. No-op when test mode is disabled.
 */
void bxdb_ckpt_verify_page(uint64_t offset, const void *buffer);

void bxdb_ckpt_ondemand_close(void);

void bxdb_ckpt_shutdown(void);

const char *bxdb_ckpt_db_path(void);
uint32_t bxdb_ckpt_snap_id(void);

#else  /* !CONFIG_BXDB */

static inline int bxdb_ckpt_save_base(const char *name,
                                      const void *memory, uint64_t memory_size,
                                      Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb; "
                     "incremental base snapshots are unavailable");
    return -ENOTSUP;
}

static inline int bxdb_ckpt_save_delta(const char *name,
                                       struct DirtyBitmapSnapshot *dirty,
                                       const void *memory, uint64_t memory_size,
                                       Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb; "
                     "incremental delta snapshots are unavailable");
    return -ENOTSUP;
}

static inline bool bxdb_ckpt_snapshot_exists(const char *name) { return false; }

static inline int bxdb_ckpt_load_bulk(const char *name,
                                      void *memory, uint64_t memory_size,
                                      Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb");
    return -ENOTSUP;
}

static inline int bxdb_ckpt_ondemand_open(const char *name,
                                          uint64_t memory_size, Error **errp)
{
    error_setg(errp, "QEMU was built without --with-bxdb");
    return -ENOTSUP;
}

static inline bool bxdb_ckpt_fetch_page(uint64_t offset, void *buffer) { return false; }
static inline void bxdb_ckpt_verify_page(uint64_t offset, const void *buffer) { }
static inline void bxdb_ckpt_ondemand_close(void) { }
static inline void bxdb_ckpt_shutdown(void) { }
static inline const char *bxdb_ckpt_db_path(void) { return NULL; }
static inline uint32_t  bxdb_ckpt_snap_id(void)  { return 0; }

#endif /* CONFIG_BXDB */

#endif /* QEMU_MIGRATION_BXDB_CHECKPOINT_H */
