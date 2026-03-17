/*
 * QEMU TCG Quantum Round-Robin vCPUs implementation
 *
 * Single-threaded round-robin scheduling with quantum-based time management.
 * Combines the single-threaded execution model of RR mode with quantum
 * time synchronization from quantum mode, but without barriers.
 *
 * Copyright (c) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qemu/timer.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/quantum.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/plugin-pf.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-rr.h"
#include "tcg-accel-ops-quantum-rr.h"
#include "hw/core/cpu.h"


/* Current CPU being executed (for debugging/monitoring) */
static CPUState *quantum_rr_current_cpu;

/* Core info table for quantum-rr mode */
static core_meta_info_t quantum_rr_core_info_table[256];

void quantum_rr_initialize_core_info_table(const char *file_name) {
    tcg_parse_core_info_file(file_name, quantum_rr_core_info_table, 256);
}

/*
 * Kick all quantum-rr vCPUs
 *
 * In single-threaded mode, we must kick ALL CPUs because:
 * 1. Only one CPU runs at a time in the round-robin loop
 * 2. The caller specifies a target CPU, but that CPU may not be running
 * 3. We need to stop the currently executing CPU to handle the event
 * 4. By kicking all CPUs, we ensure the running CPU exits and the scheduler
 *    can process pending work for any CPU (interrupts, etc.)
 *
 * Parameter is 'unused' because we kick all CPUs regardless of which one
 * was requested, similar to the original RR mode implementation.
 */
void quantum_rr_kick_vcpu_thread(CPUState *unused)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_exit(cpu);
    };
}

/* Calculate the number of CPUs */
static int quantum_rr_cpu_count(void)
{
    static unsigned int last_gen_id = ~0;
    static int cpu_count;
    CPUState *cpu;

    QEMU_LOCK_GUARD(&qemu_cpu_list_lock);

    if (cpu_list_generation_id_get() != last_gen_id) {
        cpu_count = 0;
        CPU_FOREACH(cpu) {
            ++cpu_count;
        }
        last_gen_id = cpu_list_generation_id_get();
    }

    return cpu_count;
}

/* Wait for I/O events */
static void quantum_rr_wait_io_event(void)
{
    CPUState *cpu;

    while (all_cpu_threads_idle()) {
        // this means the main thread is stopping us and want to do something, so we should wait.
        qemu_cond_wait_iothread(first_cpu->halt_cond);
    }

    CPU_FOREACH(cpu) {
        qemu_wait_io_event_common(cpu);
    }
}

/* Handle unplugged CPUs */
static void quantum_rr_deal_with_unplugged_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (cpu->unplug && !cpu_can_run(cpu)) {
            tcg_cpus_destroy(cpu);
            break;
        }
    }
}

/* Synchronize all CPUs to the maximum virtual time */
static void quantum_rr_sync_virtual_time(int cpu_count)
{
    uint64_t max_vtime = 0;
    int i;

    /* Find maximum virtual time */
    for (i = 0; i < cpu_count; i++) {
        uint64_t vtime = cpu_virtual_time[i].vts;
        if (vtime > max_vtime) {
            max_vtime = vtime;
        }
    }

    /* Synchronize all CPUs to max vtime */
    for (i = 0; i < cpu_count; i++) {
        assert(cpu_virtual_time[i].vts <= max_vtime);
        cpu_virtual_time[i].vts = max_vtime;
    }
}

/* Replenish quantum budget for all CPUs */
static void quantum_rr_replenish_budgets(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        /* Calculate quantum budget: (quantum_size * ip100ns) / 100 * 1000 picoseconds */
        cpu->quantum_budget_in_picosecond =
            (quantum_size * cpu->ip100ns) / 100 * 1000;
        assert(cpu->quantum_budget_in_picosecond > 0);

        /* Reset depletion flag */
        cpu->quantum_budget_depleted = 0;
        cpu->last_tb_instruction_count_for_quantum = 0;
    }
}

/* Advance virtual time for a CPU by quantum_size */
static void quantum_rr_advance_vtime(CPUState *cpu)
{
    cpu_virtual_time[cpu->cpu_index].vts += quantum_size;
}

/* RCU force callback - ensures RCU can reclaim memory */
static void quantum_rr_force_rcu(Notifier *notify, void *data)
{
    /* In single-threaded mode, we can directly process RCU callbacks */
    /* No need to kick since we're the only thread */
}

/*
 * Main quantum-rr CPU thread function
 * Single-threaded round-robin with quantum time management
 */
static void *quantum_rr_cpu_thread_fn(void *arg)
{
    Notifier force_rcu;
    CPUState *cpu = arg;
    int cpu_count;
    uint64_t cycle = 0;
    uint64_t next_check_threshold = quantum_check_threshold;

    assert(tcg_enabled());
    rcu_register_thread();
    force_rcu.notify = quantum_rr_force_rcu;
    rcu_add_force_rcu_notifier(&force_rcu);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* Wait for initial kick-off after machine start */
    while (first_cpu->stopped) {
        qemu_cond_wait_iothread(first_cpu->halt_cond);

        /* Process any pending work and initialize CPUs */
        CPU_FOREACH(cpu) {
            current_cpu = cpu;
            qemu_wait_io_event_common(cpu);

            /* Set up IPC values from core info table */
            cpu->ip100ns = (uint64_t)(quantum_rr_core_info_table[cpu->cpu_index].ipns * 100);
            assert(cpu->ip100ns > 0 && "CPU must have valid IPC from core_info.csv");

            cpu->bx_instruction_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_instruction_coeff;
            cpu->bx_instruction_access_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_instruction_access_coeff;
            cpu->bx_data_access_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_data_access_coeff;
            cpu->bx_private_icache_miss_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_private_icache_miss_coeff;
            cpu->bx_private_dcache_miss_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_private_dcache_miss_coeff;
            cpu->bx_shared_cache_miss_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_shared_cache_miss_coeff;
            cpu->bx_branch_count_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_branch_count_coeff;
            cpu->bx_bp_miss_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_bp_miss_coeff;
            cpu->bx_tlb_miss_coeff = quantum_rr_core_info_table[cpu->cpu_index].bx_tlb_miss_coeff;

            cpu->is_ipc_model = !core_model_is_constant(&quantum_rr_core_info_table[cpu->cpu_index]);

            cpu->default_bx_instruction_coeff         = cpu->bx_instruction_coeff;
            cpu->default_bx_instruction_access_coeff  = cpu->bx_instruction_access_coeff;
            cpu->default_bx_data_access_coeff         = cpu->bx_data_access_coeff;
            cpu->default_bx_private_icache_miss_coeff = cpu->bx_private_icache_miss_coeff;
            cpu->default_bx_private_dcache_miss_coeff = cpu->bx_private_dcache_miss_coeff;
            cpu->default_bx_shared_cache_miss_coeff   = cpu->bx_shared_cache_miss_coeff;
            cpu->default_bx_branch_count_coeff        = cpu->bx_branch_count_coeff;
            cpu->default_bx_bp_miss_coeff             = cpu->bx_bp_miss_coeff;
            cpu->default_bx_tlb_miss_coeff            = cpu->bx_tlb_miss_coeff;

            /* Initialize quantum tracking */
            cpu->last_tb_instruction_count_for_quantum = 0;
            cpu->quantum_generation = 0;
            cpu->quantum_budget_depleted = 0;
        }
    }

    cpu = first_cpu;
    cpu->exit_request = 1;

    while (1) {
        /* Replenish quantum budgets at start of each round */
        quantum_rr_replenish_budgets();
        cpu_count = quantum_rr_cpu_count();

        /* Execute all CPUs in round-robin order */
        CPU_FOREACH(cpu) {
            int r;

            /* Store current CPU for debugging/monitoring */
            qatomic_set_mb(&quantum_rr_current_cpu, cpu);
            current_cpu = cpu;

            qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                              (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

            if (cpu_can_run(cpu)) {
                qemu_mutex_unlock_iothread();
                r = tcg_cpus_exec(cpu);
                qemu_mutex_lock_iothread();

                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                    break;
                } else if (r == EXCP_ATOMIC) {
                    qemu_mutex_unlock_iothread();
                    cpu_exec_step_atomic(cpu);
                    qemu_mutex_lock_iothread();
                    break;
                }
            } else {
                /* CPU cannot run (halted/stopped) - still advance time */
                quantum_rr_advance_vtime(cpu);
            }

            /* Clear exit request if set */
            if (cpu->exit_request) {
                qatomic_set_mb(&cpu->exit_request, 0);
            }
        }

        /* Clear current CPU indicator */
        qatomic_set(&quantum_rr_current_cpu, NULL);

        /* Handle timers */
        increase_quantum_time();
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                      QEMU_TIMER_ATTR_ALL);

        if (deadline == 0) {
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        }

        /* Track cycles for periodic checks */
        cycle += quantum_size;
        if (quantum_check_threshold != 0 && cycle >= next_check_threshold) {
            if (pf_periodic_check_cb) {
                if (pf_periodic_check_cb(quantum_check_threshold)) {
                    qemu_notify_event();
                    qemu_mutex_unlock_iothread();
                    while (!first_cpu->stop) {
                        sched_yield();
                    }
                    qemu_mutex_lock_iothread();
                }
            }
            next_check_threshold += quantum_check_threshold;
        }

        /* Synchronize virtual time across all CPUs */
        quantum_rr_sync_virtual_time(cpu_count);

        /* Check if all CPUs are idle - notify main loop to prevent deadlock */
        if (all_cpu_threads_idle()) {
            /*
             * When all cpus are sleeping (e.g in WFI), to avoid a deadlock
             * in the main_loop, wake it up in order to start the warp timer.
             */
            qemu_notify_event();
        }

        /* Handle I/O events */
        quantum_rr_wait_io_event();

        /* Handle unplugged CPUs */
        quantum_rr_deal_with_unplugged_cpus();
    }

    rcu_remove_force_rcu_notifier(&force_rcu);
    rcu_unregister_thread();
    return NULL;
}

void quantum_rr_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    static QemuCond *single_tcg_halt_cond;
    static QemuThread *single_tcg_cpu_thread;

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, false);

    if (!single_tcg_cpu_thread) {
        cpu->thread = g_new0(QemuThread, 1);
        cpu->halt_cond = g_new0(QemuCond, 1);
        qemu_cond_init(cpu->halt_cond);

        /* share a single thread for all cpus with TCG */
        snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "ALL CPUs/TCG");
        qemu_thread_create(cpu->thread, thread_name,
                           quantum_rr_cpu_thread_fn,
                           cpu, QEMU_THREAD_JOINABLE);

        single_tcg_halt_cond = cpu->halt_cond;
        single_tcg_cpu_thread = cpu->thread;
    } else {
        /* we share the thread */
        cpu->thread = single_tcg_cpu_thread;
        cpu->halt_cond = single_tcg_halt_cond;
        cpu->thread_id = first_cpu->thread_id;
        cpu->can_do_io = 1;
        cpu->created = true;
    }
}

void quantum_rr_initialize(void)
{
    /* Initialize core info table */
    quantum_rr_initialize_core_info_table("core_info.csv");
}
