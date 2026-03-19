// This file is added by Cyan for the quantum mechanism.

#include "qemu/osdep.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"
#include "hw/core/cpu.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/quantum.h"
#include "sysemu/asid-coeff.h"
#include "qemu/plugin-pf.h"
#include "esesc.h"

/*
 * quantum_flush_current_stats - deduct accumulated stats into the quantum budget.
 *
 * In normal quantum mode (or ESESC normal stage), computes required_picoseconds
 * from the current plugin-exposed statistics using the core's bx_* coefficients,
 * zeroes the statistics, updates target_cycle_on_instruction and
 * cpu_virtual_time[].vts, and subtracts from quantum_budget_in_picosecond.
 *
 * In ESESC follow stage, uses a constant IPNS (esesc_derived_ip100ns) derived
 * from the preceding normal stage instead of the bx_* model.  Plugin statistics
 * are not touched in follow mode.
 *
 * Does NOT set quantum_budget_depleted — the caller is responsible for
 * checking the budget afterward.
 *
 * Safe to call from translated-code context (e.g. TTBR write handlers) where
 * current_cpu is valid.  Uses the passed @cpu parameter throughout.
 */
void quantum_flush_current_stats(CPUState *cpu)
{
    if (!quantum_enabled() || cpu->ip100ns == 0) {
        return;
    }

    uint64_t required_picoseconds = 0;

    if (quantum_esesc_enabled() && cpu->esesc_in_follow_mode) {
        /*
         * ESESC follow mode: use a constant IPNS derived from the preceding
         * normal stage.  Plugin statistics are intentionally ignored — they
         * are not reset here either; they will be discarded at the next
         * normal → follow transition.
         *
         * last_tb_instruction_count_for_quantum is always available and is
         * equivalent to the plugin's .instruction counter (confirmed).
         *
         * time_ps = instr_count * 100_000 / esesc_derived_ip100ns
         *
         * Derivation: ip100ns = instrs/100ns, so
         *   time_ns  = instr_count / (esesc_derived_ip100ns / 100)
         *   time_ps  = time_ns * 1000
         *            = instr_count * 100 * 1000 / esesc_derived_ip100ns
         *            = instr_count * 100000 / esesc_derived_ip100ns
         *
         * If esesc_derived_ip100ns is still 0 (before the first normal stage
         * completes), fall back to the base ip100ns so the CPU makes progress.
         */
        uint64_t instr_count = cpu->last_tb_instruction_count_for_quantum;
        uint64_t effective_ip100ns = cpu->esesc_derived_ip100ns
                                     ? cpu->esesc_derived_ip100ns
                                     : cpu->ip100ns;
        required_picoseconds = instr_count * 100000 / effective_ip100ns;

    } else if (g_statistics_managed_by_plugin) {
        /*
         * Normal mode (plain quantum or ESESC normal stage), plugin path:
         * use the bx_* IPC model.
         */
        struct qemu_plugin_exposed_statistics *this_core_info =
            &g_exposed_statistics[cpu->cpu_index];

        required_picoseconds += this_core_info->instruction * cpu->bx_instruction_coeff;
        required_picoseconds += this_core_info->instruction_access * cpu->bx_instruction_access_coeff;
        required_picoseconds += this_core_info->data_access * cpu->bx_data_access_coeff;
        required_picoseconds += this_core_info->private_icache_miss * cpu->bx_private_icache_miss_coeff;
        required_picoseconds += this_core_info->private_dcache_miss * cpu->bx_private_dcache_miss_coeff;
        required_picoseconds += this_core_info->shared_cache_miss * cpu->bx_shared_cache_miss_coeff;
        required_picoseconds += this_core_info->branch_count * cpu->bx_branch_count_coeff;
        required_picoseconds += this_core_info->bp_miss * cpu->bx_bp_miss_coeff;
        required_picoseconds += this_core_info->tlb_miss * cpu->bx_tlb_miss_coeff;

        /*
         * Accumulate instruction count for ESESC IPNS derivation.
         * Must be read before the memset below.
         */
        if (quantum_esesc_enabled()) {
            cpu->esesc_normal_instructions += this_core_info->instruction;
        }

        /* Reset statistics so they are not counted again. */
        memset(this_core_info, 0, sizeof(*this_core_info));
    } else {
        /*
         * Normal mode (plain quantum or ESESC normal stage), non-plugin path:
         * instructions counted directly.
         */
        required_picoseconds = cpu->last_tb_instruction_count_for_quantum * 1000;

        if (quantum_esesc_enabled()) {
            cpu->esesc_normal_instructions +=
                cpu->last_tb_instruction_count_for_quantum;
        }
    }

    cpu->last_tb_instruction_count_for_quantum = 0;

    /* Convert picoseconds to nanoseconds for target_cycle tracking. */
    cpu->target_cycle_on_instruction += required_picoseconds / 1000;

    /* Deduct from quantum budget (in picoseconds). */
    cpu->quantum_budget_in_picosecond -= required_picoseconds;

    /* Advance virtual time (vts is in nanoseconds). */
    uint64_t current_index = cpu->cpu_index;
    cpu_virtual_time[current_index].vts += required_picoseconds / 1000;
}

uint32_t HELPER(check_and_deduce_quantum)(CPUArchState *env) {
    assert(quantum_enabled());
    assert(current_cpu->env_ptr == env);

    if (current_cpu->ip100ns == 0) {
        // this is not a thread managed by quantum.
        return false;
    }

    /* Flush accumulated statistics and deduct from the quantum budget. */
    quantum_flush_current_stats(current_cpu);

    if (current_cpu->quantum_budget_in_picosecond <= 0) {
        current_cpu->quantum_budget_depleted = 1;
        return true;
    }
    return false;
}

void HELPER(set_instruction_count_for_quantum)(CPUArchState *env, uint32_t requirement) {
    assert(quantum_enabled());
    current_cpu->last_tb_instruction_count_for_quantum = requirement;
}

void HELPER(increase_target_cycle)(CPUArchState *env) {
    assert(icount_enabled());

    uint64_t current_index = current_cpu->cpu_index;
    cpu_virtual_time[current_index].vts += current_cpu->last_tb_instruction_count_for_quantum * 10000 / current_cpu->ip100ns;

    current_cpu->last_tb_instruction_count_for_quantum = 0;
}
