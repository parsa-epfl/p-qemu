/*
 * QEMU TCG vCPU common functionality
 *
 * Functionality common to all TCG vCPU variants: mttcg, rr and icount.
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/quantum.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "exec/exec-all.h"
#include "exec/hwaddr.h"
#include "exec/gdbstub.h"
#include <math.h>

#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"
#include "tcg-accel-ops-rr.h"
#include "tcg-accel-ops-icount.h"
#include "tcg-accel-ops-quantum.h"

/* common functionality among all TCG variants */

void tcg_cpu_init_cflags(CPUState *cpu, bool parallel)
{
    uint32_t cflags;

    /*
     * Include the cluster number in the hash we use to look up TBs.
     * This is important because a TB that is valid for one cluster at
     * a given physical address and set of CPU flags is not necessarily
     * valid for another:
     * the two clusters may have different views of physical memory, or
     * may have different CPU features (eg FPU present or absent).
     */
    cflags = cpu->cluster_index << CF_CLUSTER_SHIFT;

    cflags |= parallel ? CF_PARALLEL : 0;
    cflags |= icount_enabled() ? CF_USE_ICOUNT : 0;
    cpu->tcg_cflags |= cflags;
}

void tcg_cpus_destroy(CPUState *cpu)
{
    cpu_thread_signal_destroyed(cpu);
}

int tcg_cpus_exec(CPUState *cpu)
{
    int ret;
    assert(tcg_enabled());
    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);
    return ret;
}

/* mask must never be zero, except for A20 change call */
void tcg_handle_interrupt(CPUState *cpu, int mask)
{
    g_assert(qemu_mutex_iothread_locked());

    cpu->interrupt_request |= mask;

    /*
     * If called from iothread context, wake the target cpu in
     * case its halted.
     */
    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    } else {
        qatomic_set(&cpu_neg(cpu)->icount_decr.u16.high, -1);
    }
}

static bool tcg_supports_guest_debug(void)
{
    return true;
}

/* Translate GDB watchpoint type to a flags value for cpu_watchpoint_* */
static inline int xlat_gdb_type(CPUState *cpu, int gdbtype)
{
    static const int xlat[] = {
        [GDB_WATCHPOINT_WRITE]  = BP_GDB | BP_MEM_WRITE,
        [GDB_WATCHPOINT_READ]   = BP_GDB | BP_MEM_READ,
        [GDB_WATCHPOINT_ACCESS] = BP_GDB | BP_MEM_ACCESS,
    };

    CPUClass *cc = CPU_GET_CLASS(cpu);
    int cputype = xlat[gdbtype];

    if (cc->gdb_stop_before_watchpoint) {
        cputype |= BP_STOP_BEFORE_ACCESS;
    }
    return cputype;
}

static int tcg_insert_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_insert(cpu, addr, BP_GDB, NULL);
            if (err) {
                break;
            }
        }
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_insert(cpu, addr, len,
                                        xlat_gdb_type(cpu, type), NULL);
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static int tcg_remove_breakpoint(CPUState *cs, int type, vaddr addr, vaddr len)
{
    CPUState *cpu;
    int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        CPU_FOREACH(cpu) {
            err = cpu_breakpoint_remove(cpu, addr, BP_GDB);
            if (err) {
                break;
            }
        }
        return err;
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_remove(cpu, addr, len,
                                        xlat_gdb_type(cpu, type));
            if (err) {
                break;
            }
        }
        return err;
    default:
        return -ENOSYS;
    }
}

static inline void tcg_remove_all_breakpoints(CPUState *cpu)
{
    cpu_breakpoint_remove_all(cpu, BP_GDB);
    cpu_watchpoint_remove_all(cpu, BP_GDB);
}

static void tcg_accel_ops_init(AccelOpsClass *ops)
{
    if (quantum_enabled()) {
        ops->create_vcpu_thread = quantum_start_vcpu_thread;
        ops->kick_vcpu_thread = quantum_kick_vcpu_thread;
        ops->handle_interrupt = tcg_handle_interrupt;
        quantum_initialize_barrier();
    }  else if (qemu_tcg_mttcg_enabled()) {
        ops->create_vcpu_thread = mttcg_start_vcpu_thread;
        ops->kick_vcpu_thread = mttcg_kick_vcpu_thread;
        ops->handle_interrupt = tcg_handle_interrupt;
    } else {
        ops->create_vcpu_thread = rr_start_vcpu_thread;
        ops->kick_vcpu_thread = rr_kick_vcpu_thread;

        if (icount_enabled()) {
            ops->handle_interrupt = icount_handle_interrupt;
            ops->get_virtual_clock = icount_get;
            ops->get_elapsed_ticks = icount_get;
        } else {
            ops->handle_interrupt = tcg_handle_interrupt;
        }
    }

    ops->supports_guest_debug = tcg_supports_guest_debug;
    ops->insert_breakpoint = tcg_insert_breakpoint;
    ops->remove_breakpoint = tcg_remove_breakpoint;
    ops->remove_all_breakpoints = tcg_remove_all_breakpoints;
}

static void tcg_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->ops_init = tcg_accel_ops_init;
}

static const TypeInfo tcg_accel_ops_type = {
    .name = ACCEL_OPS_NAME("tcg"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = tcg_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("tcg"));

static void tcg_accel_ops_register_types(void)
{
    type_register_static(&tcg_accel_ops_type);
}
type_init(tcg_accel_ops_register_types);

/* Parse core_info.csv file and fill the core_info_table */
void tcg_parse_core_info_file(const char *file_name, core_meta_info_t *core_info_table, int max_cores)
{
    // By default, all cores' IPC is 0, which means not managed by the IPC and the quantum.
    for (int i = 0; i < max_cores; ++i) {
        core_info_table[i].host_core_idx = i;
        core_info_table[i].ipns = 0.0;
        core_info_table[i].bx_instruction_coeff = 1.0;
        core_info_table[i].bx_instruction_access_coeff = 0.0;
        core_info_table[i].bx_data_access_coeff = 0.0;
        core_info_table[i].bx_private_icache_miss_coeff = 0.0;
        core_info_table[i].bx_private_dcache_miss_coeff = 0.0;
        core_info_table[i].bx_shared_cache_miss_coeff = 0.0;
        core_info_table[i].bx_branch_count_coeff = 0.0;
        core_info_table[i].bx_bp_miss_coeff = 0.0;
        core_info_table[i].bx_tlb_miss_coeff = 0.0;
    }

    // Load the IPC from the file.
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        return;  // File not found, use defaults
    }

    char line[1024];
    int core_id = 0;

    // Read and validate header
    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "Error: Empty core_info.csv file\n");
        fclose(fp);
        exit(1);
    }

    // Check for old format (2 columns)
    if (strstr(line, "ipns") != NULL && strstr(line, "affinity_core_idx") != NULL) {
        fprintf(stderr, "Error: core_info.csv uses deprecated 2-column format.\n");
        fprintf(stderr, "Please migrate to new 12-column format using:\n");
        fprintf(stderr, "  python migrate_core_info.py <input> <output>\n");
        fclose(fp);
        exit(2);
    }

    // Check for new format header
    if (strstr(line, "host_core_idx") == NULL ||
        strstr(line, "model_type") == NULL ||
        strstr(line, "bx_instruction_coeff") == NULL) {
        fprintf(stderr, "Error: Invalid core_info.csv header. Expected 12-column format.\n");
        fclose(fp);
        exit(1);
    }

    // Parse data rows
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Skip empty lines and comments
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\0') {
            continue;
        }

        // Trim trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        char *saveptr = NULL;
        char *token = strtok_r(line, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse host_core_idx\n", core_id);
            fclose(fp);
            exit(1);
        }
        core_info_table[core_id].host_core_idx = strtoll(token, NULL, 10);

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse model_type\n", core_id);
            fclose(fp);
            exit(1);
        }

        // Trim whitespace from model_type
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';

        bool is_constant_model = false;
        if (strcmp(token, "constant") == 0) {
            is_constant_model = true;
        } else if (strcmp(token, "ipc-model") == 0) {
            is_constant_model = false;
        } else {
            fprintf(stderr, "Error: Row %d: model_type must be 'constant' or 'ipc-model', got '%s'\n",
                    core_id, token);
            fclose(fp);
            exit(1);
        }

        token = strtok_r(NULL, ",", &saveptr);
        if (!token) {
            fprintf(stderr, "Error: Row %d: failed to parse ipns\n", core_id);
            fclose(fp);
            exit(1);
        }
        double ipns = strtod(token, NULL);
        if (ipns < 0) {
            fprintf(stderr, "Error: Row %d: ipns must be non-negative, got %f\n", core_id, ipns);
            fclose(fp);
            exit(1);
        }
        core_info_table[core_id].ipns = ipns;

        // For constant model, coefficients are fixed
        if (is_constant_model) {
            core_info_table[core_id].bx_instruction_coeff = 1.0;
            core_info_table[core_id].bx_instruction_access_coeff = 0.0;
            core_info_table[core_id].bx_data_access_coeff = 0.0;
            core_info_table[core_id].bx_private_icache_miss_coeff = 0.0;
            core_info_table[core_id].bx_private_dcache_miss_coeff = 0.0;
            core_info_table[core_id].bx_shared_cache_miss_coeff = 0.0;
            core_info_table[core_id].bx_branch_count_coeff = 0.0;
            core_info_table[core_id].bx_bp_miss_coeff = 0.0;
            core_info_table[core_id].bx_tlb_miss_coeff = 0.0;

            // For constant model, we use ipns from CSV
            // Just verify it's positive
            if (ipns <= 0) {
                fprintf(stderr, "Error: Row %d: constant model requires positive ipns, got %f\n",
                        core_id, ipns);
                fclose(fp);
                exit(1);
            }
        } else {
            // IPC model: validate ipns == 1.0
            if (fabs(ipns - 1.0) > 1e-9) {
                fprintf(stderr, "Error: Row %d: ipc-model requires ipns=1.0, got %f\n",
                        core_id, ipns);
                fclose(fp);
                exit(1);
            }

            // Parse all coefficients
            const char *coeff_names[] = {
                "bx_instruction_coeff", "bx_instruction_access_coeff", "bx_data_access_coeff",
                "bx_private_icache_miss_coeff", "bx_private_dcache_miss_coeff",
                "bx_shared_cache_miss_coeff", "bx_branch_count_coeff",
                "bx_bp_miss_coeff", "bx_tlb_miss_coeff"
            };
            double *coeffs[] = {
                &core_info_table[core_id].bx_instruction_coeff,
                &core_info_table[core_id].bx_instruction_access_coeff,
                &core_info_table[core_id].bx_data_access_coeff,
                &core_info_table[core_id].bx_private_icache_miss_coeff,
                &core_info_table[core_id].bx_private_dcache_miss_coeff,
                &core_info_table[core_id].bx_shared_cache_miss_coeff,
                &core_info_table[core_id].bx_branch_count_coeff,
                &core_info_table[core_id].bx_bp_miss_coeff,
                &core_info_table[core_id].bx_tlb_miss_coeff
            };

            for (int i = 0; i < 9; i++) {
                token = strtok_r(NULL, ",", &saveptr);
                if (!token) {
                    fprintf(stderr, "Error: Row %d: failed to parse %s\n",
                            core_id, coeff_names[i]);
                    fclose(fp);
                    exit(1);
                }
                double val = strtod(token, NULL);
                if (val < 0) {
                    fprintf(stderr, "Error: Row %d: %s must be non-negative, got %f\n",
                            core_id, coeff_names[i], val);
                    fclose(fp);
                    exit(1);
                }
                *coeffs[i] = val;
            }
        }

        core_id += 1;
        if (core_id >= max_cores) {
            fprintf(stderr, "Error: Too many cores in core_info.csv (max %d)\n", max_cores);
            fclose(fp);
            exit(1);
        }
    }

    fclose(fp);
}
