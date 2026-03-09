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
#include "qemu/plugin-pf.h"

uint32_t HELPER(check_and_deduce_quantum)(CPUArchState *env) {
    assert(quantum_enabled());
    assert(current_cpu->env_ptr == env);

    if (current_cpu->ip100ns == 0) {
        // this is not a thread managed by quantum.
        return false;
    }

    // OK, now let's calculate the target time we should deduce (in picoseconds).
    // Coefficients are in fixed-point (value * 1000), so stat * coeff gives picoseconds directly.
    uint64_t required_picoseconds = 0;

    if (g_statistics_managed_by_plugin) {
        struct qemu_plugin_exposed_statistics *this_core_info = &g_exposed_statistics[current_cpu->cpu_index];

        required_picoseconds += this_core_info->instruction * current_cpu->bx_instruction_coeff;
        required_picoseconds += this_core_info->instruction_access * current_cpu->bx_instruction_access_coeff;
        required_picoseconds += this_core_info->data_access * current_cpu->bx_data_access_coeff;
        required_picoseconds += this_core_info->private_icache_miss * current_cpu->bx_private_icache_miss_coeff;
        required_picoseconds += this_core_info->private_dcache_miss * current_cpu->bx_private_dcache_miss_coeff;
        required_picoseconds += this_core_info->shared_cache_miss * current_cpu->bx_shared_cache_miss_coeff;
        required_picoseconds += this_core_info->branch_count * current_cpu->bx_branch_count_coeff;
        required_picoseconds += this_core_info->bp_miss * current_cpu->bx_bp_miss_coeff;
        required_picoseconds += this_core_info->tlb_miss * current_cpu->bx_tlb_miss_coeff;

        // clean statistics.
        memset(this_core_info, 0, sizeof(*this_core_info));
    } else {
        // For non-plugin mode: instructions are counted directly, convert to picoseconds
        required_picoseconds = current_cpu->last_tb_instruction_count_for_quantum * 1000;
    }

    current_cpu->last_tb_instruction_count_for_quantum = 0;


    // Convert picoseconds to nanoseconds for target_cycle tracking (divide by 1000)
    current_cpu->target_cycle_on_instruction += required_picoseconds / 1000;

    // deduction (budget is in picoseconds).
    current_cpu->quantum_budget_in_picosecond -= required_picoseconds;

    // increase the target cycle (vts is in nanoseconds).
    uint64_t current_index = current_cpu->cpu_index;
    cpu_virtual_time[current_index].vts += required_picoseconds / 1000;

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
