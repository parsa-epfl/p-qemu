/**
 * @file include/sysemu/asid-coeff.h
 *
 * Per-ASID IPC model coefficient table.
 *
 * Provides a global hash table mapping ARM ASID values (uint16_t, stored as
 * GUINT_TO_POINTER keys) to per-ASID bx_* coefficients.  The table is
 * populated once at startup from "asid_info.csv" (optional) and is read-only
 * thereafter, making concurrent reads from multiple vCPU threads safe.
 */

#ifndef SYSEMU_ASID_COEFF_H
#define SYSEMU_ASID_COEFF_H

#include "qemu/osdep.h"
#include <glib.h>

/**
 * asid_coeff_t - per-ASID IPC model coefficients.
 *
 * All values are stored as fixed-point integers (value * 1000), matching
 * the encoding used for the per-core bx_* coefficients in CPUState.
 */
typedef struct asid_coeff_t {
    uint64_t bx_private_icache_miss_coeff;
    uint64_t bx_private_dcache_miss_load_ptw_coeff;
    uint64_t bx_private_dcache_miss_store_coeff;
    uint64_t bx_shared_cache_miss_coeff;
    uint64_t bx_bp_miss_coeff;
    uint64_t bx_drain_pipeline_coeff;
    uint64_t bx_drain_store_buffer_coeff;
    uint64_t bx_read_noc_hop_coeff;
    uint64_t bx_write_noc_hop_coeff;
    uint64_t bx_ifetch_noc_hop_coeff;
    uint64_t bx_instruction_u_coeff;
    uint64_t bx_instruction_k_coeff;
} asid_coeff_t;

/**
 * tcg_parse_asid_info_file - load per-ASID coefficients from a CSV file.
 * @file_name: path to the CSV file (e.g. "asid_info.csv").
 *
 * Expected CSV header:
 *   asid,bx_private_icache_miss_coeff,...,bx_instruction_k_coeff
 *
 * If the file does not exist the call is silently ignored and
 * tcg_get_asid_coeff_table() will return NULL.  Must be called
 * single-threaded before any vCPU thread is started.
 */
void tcg_parse_asid_info_file(const char *file_name);

/**
 * tcg_get_asid_coeff_table - return the global ASID coefficient table.
 *
 * Returns the GHashTable populated by tcg_parse_asid_info_file(), or NULL if
 * the file was not loaded.  Keys are GUINT_TO_POINTER(asid) (uint16_t cast to
 * guint); values are heap-allocated asid_coeff_t pointers owned by the table.
 *
 * The table is write-once (populated at init) so concurrent reads are safe.
 */
GHashTable *tcg_get_asid_coeff_table(void);

/**
 * quantum_flush_current_stats - deduct accumulated stats into the quantum budget.
 * @cpu: the CPUState whose statistics should be flushed.
 *
 * Computes required_picoseconds from the current plugin-exposed statistics
 * using the core's current bx_* coefficients, zeroes the statistics, updates
 * target_cycle_on_instruction and cpu_virtual_time[].vts, and subtracts from
 * quantum_budget_in_picosecond.  Does NOT set quantum_budget_depleted — the
 * caller is responsible for checking the budget afterward.
 *
 * Safe to call from translated-code context (TTBR write handlers) where
 * current_cpu is valid.  Uses the passed @cpu parameter throughout.
 */
struct CPUState;
void quantum_flush_current_stats(struct CPUState *cpu);

#endif /* SYSEMU_ASID_COEFF_H */
