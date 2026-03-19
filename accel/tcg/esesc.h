/*
 * ESESC mode — two-stage quantum scheduling helper.
 *
 * ESESC alternates between two per-CPU stages:
 *   normal  – uses the existing IPC model (bx_* coefficients) for time
 *             deduction.  Lasts at most 100 µs of simulated time.
 *   follow  – uses a constant IPNS derived from the preceding normal stage.
 *             Ends when a checkpoint generation request arrives
 *             (pf_periodic_check_cb returns true).
 *
 * All three entry points are no-ops when quantum_esesc_enabled() is false,
 * so callers do not need their own guard checks.
 */

#ifndef TCG_ESESC_H
#define TCG_ESESC_H

#include "hw/core/cpu.h"

/*
 * esesc_init_cpu - initialise ESESC state for a newly started CPU thread.
 *
 * Must be called after cpu->quantum_generation has been set (MT mode: after
 * dynamic_barrier_polling_increase_by_1; RR mode: after the initial
 * quantum_generation = 0 assignment).
 */
void esesc_init_cpu(CPUState *cpu);

/*
 * esesc_check_cpu_stage_transition - check whether this CPU should switch
 * from normal to follow mode.
 *
 * Must be called *after* cpu->quantum_generation has been incremented for
 * the current quantum boundary, and *before* the budget is replenished.
 * Only transitions normal → follow; the reverse is handled by
 * esesc_reset_all_cpus_to_normal().
 */
void esesc_check_cpu_stage_transition(CPUState *cpu);

/*
 * esesc_reset_all_cpus_to_normal - transition all CPUs from follow → normal.
 *
 * Called when pf_periodic_check_cb returns true (checkpoint boundary).
 * Safe to call from the barrier's last-thread path or from the RR loop.
 */
void esesc_reset_all_cpus_to_normal(void);

#endif /* TCG_ESESC_H */
