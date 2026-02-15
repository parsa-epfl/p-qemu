/*
 * QEMU TCG Multi Threaded vCPUs implementation
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
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "qemu/plugin-pf.h"
#include "qemu/typedefs.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "exec/exec-all.h"
#include "hw/boards.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-quantum.h"
#include "qemu/log-for-trace.h"

#include "qemu/dynamic_barrier.h"
#include "sysemu/quantum.h"
#include <bits/time.h>
#include <stdio.h>

// const uint64_t QUANTUM_SIZE = 1000000; // 1M
dynamic_barrier_polling_t quantum_barrier;

typedef struct core_meta_info_t {
    uint64_t ip100ns;
    uint64_t affinity_core_idx;
} core_meta_info_t;

static core_meta_info_t core_info_table[256];

void quantum_initialize_core_info_table(const char *file_name) {
    // By default, all cores' IPC is 0, which means not managed by the IPC and the quantum.
    for(uint64_t i = 0; i < 256; ++i) {
        core_info_table[i].ip100ns = 0;
        core_info_table[i].affinity_core_idx = i;
    }

    // Load the IPC from the file. Each line is a integer and suggests the IPC.
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        if (quantum_enabled()) {
            // we don't do anything if the file is not found.
            printf("IPC file (%s) is not found. It is needed under the quantum mode.\n", file_name);
            exit(1);
        } else {
            return;
        }

    }

    char line[1024];
    int core_id = 0;


    // The first line is the header.
    // The header is "ipc,affinity_core_idx"
    // do a comparison with the header.
    assert(fgets(line, 1024, fp) != NULL);
    bool parse_affinity = false;
    if (strcmp(line, "ipns,affinity_core_idx\n") == 0) {
        parse_affinity = true;
    } else if (strcmp(line, "ipns\n") == 0) {
        parse_affinity = false;
    } else {
        printf("File %s is not valid for setting up the IPNS for quantum.\n", file_name);
        abort();
    }

    // Now, read every line and fill the structure.
    while(fgets(line, 1024, fp) != NULL) {
        if (parse_affinity) {
            char *token = strtok(line, ",");
            double ipns = strtod(token, NULL);
            core_info_table[core_id].ip100ns = (uint64_t)(ipns * 100);
            assert(core_info_table[core_id].ip100ns > 0 && "IPNS should be greater than 0");
            token = strtok(NULL, ",");
            core_info_table[core_id].affinity_core_idx = atoi(token);
        } else {
            double ipns = strtod(line, NULL);
            core_info_table[core_id].ip100ns = (uint64_t)(ipns * 100);
            assert(core_info_table[core_id].ip100ns > 0 && "IPNS should be greater than 0");
            core_info_table[core_id].affinity_core_idx = core_id;
        }

        core_id += 1;
    }

    fclose(fp);
}


typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

typedef struct {
    uint64_t total_time;
    uint64_t execution_time;
    uint64_t waiting_time;
    uint64_t idle_time;
    uint64_t peeking_other_time;
} per_cpu_host_time_breakdown_t;

const uint64_t RECORD_SIZE = 4096;
const uint64_t SKIP_SIZE = 1000;

static void dump_log(CPUState *cpu, per_cpu_host_time_breakdown_t *statistics) {
    // open a log file.
    char log_name[100];
    snprintf(log_name, 100, "qlog/statisitcs_%u.log", cpu->cpu_index);

    FILE *fp = fopen(log_name, "w");

    // push the enter_idle_time, target_cycle_on_idle, and target_cycle_on_instruction to the statistics.
    fprintf(
        fp,
        "%lu,%lu,%lu\n",
        cpu->enter_idle_time,
        cpu->target_cycle_on_idle,
        cpu->target_cycle_on_instruction
    );

    for(uint64_t i = 0; i < RECORD_SIZE; ++i) {
        fprintf(
            fp,
            "%lu,%lu,%lu,%lu,%lu\n",
            statistics[i].total_time,
            statistics[i].execution_time,
            statistics[i].waiting_time,
            statistics[i].idle_time,
            statistics[i].peeking_other_time
        );
    }

    fclose(fp);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    cpu->ip100ns = core_info_table[cpu->cpu_index].ip100ns;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    // per_cpu_host_time_breakdown_t statistics[RECORD_SIZE];
    // bzero(statistics, sizeof(per_cpu_host_time_breakdown_t) * RECORD_SIZE);
    // uint64_t statistic_head_counter = 0;

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;

    // initialize the field that are related to the time.
    cpu->unknown_time = 0;
    cpu->enter_idle_time = 0;
    cpu->target_cycle_on_idle = 0;
    cpu->target_cycle_on_instruction = 0;

    cpu->sgi_sender_time_ns_valid = false;
    cpu->sgi_sender_remaining_time_ns = 0;
    cpu->sgi_sender_quantum_generation = 0;

    cpu->whether_spinning_on_quantum = false;
    cpu->wakeup_during_quantum_spinning = 0;
    cpu->wakeup_while_given_ts_is_smaller_than_before = 0;

    bool affiliated_with_quantum = cpu->ip100ns && quantum_enabled();
    assert(affiliated_with_quantum);

    cpu->quantum_generation = dynamic_barrier_polling_increase_by_1(&quantum_barrier);
    cpu->quantum_budget = (quantum_size * cpu->ip100ns) / 100;
    assert(cpu->quantum_budget > 0);
    cpu->quantum_required = 0;
    cpu->quantum_budget_depleted = 0;


    qemu_log("Core%u Quantum Count: %lu ns, %lu instructions \n", cpu->cpu_index, quantum_size, quantum_size * cpu->ip100ns / 100);

    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    assert(cpu->cpu_index < 128);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_info_table[cpu->cpu_index].affinity_core_idx, &cpuset);
    int res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    assert(res == 0 && "Failed to set thread affinity");

    /* process any pending work */
    cpu->exit_request = 1;

    // uint64_t dumping_threshold = 300 * 1000 * 1000; // 300M

    // uint64_t ts0 = get_current_timestamp_ns();
    do {
        if (cpu_can_run(cpu)) {
            int r;
            qemu_mutex_unlock_iothread();
continue_to_run:
            r = tcg_cpus_exec(cpu);
            // check the quantum budget and sync before doing I/O operation.
            if (cpu->quantum_budget_depleted) {
                cpu->quantum_budget_depleted = false;
                while (cpu->quantum_budget <= 0) {
                    uint64_t old_generation = cpu->quantum_generation;
                    uint32_t old_generation_low_32bit = old_generation & 0xFFFFFFFF;
                    cpu_virtual_time[cpu->cpu_index].next_deadline_in_ns = -1;
                    int stop_request = 0;

                    cpu->whether_spinning_on_quantum = true;

                    uint32_t new_generation = dynamic_barrier_polling_wait(
                        &quantum_barrier,
                        cpu->quantum_generation,
                        &stop_request
                    );

                    cpu->whether_spinning_on_quantum = false;

                    if (stop_request != 2) {
                        assert(new_generation == old_generation_low_32bit + 1);
                        cpu->quantum_budget += (quantum_size * cpu->ip100ns) / 100;
                        cpu->quantum_generation += 1;
                    } else {
                        // this means the vCPU quits due to the machine state change.
                        assert(new_generation == old_generation_low_32bit);
                        // clean the budget.
                        cpu->quantum_budget = 0;
                        cpu->quantum_budget_depleted = 1;
                        assert(cpu->stop || cpu->stopped || !runstate_is_running());
                        // printf("[%s:%d] CPU %d quits due to the machine state change. stop_request: %d, stopped: %d \n", __FILE__, __LINE__, cpu->cpu_index, cpu->stop, cpu->stopped);
                    }

                    if (stop_request) {
                        break;
                    }
                }

                if (r == EXCP_QUANTUM && !cpu->exit_request) {
                    goto continue_to_run;
                }

                if (r == EXCP_HLT) {
                    // this is WFI.
                    // In this case, we should directly wait for the quantum barrier, without getting the iothread.
                }
            }
            qemu_mutex_lock_iothread();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                // Well, it is possible that this atomic step may deplete the quantum budget.
                // What we have to do now is to give enough quantum budget to this CPU, and remove it afterwards.
                int64_t quantum_for_deduction = cpu->quantum_required;
                // We need to sync immediately to get the quantum budget.
                while (cpu->quantum_budget <= quantum_for_deduction) {
                    uint64_t old_generation = cpu->quantum_generation;
                    uint32_t old_generation_low_32bit = old_generation & 0xFFFFFFFF;
                    cpu_virtual_time[cpu->cpu_index].next_deadline_in_ns = -1;
                    int stop_request = 0;

                    cpu->whether_spinning_on_quantum = true;

                    uint32_t new_generation = dynamic_barrier_polling_wait(
                        &quantum_barrier,
                        cpu->quantum_generation,
                        &stop_request
                    );

                    cpu->whether_spinning_on_quantum = false;

                    if (stop_request != 2) {
                        assert(new_generation == old_generation_low_32bit + 1);
                        cpu->quantum_budget += (quantum_size * cpu->ip100ns) / 100;
                        cpu->quantum_generation += 1;
                    } else {
                        // this means the vCPU quits due to the machine state change.
                        assert(new_generation == old_generation_low_32bit);
                        // clean the budget.
                        cpu->quantum_budget = 0;
                        cpu->quantum_budget_depleted = 1;
                        assert(cpu->stop || cpu->stopped || !runstate_is_running());
                    }

                    if (stop_request) {
                        break;
                    }
                }
                assert(cpu->quantum_budget_depleted == false);
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu); // This function will not decouple the thread from the barrier anymore.
        qemu_mutex_unlock_iothread();

        // it is possible that the quantum budget is depleted due to the idle state.
        if (cpu->quantum_budget_depleted && !cpu->stopped) {
            cpu->quantum_budget_depleted = false;
            do {
                uint64_t old_generation = cpu->quantum_generation;
                uint32_t old_generation_low_32bit = old_generation & 0xFFFFFFFF;
                cpu_virtual_time[cpu->cpu_index].next_deadline_in_ns = -1;
                int stop_request = 0;

                // before going to sleep, I need to reset the sgi wakeup time so that others can pass the time.
                cpu->sgi_sender_time_ns_valid = false;
                cpu->whether_spinning_on_quantum = true;

                uint32_t new_generation = dynamic_barrier_polling_wait(
                    &quantum_barrier,
                    cpu->quantum_generation,
                    &stop_request
                );

                cpu->whether_spinning_on_quantum = false;

                if (stop_request != 2) {
                    if (new_generation == old_generation_low_32bit) {
                        // The CPU thread is waken up in the middle of the quantum
                        assert(quantum_allow_interrupt_wakeup_inside);
                        break;
                    }

                    assert(new_generation == old_generation_low_32bit + 1);

                    if (cpu->quantum_budget > 0) {
                        // this means the last quantum is not completely depleted. We need to deplete it before moving forward.
                        cpu->quantum_budget = 0;
                    }

                    cpu->quantum_budget += (quantum_size * cpu->ip100ns) / 100;
                    cpu->quantum_generation += 1;
                } else {
                    // this means the vCPU quits due to the machine state change.
                    assert(new_generation == old_generation_low_32bit);
                    // clean the budget.
                    cpu->quantum_budget = 0;
                    cpu->quantum_budget_depleted = 1;
                    assert(cpu->stop || cpu->stopped || !runstate_is_running());
                    // printf("[%s:%d] CPU %d quits due to the machine state change. stop_request: %d, stopped: %d \n", __FILE__, __LINE__, cpu->cpu_index, cpu->stop, cpu->stopped);
                }

                cpu_virtual_time[cpu->cpu_index].vts += quantum_size;


                if (stop_request) {
                    break;
                }
            } while (cpu->quantum_budget <= 0);
        }

        qemu_mutex_lock_iothread();
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();

    // resign the current thread from the barrier.
    dynamic_barrier_polling_decrease_by_1(&quantum_barrier);

    // also, print the histogram.
    // open a log file.
    char log_name[100];
    snprintf(log_name, 100, "quantum_histogram_%d.log", cpu->cpu_index);

    FILE *fp = fopen(log_name, "w");
    print_histogram(quantum_barrier.histogram[cpu->cpu_index], fp);
    fclose(fp);

    return NULL;
}

void quantum_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
}

void quantum_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    cpu->thread = g_new0(QemuThread, 1);
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

void quantum_initialize_barrier(void) {
    dynamic_barrier_polling_init(&quantum_barrier, 0);
}
