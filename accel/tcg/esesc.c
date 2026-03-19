/*
 * ESESC mode — two-stage quantum scheduling implementation.
 *
 * See esesc.h for the interface description.
 */

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "sysemu/quantum.h"
#include "qemu/plugin-pf.h"

#include "esesc.h"

/*
 * Duration of the normal stage: 100 microseconds expressed in nanoseconds.
 * Declared as a file-scoped constant (not a macro) per project convention.
 */
static const uint64_t esesc_normal_duration_ns = 100000ULL;

void esesc_init_cpu(CPUState *cpu)
{
    if (!quantum_esesc_enabled()) {
        return;
    }

    cpu->esesc_in_follow_mode = false;
    cpu->esesc_normal_stage_start_generation = cpu->quantum_generation;
    cpu->esesc_normal_instructions = 0;
    cpu->esesc_derived_ip100ns = 0; /* 0 means "not yet derived"; fallback to ip100ns */
}

void esesc_check_cpu_stage_transition(CPUState *cpu)
{
    if (!quantum_esesc_enabled()) {
        return;
    }

    /* Only act when in normal mode. */
    if (!cpu->ip100ns || cpu->esesc_in_follow_mode) {
        return;
    }

    uint64_t elapsed_quanta = cpu->quantum_generation
                              - cpu->esesc_normal_stage_start_generation;
    uint64_t elapsed_ns = elapsed_quanta * quantum_size;

    if (elapsed_ns < esesc_normal_duration_ns) {
        return;
    }

    /*
     * Normal stage has run for >= 100 µs.  Derive the follow-mode IPNS.
     *
     * esesc_derived_ip100ns has units of "instructions per 100 ns"
     * (same unit as cpu->ip100ns = IPNS * 100).
     *
     *   esesc_derived_ip100ns = esesc_normal_instructions * 100 / elapsed_ns
     *
     * If no instructions were observed (e.g. the CPU was idle for the
     * entire normal stage), retain the value from the previous follow stage
     * so that the follow stage still has a sensible rate to work with.
     */
    if (cpu->esesc_normal_instructions > 0) {
        cpu->esesc_derived_ip100ns =
            cpu->esesc_normal_instructions * 100 / elapsed_ns;
    }
    /* else: esesc_derived_ip100ns keeps its previous value */

    cpu->esesc_in_follow_mode = true;
    cpu->esesc_normal_instructions = 0;

    /*
     * Discard plugin statistics that accumulated during the normal stage.
     * They are not consumed in follow mode, and must not be counted again
     * at the start of the next normal stage.
     */
    if (g_statistics_managed_by_plugin) {
        memset(&g_exposed_statistics[cpu->cpu_index], 0,
               sizeof(g_exposed_statistics[0]));
    }
}

void esesc_reset_all_cpus_to_normal(void)
{
    if (!quantum_esesc_enabled()) {
        return;
    }

    /*
     * Transition every managed CPU from follow → normal.
     *
     * esesc_derived_ip100ns is intentionally preserved so that the next
     * follow stage (after the upcoming normal stage) has a fallback value
     * if the normal stage yields zero instructions.
     *
     * Plugin statistics are left as-is; they will be discarded at the next
     * normal → follow transition in esesc_check_cpu_stage_transition().
     */
    CPUState *cpu;
    CPU_FOREACH(cpu) {
        if (!cpu->ip100ns) {
            continue;
        }
        cpu->esesc_in_follow_mode = false;
        cpu->esesc_normal_stage_start_generation = cpu->quantum_generation;
        cpu->esesc_normal_instructions = 0;
    }
}
