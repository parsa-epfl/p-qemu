#include "qemu/dynamic_barrier.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/quantum.h"
#include "qemu/plugin-pf.h"



static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

static void *report_time_peridically(void *arg) {
    dynamic_barrier_polling_t *barrier = arg;
    while (1) {
        sleep(10);
        uint64_t total_diff = barrier->total_diff;
        uint64_t generation = barrier->return_value.two_32.generation;
        printf("Total time spent in the barrier: %lu ns, generation: %lu, normalized_diff: %lf\n", total_diff, generation, (double)total_diff / generation);
    }
    return NULL;
}

// create a timestamp of each thread.
// static __thread uint64_t thread_start_quantum_timestamp = 0;

// // Initialize the dynamic barrier
// int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold) {
//     int status;

//     status = pthread_mutex_init(&barrier->mutex, NULL);
//     if (status != 0) return status;

//     status = pthread_cond_init(&barrier->cond, NULL);
//     if (status != 0) {
//         pthread_mutex_destroy(&barrier->mutex);
//         return status;
//     }

//     barrier->threshold = initial_threshold;
//     barrier->count = 0;
//     barrier->generation = 0;

//     // start another thread to call report_time_peridically.

//     return 0;
// }

// // Destroy the dynamic barrier
// int dynamic_barrier_destroy(dynamic_barrier_t *barrier) {
//     pthread_mutex_destroy(&barrier->mutex);
//     pthread_cond_destroy(&barrier->cond);
//     return 0;
// }

// // Wait on the barrier
// int dynamic_barrier_wait(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);

//     int gen = barrier->generation;
//     barrier->count++;

//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     } else {
//         while (gen == barrier->generation) {
//             pthread_cond_wait(&barrier->cond, &barrier->mutex);
//         }
//     }

//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }



// int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     barrier->threshold = barrier->threshold + 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }

// int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     if (barrier->threshold <= 0) {
//         pthread_mutex_unlock(&barrier->mutex);
//         return -1;
//     }
//     barrier->threshold = barrier->threshold - 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }


int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold) {
    barrier->lock.next_ticket = 0;
    barrier->lock.now_serving = 0;

    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->return_value.two_32.generation = 0;
    barrier->return_value.two_32.stop_request = 0;

    if (quantum_enabled()) {
        // pthread_t tid;
        // pthread_create(&tid, NULL, report_time_peridically, barrier);
    }

    for (int i = 0; i < 128; i++) {
        barrier->histogram[i] = create_histogram(100, 1e5, 101e5);
    }

    barrier->current_cycle = 0;
    barrier->next_check_threshold = quantum_check_threshold;

    return 0;
}

int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier) {
    for (int i = 0; i < 128; i++) {
        free_histogram(barrier->histogram[i]);
    }

    return 0;
}

static void dynamic_barrier_polling_acquire_lock(dynamic_barrier_polling_t *barrier) {
    uint64_t my_ticket = atomic_fetch_add(&barrier->lock.next_ticket, 1);
    while (atomic_load(&barrier->lock.now_serving) != my_ticket) {
        // do nothing
    }
}

static void dynamic_barrier_polling_release_lock(dynamic_barrier_polling_t *barrier) {
    atomic_fetch_add(&barrier->lock.now_serving, 1);
}

uint32_t dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier, uint32_t private_generation, int *stop_request) {
    assert(current_cpu != NULL);

    dynamic_barrier_polling_acquire_lock(barrier);

    uint32_t current_gen = atomic_load(&barrier->return_value.two_32.generation);

    assert(private_generation == current_gen);

    uint64_t waiting_count = barrier->count;

    if (waiting_count == barrier->threshold - 1) {
        barrier->current_cycle += quantum_size;
        // barrier->stop_request = 0;
        bool broadcast_stop_request = 0;

        if(!runstate_is_running()) {
            // The machine is not running, so we can break.
            *stop_request = 2;
            dynamic_barrier_polling_release_lock(barrier);
            return current_gen; // abandon the current quantum.
        }

        barrier->count = 0;

        // Advance the virtual clock by the quantum size.

        increase_quantum_time();

        if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
            qemu_mutex_lock_iothread();
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
            qemu_mutex_unlock_iothread();
        }


        // Then, run the periodic check.
        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (pf_periodic_check_cb != NULL) {
                if(pf_periodic_check_cb(quantum_check_threshold)) {
                    broadcast_stop_request = 1;
                    // Notify the main loop for the incoming snapshot event.
                    qemu_notify_event();

                    // wait for the machine state to become suspended for VM.
                    // TODO check if we need to still do this in single node (+ checkpointing on single node in general)
                    // while (current_cpu->stop != true) {
                    //     sched_yield();
                    // }
                }
            }
            barrier->next_check_threshold += quantum_check_threshold;
        }

        barrier_result_t return_value;

        return_value.stop_request = broadcast_stop_request;
        return_value.generation = current_gen + 1;

        // cancel the sgi waking up request, because the thread is going to wake up.
        current_cpu->sgi_sender_time_ns_valid = false;

        // increase the generation and notify others.
        atomic_store(&barrier->return_value.one_64, *((uint64_t *)&return_value));

        // printf("CPU %d increase the quantum barrier generation. Current Quantum Generation: %u\n", current_cpu->cpu_index, current_gen + 1);

        dynamic_barrier_polling_release_lock(barrier); // we can release the generation here.

        if (broadcast_stop_request) {
            *stop_request = 1;
        } else {
            *stop_request = 0;
        }
    } else {
        barrier->count += 1;
        dynamic_barrier_polling_release_lock(barrier);

        barrier_result_t barrier_return_value;

        uint64_t spinning_count = 0;

        // You just need to wait.
        while (true) {
            *((uint64_t *)&barrier_return_value) = atomic_load(&barrier->return_value.one_64);

            if (barrier_return_value.generation != current_gen) {
                current_cpu->sgi_sender_time_ns_valid = false;
                break;
            }

            ++spinning_count;

            if (spinning_count % 1000000 == 0) {
                spinning_count = 0;
                // check the machine state.
                if (!runstate_is_running()) {
                    dynamic_barrier_polling_acquire_lock(barrier);
                    // The machine is not running, so we can break.
                    // Before breaking, we need to cancel the waiting count.
                    if (current_gen == barrier->return_value.two_32.generation) {
                        barrier->count -= 1;
                        assert(barrier->count < barrier->threshold);
                        *stop_request = 2;
                        dynamic_barrier_polling_release_lock(barrier);
                        return current_gen; // abandon the current quantum.
                    }

                    // this means the generation has been changed, so no need to decrease the count.
                    // There should be no pending waiters, because they should be confined by the qemu big lock.
                    assert(barrier->count == 0);
                    if (barrier->return_value.two_32.stop_request) {
                        *stop_request = 1;
                    } else {
                        *stop_request = 0;
                    }

                    dynamic_barrier_polling_release_lock(barrier);
                    return current_gen + 1; // abandon the current quantum.
                }
            }

            if (quantum_allow_interrupt_wakeup_inside) {
                // Now, we need to check whether the CPU has work to do

                // How many credits do I have?
                if (current_cpu->quantum_budget <= 0) {
                    // No need to continue.
                    continue;
                }

                if (cpu_thread_is_idle(current_cpu)) {
                    continue;
                }

                // Well, this means the CPU has work to do.
                // Grab the lock.
                dynamic_barrier_polling_acquire_lock(barrier);

                if (barrier->return_value.two_32.generation != current_gen) {
                    // The generation has changed, which mean the last quantum has been finished.
                    // This thread also needs to move to the next quantum.
                    dynamic_barrier_polling_release_lock(barrier);
                    break;
                }

                // Now, we need to detach from the barrier.
                assert(barrier->count > 0);
                barrier->count -= 1;

                // I need to go over all CPUs and understand what is their time.
                CPUState *cpu;
                double all_time = 0;
                uint64_t cpu_count = 0;
                CPU_FOREACH(cpu) {
                    if (cpu == current_cpu) continue;
                    if (cpu->whether_spinning_on_quantum && cpu->quantum_budget <= 0) {
                        // This CPU is not running, so we can skip it.
                        continue;
                    }


                    double this_cpu_time = cpu->quantum_budget * 100.0 / cpu->ip100ns;

                    if (this_cpu_time < 0) {
                        this_cpu_time = 0;
                    }

                    all_time += this_cpu_time;
                    cpu_count += 1;
                }

                // Based on the time, calculate the new budget.
                if (cpu_count > 0) {
                    double average_time = all_time / cpu_count;
                    // Now, we can calculate the new budget.
                    int64_t new_budget = (int64_t)(average_time * current_cpu->ip100ns / 100.0);
                    if (new_budget < 0) {
                        new_budget = 0;
                    }

                    if (new_budget != 0) {
                        current_cpu->wakeup_during_quantum_spinning += 1;
                        if (new_budget < current_cpu->quantum_budget) {
                            current_cpu->wakeup_while_given_ts_is_smaller_than_before += 1;
                        }
                    }

                    if (new_budget < current_cpu->quantum_budget) {
                        // This means the CPU has been sleeping for a long time.
                        current_cpu->quantum_budget = new_budget;
                    }
                } else {
                    current_cpu->quantum_budget = 0;
                }

                // As long as the budget is not depleted, we can continue to run.
                if (current_cpu->quantum_budget <= 0) {
                    // No need to continue.
                    dynamic_barrier_polling_release_lock(barrier);
                    continue;
                }


                //

                // if (current_cpu->sgi_sender_time_ns_valid) {
                //     // this means the CPU thread is waken up by a SGI. The source CPU has the time.
                //     uint64_t sender_time = current_cpu->sgi_sender_remaining_time_ns;
                //     uint64_t sender_generation = current_cpu->sgi_sender_quantum_generation;
                //     assert(sender_generation == current_gen);
                //     int64_t new_budget_on_acceptance = (sender_time * current_cpu->ip100ns) / 100;

                //     // update the budget if the new budget is smaller than the current budget, meaning that the sleeping has happened.
                //     if (new_budget_on_acceptance < current_cpu->quantum_budget) {
                //         current_cpu->quantum_budget = new_budget_on_acceptance;
                //     }

                //     // cleared, meaning that the time is updated and the thread is waken up.
                //     current_cpu->sgi_sender_time_ns_valid = false;
                // }

                // release the lock.
                dynamic_barrier_polling_release_lock(barrier);

                return current_gen;
            }

        }

        // read the stop request set by the last thread.
        if (barrier_return_value.stop_request) {
            *stop_request = 1;
        } else {
            *stop_request = 0;
        }

    }

    return current_gen + 1;
}

uint32_t dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier) {
    uint32_t current_generation;
    dynamic_barrier_polling_acquire_lock(barrier);
    current_generation = atomic_load(&barrier->return_value.two_32.generation);
    barrier->threshold += 1;
    dynamic_barrier_polling_release_lock(barrier);

    return current_generation;
}

int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    if (barrier->threshold <= 0) {
        dynamic_barrier_polling_release_lock(barrier);
        assert(false);
    }

    barrier->threshold -= 1;
    uint64_t waiting_count = barrier->count;

    if (waiting_count == barrier->threshold && waiting_count != 0) {
        barrier->current_cycle += quantum_size;

        if (barrier->next_check_threshold != 0 && barrier->current_cycle >= barrier->next_check_threshold) {
            if (pf_periodic_check_cb != NULL) pf_periodic_check_cb(quantum_check_threshold);
            barrier->next_check_threshold += quantum_check_threshold;
        }


        barrier->count = 0;

        // increase the generation and notify others.
        atomic_fetch_add(&barrier->return_value.two_32.generation, 1);
    }

    dynamic_barrier_polling_release_lock(barrier);
    return 0;
}

void dynamic_barrier_polling_reset(dynamic_barrier_polling_t *barrier) {
    dynamic_barrier_polling_acquire_lock(barrier);
    atomic_store(&barrier->return_value.two_32.generation, 0); // this should make everyone to not wait.
    barrier->count = 0;
    dynamic_barrier_polling_release_lock(barrier);
}
