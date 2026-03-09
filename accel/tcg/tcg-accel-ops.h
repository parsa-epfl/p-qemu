/*
 * QEMU TCG vCPU common functionality
 *
 * Functionality common to all TCG vcpu variants: mttcg, rr and icount.
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TCG_ACCEL_OPS_H
#define TCG_ACCEL_OPS_H

#include "sysemu/cpus.h"

/* Core metadata information from core_info.csv */
typedef struct core_meta_info_t {
    int64_t host_core_idx;  // Host CPU core for affinity
    double ipns;
    // Coefficients are stored as fixed-point (value * 1000 for 3 decimal places)
    uint64_t bx_instruction_coeff;
    uint64_t bx_instruction_access_coeff;
    uint64_t bx_data_access_coeff;
    uint64_t bx_private_icache_miss_coeff;
    uint64_t bx_private_dcache_miss_coeff;
    uint64_t bx_shared_cache_miss_coeff;
    uint64_t bx_branch_count_coeff;
    uint64_t bx_bp_miss_coeff;
    uint64_t bx_tlb_miss_coeff;
} core_meta_info_t;

/* Check if model is constant type (bx_instruction_coeff=1000, others=0) */
static inline bool core_model_is_constant(const core_meta_info_t *info) {
    return (info->bx_instruction_coeff == 1000 &&
            info->bx_instruction_access_coeff == 0 &&
            info->bx_data_access_coeff == 0 &&
            info->bx_private_icache_miss_coeff == 0 &&
            info->bx_private_dcache_miss_coeff == 0 &&
            info->bx_shared_cache_miss_coeff == 0 &&
            info->bx_branch_count_coeff == 0 &&
            info->bx_bp_miss_coeff == 0 &&
            info->bx_tlb_miss_coeff == 0);
}

void tcg_cpus_destroy(CPUState *cpu);
int tcg_cpus_exec(CPUState *cpu);
void tcg_handle_interrupt(CPUState *cpu, int mask);
void tcg_cpu_init_cflags(CPUState *cpu, bool parallel);

/* Parse core_info.csv file and fill the core_info_table */
void tcg_parse_core_info_file(const char *file_name, core_meta_info_t *core_info_table, int max_cores);

#endif /* TCG_ACCEL_OPS_H */
